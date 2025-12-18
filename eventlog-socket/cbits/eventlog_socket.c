// For POLLRDHUP
#define _GNU_SOURCE

#include <assert.h>
#include <stdbool.h>
#include <poll.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netdb.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>

#include <Rts.h>
#include <rts/prof/Heap.h>

#include "eventlog_socket.h"

#define LISTEN_BACKLOG 5
#define POLL_LISTEN_TIMEOUT 10000
#define POLL_WRITE_TIMEOUT 1000
#define CONTROL_MAGIC "GCTL"
#define CONTROL_MAGIC_LEN 4

enum control_command {
  CONTROL_CMD_START_HEAP_PROFILING = 0x01,
  CONTROL_CMD_STOP_HEAP_PROFILING = 0x02,
  CONTROL_CMD_REQUEST_HEAP_PROFILE = 0x03,
};

#ifndef POLLRDHUP
#define POLLRDHUP POLLHUP
#endif

// logging helper macros:
// - use PRINT_ERR to unconditionally log erroneous situations
// - otherwise use DEBUG_ERR
#define PRINT_ERR(...) fprintf(stderr, "ghc-eventlog-socket: " __VA_ARGS__)
#ifdef DEBUG
#define DEBUG_ERR(fmt, ...) fprintf(stderr, "ghc-eventlog-socket %s: " fmt, __func__, __VA_ARGS__)
#define DEBUG0_ERR(fmt) fprintf(stderr, "ghc-eventlog-socket %s: " fmt, __func__)
#else
#define DEBUG_ERR(fmt, ...)
#define DEBUG0_ERR(fmt)
#endif

struct write_buffer;
void write_buffer_free(struct write_buffer *buf);
static void start_control_receiver(int fd);
static void *control_receiver(void *arg);
static void control_connection_closed(int fd);
// control channel read status: keeps writer socket alive as long as we can
// still read from the peer, even if the payload is malformed.
enum control_recv_status {
  CONTROL_RECV_OK,
  CONTROL_RECV_PROTOCOL_ERROR,
  CONTROL_RECV_DISCONNECTED,
};
static enum control_recv_status control_receive_command(int fd, enum control_command *cmd_out);
static void handle_control_command(enum control_command cmd);

/*********************************************************************************
 * data definitions
 *********************************************************************************/


struct write_buffer_item {
  uint8_t *orig; // original data pointer (which we free)
  uint8_t *data;
  size_t size; // invariant: size is not zero
  struct write_buffer_item *next;
};

// invariant: head and last are both NULL or both not NULL.
struct write_buffer {
  struct write_buffer_item *head;
  struct write_buffer_item *last;
};

/*********************************************************************************
 * globals
 *********************************************************************************/

/* This module is concurrent.
 * There are three thread(group)s:
 * 1. RTS
 * 2. worker spawned by open_socket (this writes to the socket)
 * 3. listener spawned by start_control_reciever (this recieves messages on the socket)
 */

// variables read and written by worker only:
static bool initialized = false;
static int listen_fd = -1;
static const char *g_sock_path = NULL;
static int wake_pipe[2] = { -1, -1 };

enum listener_kind {
  LISTENER_UNIX,
  LISTENER_TCP,
};

struct listener_config {
  enum listener_kind kind;
  const char *sock_path;
  const char *tcp_host;
  const char *tcp_port;
};

static bool listener_config_valid(const struct listener_config *config) {
  if (config == NULL) {
    return false;
  }

  switch (config->kind) {
    case LISTENER_UNIX:
      return config->sock_path != NULL;
    case LISTENER_TCP:
      return config->tcp_port != NULL;
    default:
      return false;
  }
}

static void cleanup_socket(void) {
    if (g_sock_path) unlink(g_sock_path);
    if (wake_pipe[0] != -1) {
      close(wake_pipe[0]);
      wake_pipe[0] = -1;
    }
    if (wake_pipe[1] != -1) {
      close(wake_pipe[1]);
      wake_pipe[1] = -1;
    }
}

static void drain_worker_wake(void)
{
  if (wake_pipe[0] == -1)
    return;

  uint8_t buf[32];
  while (true) {
    ssize_t ret = read(wake_pipe[0], buf, sizeof(buf));
    if (ret > 0) {
      continue;
    } else if (ret == 0) {
      break;
    } else if (errno == EINTR) {
      continue;
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    } else {
      PRINT_ERR("failed to drain wake pipe: %s\n", strerror(errno));
      break;
    }
  }
}

static void wake_worker(void)
{
  if (wake_pipe[1] == -1)
    return;

  uint8_t byte = 1;
  ssize_t ret = write(wake_pipe[1], &byte, sizeof(byte));
  if (ret == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
    PRINT_ERR("failed to wake worker: %s\n", strerror(errno));
  }
}


// concurrency variables
static pthread_t listen_thread;
static pthread_cond_t new_conn_cond;
// Signal to the control thread to start.
static pthread_cond_t control_ready_cond;
// Whether we have started the control thread
static volatile bool control_ready = false;
// Whether the controller thread is ready to start
static bool control_ready_armed = false;
// Global mutex guarding all shared state between RTS threads, the worker thread,
// and the detached control receiver. Only client_fd and wt need protection, but
// using a single mutex ensures we keep their updates consistent.
static pthread_mutex_t mutex;

// variables accessed by multiple threads and guarded by mutex:
//  * client_fd: written by worker, writer_stop, and control receiver to signal
//    when a client connects/disconnects. The lock ensures the fd value does not
//    change while other threads inspect or write to it.
//  * wt: queue of pending eventlog chunks. RTS writers append while the worker
//    thread consumes; the lock ensures push/pop operations stay consistent.
//
// Note: RTS writes client_fd in writer_stop.
static volatile int client_fd = -1;
static struct write_buffer wt = {
  .head = NULL,
  .last = NULL,
};

/*********************************************************************************
 * control channel helpers (minimal protocol)
 *********************************************************************************/

static bool control_wait_for_data(int fd)
{
  struct pollfd pfd = {
    .fd = fd,
    .events = POLLIN | POLLRDHUP,
    .revents = 0,
  };

  int pret = poll(&pfd, 1, POLL_WRITE_TIMEOUT);
  if (pret == -1) {
    if (errno == EINTR)
      return true;
    PRINT_ERR("control poll() failed: %s\n", strerror(errno));
    return false;
  }
  if (pret == 0)
    return true; // timeout, simply retry

  if (pfd.revents & POLLRDHUP)
    return false;

  return true;
}

static enum control_recv_status control_read_exact(int fd, uint8_t *buf, size_t len)
{
  size_t have = 0;
  while (have < len) {
    ssize_t got = recv(fd, buf + have, len - have, 0);
    DEBUG_ERR("control_read_exact %zd/%zu\n", got, len);
    if (got == 0)
      return CONTROL_RECV_DISCONNECTED;
    if (got < 0) {
      if (errno == EINTR)
        continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        if (!control_wait_for_data(fd))
          return CONTROL_RECV_DISCONNECTED;
        continue;
      }
      PRINT_ERR("control recv() failed: %s\n", strerror(errno));
      return CONTROL_RECV_DISCONNECTED;
    }
    have += (size_t)got;
  }
  return CONTROL_RECV_OK;
}

static enum control_recv_status control_receive_command(int fd, enum control_command *cmd_out)
{
  uint8_t header[CONTROL_MAGIC_LEN];
  enum control_recv_status status =
      control_read_exact(fd, header, CONTROL_MAGIC_LEN);
  if (status != CONTROL_RECV_OK)
    return status;
  if (memcmp(header, CONTROL_MAGIC, CONTROL_MAGIC_LEN) != 0) {
    DEBUG_ERR("invalid control magic: %02x %02x %02x %02x\n",
              header[0], header[1], header[2], header[3]);
    return CONTROL_RECV_PROTOCOL_ERROR;
  }
  uint8_t cmd_id = 0;
  status = control_read_exact(fd, &cmd_id, 1);
  if (status != CONTROL_RECV_OK)
    return status;

  switch ((enum control_command) cmd_id) {
    case CONTROL_CMD_START_HEAP_PROFILING:
    case CONTROL_CMD_STOP_HEAP_PROFILING:
    case CONTROL_CMD_REQUEST_HEAP_PROFILE:
      *cmd_out = cmd_id;
      DEBUG_ERR("control command 0x%02x\n", cmd_id);
      break;
    default:
      PRINT_ERR("unknown control command id 0x%02x\n", cmd_id);
      return CONTROL_RECV_PROTOCOL_ERROR;
  }

  return CONTROL_RECV_OK;
}

static void handle_control_command(enum control_command cmd)
{
  switch (cmd) {
    case CONTROL_CMD_START_HEAP_PROFILING:
      DEBUG0_ERR("control: startHeapProfiling\n");
      startHeapProfTimer();
      break;
    case CONTROL_CMD_STOP_HEAP_PROFILING:
      DEBUG0_ERR("control: stopHeapProfiling\n");
      stopHeapProfTimer();
      break;
    case CONTROL_CMD_REQUEST_HEAP_PROFILE:
      DEBUG0_ERR("control: requestHeapProfile\n");
      requestHeapCensus();
      break;
    default:
      PRINT_ERR("control: unhandled command %d\n", cmd);
      break;
  }
}

static void start_control_receiver(int fd)
{
  pthread_t tid;
  int ret = pthread_create(&tid, NULL, control_receiver, (void *)(intptr_t)fd);
  if (ret != 0) {
    PRINT_ERR("failed to start control receiver: %s\n", strerror(ret));
    return;
  }
  ret = pthread_detach(tid);
  if (ret != 0) {
    PRINT_ERR("failed to detach control receiver: %s\n", strerror(ret));
  }
}

static void control_connection_closed(int fd)
{
  // Control receiver runs concurrently with RTS writers,
  // so clear client_fd/wt within the shared mutex.
  pthread_mutex_lock(&mutex);
  if (client_fd == fd) {
    client_fd = -1;
    write_buffer_free(&wt);
  }
  pthread_mutex_unlock(&mutex);
}

static void *control_receiver(void *arg)
{
  int fd = (int)(intptr_t)arg;

  pthread_mutex_lock(&mutex);
  while (!control_ready) {
    pthread_cond_wait(&control_ready_cond, &mutex);
  }
  pthread_mutex_unlock(&mutex);

  while (true) {
    // Grab consistent snapshot of client_fd; connection teardown may happen
    // at any time so we must hold the mutex while comparing.
    pthread_mutex_lock(&mutex);
    int current_fd = client_fd;
    pthread_mutex_unlock(&mutex);
    if (current_fd != fd)
      break;

    enum control_command cmd;
    enum control_recv_status status = control_receive_command(fd, &cmd);
    if (status == CONTROL_RECV_DISCONNECTED) {
      control_connection_closed(fd);
      break;
    } else if (status == CONTROL_RECV_PROTOCOL_ERROR) {
      // Ignore protocol errors: they indicate garbage control traffic but
      // shouldn't drop the data writer. Keep listening for valid commands.
      continue;
    }

    handle_control_command(cmd);
  }

  return NULL;
}

/*********************************************************************************
 * write_buffer
 *********************************************************************************/

// push to the back.
// Caller must serialize externally (writer_write/write_iteration hold mutex)
// so that head/last invariants stay intact.
void write_buffer_push(struct write_buffer *buf, uint8_t *data, size_t size) {
  DEBUG_ERR("%p, %lu\n", data, size);
  uint8_t *copy = malloc(size);
  memcpy(copy, data, size);

  struct write_buffer_item *item = malloc(sizeof(struct write_buffer_item));
  item->orig = copy;
  item->data = copy;
  item->size = size;
  item->next = NULL;

  struct write_buffer_item *last = buf->last;
  if (last == NULL) {
    assert(buf->head == NULL);

    buf->head = item;
    buf->last = item;
  } else {
    last->next = item;
    buf->last = item;
  }

  DEBUG_ERR("%p %p %p\n", buf, &wt, buf->head);
};

// pop from the front.
// Requires the same external synchronization as write_buffer_push.
void write_buffer_pop(struct write_buffer *buf) {
  struct write_buffer_item *head = buf->head;
  if (head == NULL) {
    // buffer is empty: nothing to do.
    return;
  } else {
    buf->head = head->next;
    if (buf->last == head) {
      buf->last = NULL;
    }
    free(head->orig);
    free(head);
  }
}

// buf itself is not freed.
// it's safe to call write_buffer_free multiple times on the same buf.
void write_buffer_free(struct write_buffer *buf) {
  // not the most effecient implementation,
  // but should be obviously correct.
  while (buf->head) {
    write_buffer_pop(buf);
  }
}

/*********************************************************************************
 * EventLogWriter
 *********************************************************************************/

static void writer_init(void)
{
  // nothing
}

static void writer_enqueue(uint8_t *data, size_t size) {
  DEBUG_ERR("size: %p %lu\n", data, size);
  bool was_empty = wt.head == NULL;

  // TODO: check the size of the queue
  // if it's too big, we can start dropping blocks.

  // for now, we just push everythinb to the back of the buffer.
  write_buffer_push(&wt, data, size);

  DEBUG_ERR("wt.head = %p\n", wt.head);
  if (was_empty) {
    wake_worker();
  }
}

static bool writer_write(void *eventlog, size_t size)
{
  DEBUG_ERR("size: %lu\n", size);
  // Serialize against worker/control threads so that client_fd and wt are read
  // atomically with respect to connection establishment/teardown.
  pthread_mutex_lock(&mutex);
  int fd = client_fd;
  if (fd < 0) {
    goto exit;
  }

  DEBUG_ERR("client_fd = %d; wt.head = %p\n", fd, wt.head);

  if (wt.head != NULL) {
    writer_enqueue(eventlog, size);
  } else {
    uint8_t *ptr = eventlog;
    size_t remaining = size;
    while (remaining > 0) {
      ssize_t ret = send(fd, ptr, remaining, 0);
      DEBUG_ERR("send return %zd\n", ret);
      if (ret == -1) {
        if (errno == EINTR) {
          continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
          writer_enqueue(ptr, remaining);
        } else if (errno == EPIPE) {
          // connection closed
        } else {
          PRINT_ERR("failed to send: %s\n", strerror(errno));
        }
        break;
      } else if (ret == 0) {
        // peer closed connection
        break;
      } else {
        ptr += ret;
        remaining -= (size_t)ret;
      }
    }
  }

exit:
  pthread_mutex_unlock(&mutex);
  return true;
}

static void writer_flush(void)
{
  // no-op
}

static void writer_stop(void)
{
  // RTS shutdown path must hold mutex so updates to client_fd/wt stay ordered
  // with the worker thread noticing the disconnect.
  pthread_mutex_lock(&mutex);
  if (client_fd >= 0) {
    close(client_fd);
    client_fd = -1;
    write_buffer_free(&wt);
  }
  pthread_mutex_unlock(&mutex);
}

const EventLogWriter SocketEventLogWriter = {
  .initEventLogWriter = writer_init,
  .writeEventLog = writer_write,
  .flushEventLog = writer_flush,
  .stopEventLogWriter = writer_stop
};

/*********************************************************************************
 * Main worker (in own thread)
 *********************************************************************************/

static void listen_iteration(void) {
  bool start_eventlog = false;

  if (listen(listen_fd, LISTEN_BACKLOG) == -1) {
    PRINT_ERR("listen() failed: %s\n", strerror(errno));
    abort();
  }

  struct sockaddr_storage remote;
  socklen_t len = sizeof(remote);

  struct pollfd pfd_accept = {
    .fd = listen_fd,
    .events = POLLIN,
    .revents = 0,
  };

  DEBUG_ERR("listen_iteration: waiting for accept on fd %d\n", listen_fd);

  // poll until we can accept
  while (true) {
    int ret = poll(&pfd_accept, 1, POLL_LISTEN_TIMEOUT);
    if (ret ==  -1) {
      PRINT_ERR("poll() failed: %s\n", strerror(errno));
      return;
    } else if (ret == 0) {
      DEBUG0_ERR("accept poll timed out\n");
    } else {
      // got connection
      DEBUG0_ERR("accept poll ready\n");
      break;
    }
  }

  // accept
  int fd = accept(listen_fd, (struct sockaddr *) &remote, &len);
  if (fd == -1) {
    PRINT_ERR("accept failed: %s\n", strerror(errno));
    return;
  }
  DEBUG_ERR("accepted new connection fd=%d\n", fd);

  // set socket into non-blocking mode
  int flags = fcntl(fd, F_GETFL);
  if (flags == -1) {
    PRINT_ERR("fnctl F_GETFL failed: %s\n", strerror(errno));
  }
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
    PRINT_ERR("fnctl F_SETFL failed: %s\n", strerror(errno));
  }

  int sndbuf = 0;
  socklen_t optlen = sizeof(sndbuf);
  if (getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, &optlen) == -1) {
    PRINT_ERR("getsockopt(SO_SNDBUF) failed: %s\n", strerror(errno));
  } else {
    DEBUG_ERR("accepted fd=%d SO_SNDBUF=%d\n", fd, sndbuf);
  }

  int sndbuf2 = 0;
  socklen_t optlen2 = sizeof(sndbuf);
  if (getsockopt(fd, SOL_SOCKET, SO_SNDLOWAT, &sndbuf2, &optlen2) == -1) {
    PRINT_ERR("getsockopt(SO_SNDBUF) failed: %s\n", strerror(errno));
  } else {
    DEBUG_ERR("accepted fd=%d SO_SNDLOWAT=%d\n", fd, sndbuf2);
  }

  // we stop existing logging so we can replay header on the new connection
  if (eventLogStatus() == EVENTLOG_RUNNING) {
    endEventLogging();
    start_eventlog = true;
  }

  // we got client_id now.
  // Publish new fd under mutex so RTS writers either see the connection along
  // with an empty queue or not at all. Keep the lock held through the condition
  // broadcast so the predicate update stays atomic on every platform.
  pthread_mutex_lock(&mutex);
  DEBUG_ERR("publishing client_fd=%d (previous=%d)\n", fd, client_fd);
  client_fd = fd;
  pthread_cond_broadcast(&new_conn_cond);
  pthread_mutex_unlock(&mutex);

  start_control_receiver(fd);

  if (start_eventlog) {
    // start writing
    startEventLogging(&SocketEventLogWriter);
  }

  // we are done.
}

// nothing to write iteration.
//
// we poll only for whether the connection is closed.
static void nonwrite_iteration(int fd) {
  DEBUG_ERR("(%d)\n", fd);

  // Wait for socket to disconnect or for pending data.
  struct pollfd pfds[2];
  pfds[0].fd = fd;
  pfds[0].events = POLLRDHUP;
  pfds[0].revents = 0;
  int nfds = 1;
  if (wake_pipe[0] != -1) {
    pfds[1].fd = wake_pipe[0];
    pfds[1].events = POLLIN;
    pfds[1].revents = 0;
    nfds = 2;
  }

  int ret = poll(pfds, nfds, -1);
  if (ret == -1) {
    if (errno == EINTR) {
      return;
    }
    PRINT_ERR("poll() failed: %s\n", strerror(errno));
    return;
  }

  if (nfds == 2 && (pfds[1].revents & POLLIN)) {
    drain_worker_wake();
    return;
  }

  if (pfds[0].revents & POLLHUP) {
    DEBUG_ERR("(%d) POLLRDHUP\n", fd);

    pthread_mutex_lock(&mutex);
    client_fd = -1;
    write_buffer_free(&wt);
    pthread_mutex_unlock(&mutex);
    return;
  }
}

// write iteration.
//
// we poll for both: can we write, and whether the connection is closed.
static void write_iteration(int fd) {
  DEBUG_ERR("(%d)\n", fd);

  // Wait for socket to disconnect
  struct pollfd pfd = {
    .fd = fd,
    .events = POLLOUT | POLLRDHUP,
    .revents = 0,
  };

  int ret = poll(&pfd, 1, POLL_WRITE_TIMEOUT);
  if (ret == -1 && errno != EAGAIN) {
    // error
    PRINT_ERR("poll() failed: %s\n", strerror(errno));
    return;
  } else if (ret == 0) {
    // timeout
    return;
  }

  // reset client_fd on RDHUP.
  if (pfd.revents & POLLHUP) {
    DEBUG_ERR("(%d) POLLRDHUP\n", fd);

    // reset client_fd
    // Protect concurrent access to client_fd and wt during teardown.
    pthread_mutex_lock(&mutex);
    assert(fd == client_fd);
    client_fd = -1;
    write_buffer_free(&wt);
    pthread_mutex_unlock(&mutex);
    return;
  }

  if (pfd.revents & POLLOUT) {
    DEBUG_ERR("(%d) POLLOUT\n", fd);

    // RTS writers also access wt, so consume queued buffers under the mutex.
    pthread_mutex_lock(&mutex);
    while (wt.head) {
      struct write_buffer_item *item = wt.head;
      ret = write(client_fd, item->data, item->size);

      if (ret == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          // couldn't write anything, shouldn't happend.
          // do nothing.
        } else if (errno == EPIPE) {
          client_fd = -1;
          write_buffer_free(&wt);
        } else {
          PRINT_ERR("failed to write: %s\n", strerror(errno));
        }

        // break out of the loop
        break;

      } else {
        // we wrote something
        if (ret >= item->size) {
          // we wrote whole element, try to write next element too
          write_buffer_pop(&wt);
          continue;
        } else {
          item->size -= ret;
          item->data += ret;
          break;
        }
      }
    }
    pthread_mutex_unlock(&mutex);
  }
}

static void iteration(void) {
  // Snapshot shared state under lock so worker decisions (listen vs write)
  // align with the current connection/queue state.
  pthread_mutex_lock(&mutex);
  int fd = client_fd;
  bool empty = wt.head == NULL;
  DEBUG_ERR("fd = %d, wt.head = %p\n", fd, wt.head);
  pthread_mutex_unlock(&mutex);

  if (fd != -1) {
    if (empty) {
      nonwrite_iteration(fd);
    } else {
      write_iteration(fd);
    }
  } else {
    listen_iteration();
  }
}

/* Main loop of eventlog-socket own thread:
 * Currently it is two states:
 * - either we have connection, then we poll for writes (and drop of connection).
 * - or we don't have, then we poll for accept.
 */
static void *worker(void *arg)
{
  (void)arg;
  while (true) {
    iteration();
  }

  return NULL; // unreachable
}

/*********************************************************************************
 * Initialization
 *********************************************************************************/

// Initialize the Unix-domain listener socket and bind it to the provided path.
// This function does not start any threads; open_socket() completes the setup.
static void init_unix_listener(const char *sock_path)
{
  DEBUG_ERR("init Unix listener: %s\n", sock_path);

  listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);

  // Record the sock_path so it can be unlinked at exit
  g_sock_path = strdup(sock_path);

  struct sockaddr_un local;
  memset(&local, 0, sizeof(local));
  local.sun_family = AF_UNIX;
  strncpy(local.sun_path, sock_path, sizeof(local.sun_path) - 1);
  unlink(sock_path);
  if (bind(listen_fd, (struct sockaddr *) &local,
           sizeof(struct sockaddr_un)) == -1) {
    PRINT_ERR("failed to bind socket %s: %s\n", sock_path, strerror(errno));
    abort();
  }
}

// Initialize a TCP listener bound to the specified host/port combination.
// Either host or port may be NULL, in which case the defaults used by
// getaddrinfo (INADDR_ANY / unspecified port) apply.
static void init_tcp_listener(const char *host, const char *port)
{
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  struct addrinfo *res = NULL;
  int ret = getaddrinfo(host, port, &hints, &res);
  if (ret != 0) {
    PRINT_ERR("getaddrinfo failed: %s\n", gai_strerror(ret));
    abort();
  }

  struct addrinfo *rp;
  for (rp = res; rp != NULL; rp = rp->ai_next) {
    listen_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (listen_fd == -1) {
      continue;
    }

    int reuse = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == -1) {
      PRINT_ERR("setsockopt(SO_REUSEADDR) failed: %s\n", strerror(errno));
      close(listen_fd);
      listen_fd = -1;
      continue;
    }

    if (bind(listen_fd, rp->ai_addr, rp->ai_addrlen) == 0) {
      char hostbuf[NI_MAXHOST];
      char servbuf[NI_MAXSERV];
      if (getnameinfo(rp->ai_addr, rp->ai_addrlen,
                      hostbuf, sizeof(hostbuf),
                      servbuf, sizeof(servbuf),
                      NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
        DEBUG_ERR("bound TCP listener to %s:%s\n", hostbuf, servbuf);
      }
      break; // success
    }

    PRINT_ERR("failed to bind TCP socket: %s\n", strerror(errno));
    close(listen_fd);
    listen_fd = -1;
  }

  freeaddrinfo(res);

  if (listen_fd == -1) {
    PRINT_ERR("unable to bind TCP listener\n");
    abort();
  }
}

static void open_socket(const struct listener_config *config)
{
  switch (config->kind) {
    case LISTENER_UNIX:
      init_unix_listener(config->sock_path);
      break;
    case LISTENER_TCP:
      init_tcp_listener(config->tcp_host, config->tcp_port);
      break;
    default:
      PRINT_ERR("unknown listener kind\n");
      abort();
  }

  int ret = pthread_create(&listen_thread, NULL, worker, NULL);
  if (ret != 0) {
    PRINT_ERR("failed to spawn thread: %s\n", strerror(ret));
    abort();
  }
}


/*********************************************************************************
 * Public interface
 *********************************************************************************/


static void ensure_initialized(void)
{
  if (!initialized) {
    pthread_mutex_init(&mutex, NULL);
    pthread_cond_init(&new_conn_cond, NULL);
    pthread_cond_init(&control_ready_cond, NULL);
    control_ready = false;
    if (pipe(wake_pipe) == -1) {
      PRINT_ERR("failed to create wake pipe: %s\n", strerror(errno));
      abort();
    }
    for (int i = 0; i < 2; i++) {
      int flags = fcntl(wake_pipe[i], F_GETFL, 0);
      if (flags == -1 || fcntl(wake_pipe[i], F_SETFL, flags | O_NONBLOCK) == -1) {
        PRINT_ERR("failed to set wake pipe nonblocking: %s\n", strerror(errno));
        abort();
      }
    }
    atexit(cleanup_socket);
    initialized = true;
  }
}

static void signal_control_ready(void)
{
  pthread_mutex_lock(&mutex);
  if (!control_ready) {
    control_ready = true;
    pthread_cond_broadcast(&control_ready_cond);
  }
  pthread_mutex_unlock(&mutex);
}

// Use this when you install SocketEventLogWriter via RtsConfig before hs_main.
// It spawns the worker immediately but defers handling of control messages
// until eventlog_socket_ready() is invoked after RTS initialization.
static void
eventlog_socket_init(const struct listener_config *config)
{
  ensure_initialized();

  if (!listener_config_valid(config))
    return;

  pthread_mutex_lock(&mutex);
  control_ready = false;
  control_ready_armed = true;
  pthread_mutex_unlock(&mutex);

  open_socket(config);
}

void eventlog_socket_ready(void)
{
  bool armed = false;

  pthread_mutex_lock(&mutex);
  if (control_ready_armed) {
    control_ready_armed = false;
    armed = true;
  }
  pthread_mutex_unlock(&mutex);

  if (armed) {
    signal_control_ready();
  }
}

void eventlog_socket_init_unix(const char *sock_path)
{
  struct listener_config config = {
    .kind = LISTENER_UNIX,
    .sock_path = sock_path,
    .tcp_host = NULL,
    .tcp_port = NULL,
  };
  eventlog_socket_init(&config);
}

void eventlog_socket_init_tcp(const char *host, const char *port)
{
  struct listener_config config = {
    .kind = LISTENER_TCP,
    .sock_path = NULL,
    .tcp_host = host,
    .tcp_port = port,
  };
  eventlog_socket_init(&config);
}

void eventlog_socket_wait(void)
{
  // Condition variable pairs with the mutex so reader threads can wait for the
  // worker to publish a connected client_fd atomically.
  pthread_mutex_lock(&mutex);
  DEBUG_ERR("eventlog_socket_wait: initial client_fd=%d\n", client_fd);
  while (client_fd == -1) {
    DEBUG0_ERR("eventlog_socket_wait: blocking for connection\n");
    int ret = pthread_cond_wait(&new_conn_cond, &mutex);
    if (ret != 0) {
      PRINT_ERR("failed to wait on condition variable: %s\n", strerror(ret));
    }
    DEBUG_ERR("eventlog_socket_wait: woke up, client_fd=%d\n", client_fd);
  }
  DEBUG_ERR("eventlog_socket_wait: proceeding with client_fd=%d\n", client_fd);
  pthread_mutex_unlock(&mutex);
}

int eventlog_socket_hs_main(int argc, char *argv[], RtsConfig conf, StgClosure *main_closure)
{
  SchedulerStatus status;
  int exit_status;

  hs_init_ghc(&argc, &argv, conf);

  // Signal that the RTS is ready so the eventlog writer can accept control connections.
  eventlog_socket_ready();

  {
    Capability *cap = rts_lock();
    rts_evalLazyIO(&cap, main_closure, NULL);
    status = rts_getSchedStatus(cap);
    rts_unlock(cap);
  }

  switch (status) {
  case Killed:
    errorBelch("main thread exited (uncaught exception)");
    exit_status = EXIT_KILLED;
    break;
  case Interrupted:
    errorBelch("interrupted");
    exit_status = EXIT_INTERRUPTED;
    break;
  case HeapExhausted:
    exit_status = EXIT_HEAPOVERFLOW;
    break;
  case Success:
    exit_status = EXIT_SUCCESS;
    break;
  default:
    barf("main thread completed with invalid status");
  }

  shutdownHaskellAndExit(exit_status, 0 /* !fastExit */);
}

// Use this from an already-running RTS: it reconfigures eventlogging to use
// SocketEventLogWriter and restarts the log when a client connects.
static void eventlog_socket_start(const struct listener_config *config, bool wait)
{
  ensure_initialized();

  if (!listener_config_valid(config))
    return;

  if (eventLogStatus() == EVENTLOG_NOT_SUPPORTED) {
    PRINT_ERR("eventlog is not supported.\n");
    return;
  }

  // we stop existing logging
  if (eventLogStatus() == EVENTLOG_RUNNING) {
    endEventLogging();
  }

  // ... and restart with outer socket writer,
  // which is no-op so far.
  //
  // This trickery is to avoid
  //
  //     printAndClearEventLog: could not flush event log
  //
  // warning messages from showing up in stderr.
  startEventLogging(&SocketEventLogWriter);

  open_socket(config);
  // Presume that the RTS is already running and we're ready if you're directly using this
  // function.
  signal_control_ready();
  if (wait) {
    switch (config->kind) {
      case LISTENER_UNIX:
        DEBUG_ERR("ghc-eventlog-socket: Waiting for connection to %s...\n", config->sock_path);
        break;
      case LISTENER_TCP: {
        const char *host = config->tcp_host ? config->tcp_host : "*";
        DEBUG_ERR("ghc-eventlog-socket: Waiting for TCP connection on %s:%s...\n", host, config->tcp_port);
        break;
      }
      default:
        break;
    }
    eventlog_socket_wait();
  }
}

void eventlog_socket_start_unix(const char *sock_path, bool wait)
{
  struct listener_config config = {
    .kind = LISTENER_UNIX,
    .sock_path = sock_path,
    .tcp_host = NULL,
    .tcp_port = NULL,
  };
  eventlog_socket_start(&config, wait);
}

void eventlog_socket_start_tcp(const char *host, const char *port, bool wait)
{
  struct listener_config config = {
    .kind = LISTENER_TCP,
    .sock_path = NULL,
    .tcp_host = host,
    .tcp_port = port,
  };
  eventlog_socket_start(&config, wait);
}
