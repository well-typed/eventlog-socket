// For POLLRDHUP
#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <Rts.h>
#include <rts/prof/Heap.h>

#include "./eventlog_socket/debug.h"
#include "eventlog_socket.h"

#define LISTEN_BACKLOG 5
#define POLL_LISTEN_TIMEOUT 10000
#define POLL_WRITE_TIMEOUT 1000
#define CONTROL_MAGIC "GCTL"
#define CONTROL_MAGIC_LEN 4
#define CONTROL_NAMESPACE_LEN 4

enum control_command {
  CONTROL_CMD_START_HEAP_PROFILING = 0x00,
  CONTROL_CMD_STOP_HEAP_PROFILING = 0x01,
  CONTROL_CMD_REQUEST_HEAP_PROFILE = 0x02,
};
#define CONTROL_NAMESPACE_CORE 0

#ifndef POLLRDHUP
#define POLLRDHUP POLLHUP
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
typedef void (*control_command_handler_fn)(control_namespace_t namespace_id,
                                           uint8_t cmd_id, void *user_data);

struct control_handler_entry {
  control_namespace_t namespace_id;
  uint8_t cmd_id;
  control_command_handler_fn handler;
  void *user_data;
  struct control_handler_entry *next;
};

static enum control_recv_status
control_receive_command(int fd, control_namespace_t *namespace_out,
                        uint8_t *cmd_out);
static void handle_control_command(control_namespace_t namespace_id,
                                   uint8_t cmd);
static bool register_control_command(control_namespace_t namespace_id,
                                     uint8_t cmd_id,
                                     control_command_handler_fn handler,
                                     void *user_data);
static void register_builtin_control_commands(void);

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
 * 3. listener spawned by start_control_receiver (this receives messages on the
 * socket)
 */

// variables read and written by worker only:
static bool g_initialized = false;
static int g_listen_fd = -1;
static const char *g_sock_path = NULL;
static int g_wake_pipe[2] = {-1, -1};

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
  if (g_sock_path) {
    unlink(g_sock_path);
  }
  if (g_wake_pipe[0] != -1) {
    close(g_wake_pipe[0]);
    g_wake_pipe[0] = -1;
  }
  if (g_wake_pipe[1] != -1) {
    close(g_wake_pipe[1]);
    g_wake_pipe[1] = -1;
  }
}

static void drain_worker_wake(void) {
  if (g_wake_pipe[0] == -1) {
    return;
  }

  uint8_t buf[32];
  while (true) {
    ssize_t ret = read(g_wake_pipe[0], buf, sizeof(buf));
    if (ret > 0) {
      continue;
    } else if (ret == 0) {
      break;
    } else if (errno == EINTR) {
      continue;
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    } else {
      DEBUG_ERROR("failed to drain wake pipe: %s\n", strerror(errno));
      break;
    }
  }
}

static void wake_worker(void) {
  if (g_wake_pipe[1] == -1) {
    return;
  }

  uint8_t byte = 1;
  ssize_t ret = write(g_wake_pipe[1], &byte, sizeof(byte));
  if (ret == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
    DEBUG_ERROR("failed to wake worker: %s\n", strerror(errno));
  }
}

// concurrency variables
static pthread_t g_listen_thread;
static pthread_cond_t g_new_conn_cond;
// Signal to the control thread to start.
static pthread_cond_t g_control_ready_cond;
// Whether we have started the control thread
static volatile bool g_control_ready = false;
// Whether the controller thread is ready to start
static bool g_control_ready_armed = false;
// Registry of control command handlers
static struct control_handler_entry *g_control_handlers = NULL;
static bool g_control_handlers_initialized = false;
// Global mutex guarding all shared state between RTS threads, the worker
// thread, and the detached control receiver. Only client_fd and wt need
// protection, but using a single mutex ensures we keep their updates
// consistent.
static pthread_mutex_t g_mutex;

// variables accessed by multiple threads and guarded by mutex:
//  * client_fd: written by worker, writer_stop, and control receiver to signal
//    when a client connects/disconnects. The lock ensures the fd value does not
//    change while other threads inspect or write to it.
//  * wt: queue of pending eventlog chunks. RTS writers append while the worker
//    thread consumes; the lock ensures push/pop operations stay consistent.
//
// Note: RTS writes client_fd in writer_stop.
static volatile int g_client_fd = -1;
static struct write_buffer g_write_buffer = {
    .head = NULL,
    .last = NULL,
};

/*********************************************************************************
 * control channel helpers (minimal protocol)
 *********************************************************************************/

static bool control_wait_for_data(int fd) {
  struct pollfd pfd = {
      .fd = fd,
      .events = POLLIN | POLLRDHUP,
      .revents = 0,
  };

  int pret = poll(&pfd, 1, POLL_WRITE_TIMEOUT);
  if (pret == -1) {
    if (errno == EINTR) {
      return true;
    }
    DEBUG_ERROR("control poll() failed: %s\n", strerror(errno));
    return false;
  }
  if (pret == 0) {
    return true; // timeout, simply retry
  }

  if (pfd.revents & POLLRDHUP) {
    return false;
  }

  return true;
}

static enum control_recv_status control_read_exact(int fd, uint8_t *buf,
                                                   size_t len) {
  size_t have = 0;
  while (have < len) {
    ssize_t got = recv(fd, buf + have, len - have, 0);
    DEBUG_TRACE("control_read_exact %zd/%zu\n", got, len);
    if (got == 0) {
      return CONTROL_RECV_DISCONNECTED;
    }
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        if (!control_wait_for_data(fd)) {
          return CONTROL_RECV_DISCONNECTED;
        }
        continue;
      }
      DEBUG_ERROR("control recv() failed: %s\n", strerror(errno));
      return CONTROL_RECV_DISCONNECTED;
    }
    have += (size_t)got;
  }
  return CONTROL_RECV_OK;
}

static bool register_control_command(control_namespace_t namespace_id,
                                     uint8_t cmd_id,
                                     control_command_handler_fn handler,
                                     void *user_data) {
  if (handler == NULL) {
    return false;
  }

  bool success = false;
  pthread_mutex_lock(&g_mutex);
  struct control_handler_entry *entry = g_control_handlers;
  while (entry != NULL) {
    if (entry->cmd_id == cmd_id && entry->namespace_id == namespace_id) {
      DEBUG_ERROR(
          "warning: duplicate registration for namespace 0x%08x command "
          "0x%02x; keeping existing handler\n",
          namespace_id, cmd_id);
      goto out;
    }
    entry = entry->next;
  }

  entry = malloc(sizeof(struct control_handler_entry));
  if (entry == NULL) {
    DEBUG_ERROR("control: failed to allocate handler entry\n");
    goto out;
  }
  entry->namespace_id = namespace_id;
  entry->cmd_id = cmd_id;
  entry->handler = handler;
  entry->user_data = user_data;
  entry->next = g_control_handlers;
  g_control_handlers = entry;
  success = true;

out:
  pthread_mutex_unlock(&g_mutex);
  return success;
}

static void control_start_heap_profiling(control_namespace_t namespace_id,
                                         uint8_t cmd, void *user_data) {
  (void)cmd;
  (void)namespace_id;
  (void)user_data;
  DEBUG_TRACE("control: startHeapProfiling\n");
  startHeapProfTimer();
}

static void control_stop_heap_profiling(control_namespace_t namespace_id,
                                        uint8_t cmd, void *user_data) {
  (void)cmd;
  (void)namespace_id;
  (void)user_data;
  DEBUG_TRACE("control: stopHeapProfiling\n");
  stopHeapProfTimer();
}

static void control_request_heap_profile(control_namespace_t namespace_id,
                                         uint8_t cmd, void *user_data) {
  (void)cmd;
  (void)namespace_id;
  (void)user_data;
  DEBUG_TRACE("control: requestHeapProfile\n");
  requestHeapCensus();
}

static void register_builtin_control_commands(void) {
  if (g_control_handlers_initialized) {
    return;
  }

  bool ok = true;
  ok = ok && register_control_command(CONTROL_NAMESPACE_CORE,
                                      CONTROL_CMD_START_HEAP_PROFILING,
                                      control_start_heap_profiling, NULL);
  ok = ok && register_control_command(CONTROL_NAMESPACE_CORE,
                                      CONTROL_CMD_STOP_HEAP_PROFILING,
                                      control_stop_heap_profiling, NULL);
  ok = ok && register_control_command(CONTROL_NAMESPACE_CORE,
                                      CONTROL_CMD_REQUEST_HEAP_PROFILE,
                                      control_request_heap_profile, NULL);

  if (!ok) {
    DEBUG_ERROR("failed to register builtin control commands\n");
  } else {
    g_control_handlers_initialized = true;
  }
}

static enum control_recv_status
control_receive_command(int fd, control_namespace_t *namespace_out,
                        uint8_t *cmd_out) {
  uint8_t header[CONTROL_MAGIC_LEN];
  enum control_recv_status status =
      control_read_exact(fd, header, CONTROL_MAGIC_LEN);
  if (status != CONTROL_RECV_OK) {
    return status;
  }
  if (memcmp(header, CONTROL_MAGIC, CONTROL_MAGIC_LEN) != 0) {
    DEBUG_TRACE("invalid control magic: %02x %02x %02x %02x\n", header[0],
                header[1], header[2], header[3]);
    return CONTROL_RECV_PROTOCOL_ERROR;
  }
  uint8_t namespace_id;
  status = control_read_exact(fd, &namespace_id, 1);
  if (status != CONTROL_RECV_OK) {
    return status;
  }
  uint8_t cmd_id = 0;
  status = control_read_exact(fd, &cmd_id, 1);
  if (status != CONTROL_RECV_OK) {
    return status;
  }

  *namespace_out = namespace_id;
  *cmd_out = cmd_id;
  DEBUG_TRACE("control command namespace=0x%08x id=0x%02x\n", namespace_id,
              cmd_id);
  return CONTROL_RECV_OK;
}

static void handle_control_command(control_namespace_t namespace_id,
                                   uint8_t cmd) {
  control_command_handler_fn handler = NULL;
  void *user_data = NULL;

  pthread_mutex_lock(&g_mutex);
  struct control_handler_entry *entry = g_control_handlers;
  while (entry != NULL) {
    if (entry->cmd_id == cmd && entry->namespace_id == namespace_id) {
      handler = entry->handler;
      user_data = entry->user_data;
      break;
    }
    entry = entry->next;
  }
  pthread_mutex_unlock(&g_mutex);

  if (handler != NULL) {
    handler(namespace_id, cmd, user_data);
  } else {
    DEBUG_ERROR("control: unhandled command namespace=0x%08x id=0x%02x\n",
                namespace_id, cmd);
  }
}

static void start_control_receiver(int fd) {
  pthread_t tid;
  int ret = pthread_create(&tid, NULL, control_receiver, (void *)(intptr_t)fd);
  if (ret != 0) {
    DEBUG_ERROR("failed to start control receiver: %s\n", strerror(ret));
    return;
  }
  ret = pthread_detach(tid);
  if (ret != 0) {
    DEBUG_ERROR("failed to detach control receiver: %s\n", strerror(ret));
  }
}

static void control_connection_closed(int fd) {
  // Control receiver runs concurrently with RTS writers,
  // so clear client_fd/wt within the shared mutex.
  pthread_mutex_lock(&g_mutex);
  if (g_client_fd == fd) {
    g_client_fd = -1;
    write_buffer_free(&g_write_buffer);
  }
  pthread_mutex_unlock(&g_mutex);
}

static void *control_receiver(void *arg) {
  int fd = (int)(intptr_t)arg;

  pthread_mutex_lock(&g_mutex);
  while (!g_control_ready) {
    pthread_cond_wait(&g_control_ready_cond, &g_mutex);
  }
  pthread_mutex_unlock(&g_mutex);

  while (true) {
    // Grab consistent snapshot of client_fd; connection teardown may happen
    // at any time so we must hold the mutex while comparing.
    pthread_mutex_lock(&g_mutex);
    int current_fd = g_client_fd;
    pthread_mutex_unlock(&g_mutex);
    if (current_fd != fd) {
      break;
    }

    control_namespace_t namespace_id = 0;
    uint8_t cmd = 0;
    enum control_recv_status status =
        control_receive_command(fd, &namespace_id, &cmd);
    if (status == CONTROL_RECV_DISCONNECTED) {
      control_connection_closed(fd);
      break;
    } else if (status == CONTROL_RECV_PROTOCOL_ERROR) {
      // Ignore protocol errors: they indicate garbage control traffic but
      // shouldn't drop the data writer. Keep listening for valid commands.
      continue;
    }

    handle_control_command(namespace_id, cmd);
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
  DEBUG_TRACE("%p, %lu\n", data, size);
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

  DEBUG_TRACE("%p %p %p\n", buf, &g_write_buffer, buf->head);
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
  // not the most efficient implementation,
  // but should be obviously correct.
  while (buf->head) {
    write_buffer_pop(buf);
  }
}

/*********************************************************************************
 * EventLogWriter
 *********************************************************************************/

static void writer_init(void) {
  // nothing
}

static void writer_enqueue(uint8_t *data, size_t size) {
  DEBUG_TRACE("size: %p %lu\n", data, size);
  bool was_empty = g_write_buffer.head == NULL;

  // TODO: check the size of the queue
  // if it's too big, we can start dropping blocks.

  // for now, we just push everythinb to the back of the buffer.
  write_buffer_push(&g_write_buffer, data, size);

  DEBUG_TRACE("wt.head = %p\n", g_write_buffer.head);
  if (was_empty) {
    wake_worker();
  }
}

static bool writer_write(void *eventlog, size_t size) {
  DEBUG_TRACE("size: %lu\n", size);
  // Serialize against worker/control threads so that client_fd and wt are read
  // atomically with respect to connection establishment/teardown.
  pthread_mutex_lock(&g_mutex);
  int fd = g_client_fd;
  if (fd < 0) {
    goto exit;
  }

  DEBUG_TRACE("client_fd = %d; wt.head = %p\n", fd, g_write_buffer.head);

  if (g_write_buffer.head != NULL) {
    // if there is stuff in queue already, we enqueue the current block.
    writer_enqueue(eventlog, size);
  } else {

    // and if there isn't, we can write immediately.
    int ret = write(fd, eventlog, size);
    DEBUG_TRACE("write return %d\n", ret);

    if (ret == -1) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // couldn't write anything, enqueue whole block
        writer_enqueue(eventlog, size);
        goto exit;
      } else if (errno == EPIPE) {
        // connection closed, simply exit
        goto exit;

      } else {
        DEBUG_ERROR("failed to write: %s\n", strerror(errno));
        goto exit;
      }
    } else {
      // we wrote something
      if (ret >= size) {
        // we wrote everything, nothing to do
        goto exit;
      } else {
        // we wrote only part of the buffer
        writer_enqueue(eventlog + ret, size - ret);
      }
    }
  }

exit:
  pthread_mutex_unlock(&g_mutex);
  return true;
}

static void writer_flush(void) {
  // no-op
}

static void writer_stop(void) {
  // RTS shutdown path must hold mutex so updates to client_fd/wt stay ordered
  // with the worker thread noticing the disconnect.
  pthread_mutex_lock(&g_mutex);
  if (g_client_fd >= 0) {
    close(g_client_fd);
    g_client_fd = -1;
    write_buffer_free(&g_write_buffer);
  }
  pthread_mutex_unlock(&g_mutex);
}

const EventLogWriter SocketEventLogWriter = {.initEventLogWriter = writer_init,
                                             .writeEventLog = writer_write,
                                             .flushEventLog = writer_flush,
                                             .stopEventLogWriter = writer_stop};

/*********************************************************************************
 * Main worker (in own thread)
 *********************************************************************************/

static void listen_iteration(void) {
  bool start_eventlog = false;

  if (listen(g_listen_fd, LISTEN_BACKLOG) == -1) {
    DEBUG_ERROR("listen() failed: %s\n", strerror(errno));
    abort();
  }

  struct sockaddr_storage remote;
  socklen_t len = sizeof(remote);

  struct pollfd pfd_accept = {
      .fd = g_listen_fd,
      .events = POLLIN,
      .revents = 0,
  };

  DEBUG_TRACE("listen_iteration: waiting for accept on fd %d\n", g_listen_fd);

  // poll until we can accept
  while (true) {
    int ret = poll(&pfd_accept, 1, POLL_LISTEN_TIMEOUT);
    if (ret == -1) {
      DEBUG_ERROR("poll() failed: %s\n", strerror(errno));
      return;
    } else if (ret == 0) {
      DEBUG_TRACE("accept poll timed out\n");
    } else {
      // got connection
      DEBUG_TRACE("accept poll ready\n");
      break;
    }
  }

  // accept
  int fd = accept(g_listen_fd, (struct sockaddr *)&remote, &len);
  if (fd == -1) {
    DEBUG_ERROR("accept failed: %s\n", strerror(errno));
    return;
  }
  DEBUG_TRACE("accepted new connection fd=%d\n", fd);

  // set socket into non-blocking mode
  int flags = fcntl(fd, F_GETFL);
  if (flags == -1) {
    DEBUG_ERROR("fnctl F_GETFL failed: %s\n", strerror(errno));
  }
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
    DEBUG_ERROR("fnctl F_SETFL failed: %s\n", strerror(errno));
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
  pthread_mutex_lock(&g_mutex);
  DEBUG_TRACE("publishing client_fd=%d (previous=%d)\n", fd, g_client_fd);
  g_client_fd = fd;
  pthread_cond_broadcast(&g_new_conn_cond);
  pthread_mutex_unlock(&g_mutex);

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
  DEBUG_TRACE("(%d)\n", fd);

  // Wait for socket to disconnect or for pending data.
  struct pollfd pfds[2];
  pfds[0].fd = fd;
  pfds[0].events = POLLRDHUP;
  pfds[0].revents = 0;
  int nfds = 1;
  if (g_wake_pipe[0] != -1) {
    pfds[1].fd = g_wake_pipe[0];
    pfds[1].events = POLLIN;
    pfds[1].revents = 0;
    nfds = 2;
  }

  int ret = poll(pfds, nfds, -1);
  if (ret == -1) {
    if (errno == EINTR) {
      return;
    }
    DEBUG_ERROR("poll() failed: %s\n", strerror(errno));
    return;
  }

  if (nfds == 2 && (pfds[1].revents & POLLIN)) {
    drain_worker_wake();
    return;
  }

  if (pfds[0].revents & POLLHUP) {
    DEBUG_TRACE("(%d) POLLRDHUP\n", fd);

    pthread_mutex_lock(&g_mutex);
    g_client_fd = -1;
    write_buffer_free(&g_write_buffer);
    pthread_mutex_unlock(&g_mutex);
    return;
  }
}

// write iteration.
//
// we poll for both: can we write, and whether the connection is closed.
static void write_iteration(int fd) {
  DEBUG_TRACE("(%d)\n", fd);

  // Wait for socket to disconnect
  struct pollfd pfd = {
      .fd = fd,
      .events = POLLOUT | POLLRDHUP,
      .revents = 0,
  };

  int ret = poll(&pfd, 1, POLL_WRITE_TIMEOUT);
  if (ret == -1 && errno != EAGAIN) {
    // error
    DEBUG_ERROR("poll() failed: %s\n", strerror(errno));
    return;
  } else if (ret == 0) {
    // timeout
    return;
  }

  // reset client_fd on RDHUP.
  if (pfd.revents & POLLHUP) {
    DEBUG_TRACE("(%d) POLLRDHUP\n", fd);

    // reset client_fd
    // Protect concurrent access to client_fd and wt during teardown.
    pthread_mutex_lock(&g_mutex);
    assert(fd == g_client_fd);
    g_client_fd = -1;
    write_buffer_free(&g_write_buffer);
    pthread_mutex_unlock(&g_mutex);
    return;
  }

  if (pfd.revents & POLLOUT) {
    DEBUG_TRACE("(%d) POLLOUT\n", fd);

    // RTS writers also access wt, so consume queued buffers under the mutex.
    pthread_mutex_lock(&g_mutex);
    while (g_write_buffer.head) {
      struct write_buffer_item *item = g_write_buffer.head;
      ret = write(g_client_fd, item->data, item->size);

      if (ret == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          // couldn't write anything, shouldn't happen.
          // do nothing.
        } else if (errno == EPIPE) {
          g_client_fd = -1;
          write_buffer_free(&g_write_buffer);
        } else {
          DEBUG_ERROR("failed to write: %s\n", strerror(errno));
        }

        // break out of the loop
        break;

      } else {
        // we wrote something
        if (ret >= item->size) {
          // we wrote whole element, try to write next element too
          write_buffer_pop(&g_write_buffer);
          continue;
        } else {
          item->size -= ret;
          item->data += ret;
          break;
        }
      }
    }
    pthread_mutex_unlock(&g_mutex);
  }
}

static void iteration(void) {
  // Snapshot shared state under lock so worker decisions (listen vs write)
  // align with the current connection/queue state.
  pthread_mutex_lock(&g_mutex);
  int fd = g_client_fd;
  bool empty = g_write_buffer.head == NULL;
  DEBUG_TRACE("fd = %d, wt.head = %p\n", fd, g_write_buffer.head);
  pthread_mutex_unlock(&g_mutex);

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
 * - either we have connection, then we poll for writes (and drop of
 * connection).
 * - or we don't have, then we poll for accept.
 */
static void *worker(void *arg) {
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
static void init_unix_listener(const char *sock_path) {
  DEBUG_TRACE("init Unix listener: %s\n", sock_path);

  g_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);

  // Record the sock_path so it can be unlinked at exit
  g_sock_path = strdup(sock_path);

  struct sockaddr_un local;
  memset(&local, 0, sizeof(local));
  local.sun_family = AF_UNIX;
  strncpy(local.sun_path, sock_path, sizeof(local.sun_path) - 1);
  unlink(sock_path);
  if (bind(g_listen_fd, (struct sockaddr *)&local,
           sizeof(struct sockaddr_un)) == -1) {
    DEBUG_ERROR("failed to bind socket %s: %s\n", sock_path, strerror(errno));
    abort();
  }
}

// Initialize a TCP listener bound to the specified host/port combination.
// Either host or port may be NULL, in which case the defaults used by
// getaddrinfo (INADDR_ANY / unspecified port) apply.
static void init_tcp_listener(const char *host, const char *port) {
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  struct addrinfo *res = NULL;
  int ret = getaddrinfo(host, port, &hints, &res);
  if (ret != 0) {
    DEBUG_ERROR("getaddrinfo failed: %s\n", gai_strerror(ret));
    abort();
  }

  struct addrinfo *rp;
  for (rp = res; rp != NULL; rp = rp->ai_next) {
    g_listen_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (g_listen_fd == -1) {
      continue;
    }

    int reuse = 1;
    if (setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse,
                   sizeof(reuse)) == -1) {
      DEBUG_ERROR("setsockopt(SO_REUSEADDR) failed: %s\n", strerror(errno));
      close(g_listen_fd);
      g_listen_fd = -1;
      continue;
    }

    if (bind(g_listen_fd, rp->ai_addr, rp->ai_addrlen) == 0) {
      char hostbuf[NI_MAXHOST];
      char servbuf[NI_MAXSERV];
      if (getnameinfo(rp->ai_addr, rp->ai_addrlen, hostbuf, sizeof(hostbuf),
                      servbuf, sizeof(servbuf),
                      NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
        DEBUG_TRACE("bound TCP listener to %s:%s\n", hostbuf, servbuf);
      }
      break; // success
    }

    DEBUG_ERROR("failed to bind TCP socket: %s\n", strerror(errno));
    close(g_listen_fd);
    g_listen_fd = -1;
  }

  freeaddrinfo(res);

  if (g_listen_fd == -1) {
    DEBUG_ERROR("unable to bind TCP listener\n");
    abort();
  }
}

static void open_socket(const struct listener_config *config) {
  switch (config->kind) {
  case LISTENER_UNIX:
    init_unix_listener(config->sock_path);
    break;
  case LISTENER_TCP:
    init_tcp_listener(config->tcp_host, config->tcp_port);
    break;
  default:
    DEBUG_ERROR("unknown listener kind\n");
    abort();
  }

  int ret = pthread_create(&g_listen_thread, NULL, worker, NULL);
  if (ret != 0) {
    DEBUG_ERROR("failed to spawn thread: %s\n", strerror(ret));
    abort();
  }
}

/*********************************************************************************
 * Public interface
 *********************************************************************************/

static void ensure_initialized(void) {
  if (!g_initialized) {
    pthread_mutex_init(&g_mutex, NULL);
    pthread_cond_init(&g_new_conn_cond, NULL);
    pthread_cond_init(&g_control_ready_cond, NULL);
    g_control_ready = false;
    if (pipe(g_wake_pipe) == -1) {
      DEBUG_ERROR("failed to create wake pipe: %s\n", strerror(errno));
      abort();
    }
    for (int i = 0; i < 2; i++) {
      int flags = fcntl(g_wake_pipe[i], F_GETFL, 0);
      if (flags == -1 ||
          fcntl(g_wake_pipe[i], F_SETFL, flags | O_NONBLOCK) == -1) {
        DEBUG_ERROR("failed to set wake pipe nonblocking: %s\n",
                    strerror(errno));
        abort();
      }
    }
    atexit(cleanup_socket);
    g_initialized = true;
  }
  register_builtin_control_commands();
}

bool eventlog_socket_register_control_command(
    control_namespace_t namespace_id, uint8_t cmd_id,
    eventlog_control_command_handler handler, void *user_data) {
  ensure_initialized();
  return register_control_command(namespace_id, cmd_id, handler, user_data);
}

static void signal_control_ready(void) {
  pthread_mutex_lock(&g_mutex);
  if (!g_control_ready) {
    g_control_ready = true;
    pthread_cond_broadcast(&g_control_ready_cond);
  }
  pthread_mutex_unlock(&g_mutex);
}

// Use this when you install SocketEventLogWriter via RtsConfig before hs_main.
// It spawns the worker immediately but defers handling of control messages
// until eventlog_socket_ready() is invoked after RTS initialization.
static void eventlog_socket_init(const struct listener_config *config) {
  ensure_initialized();

  if (!listener_config_valid(config)) {
    return;
  }

  pthread_mutex_lock(&g_mutex);
  g_control_ready = false;
  g_control_ready_armed = true;
  pthread_mutex_unlock(&g_mutex);

  open_socket(config);
}

void eventlog_socket_ready(void) {
  bool armed = false;

  pthread_mutex_lock(&g_mutex);
  if (g_control_ready_armed) {
    g_control_ready_armed = false;
    armed = true;
  }
  pthread_mutex_unlock(&g_mutex);

  if (armed) {
    signal_control_ready();
  }
}

void eventlog_socket_init_unix(const char *sock_path) {
  struct listener_config config = {
      .kind = LISTENER_UNIX,
      .sock_path = sock_path,
      .tcp_host = NULL,
      .tcp_port = NULL,
  };
  eventlog_socket_init(&config);
}

void eventlog_socket_init_tcp(const char *host, const char *port) {
  struct listener_config config = {
      .kind = LISTENER_TCP,
      .sock_path = NULL,
      .tcp_host = host,
      .tcp_port = port,
  };
  eventlog_socket_init(&config);
}

void eventlog_socket_wait(void) {
  // Condition variable pairs with the mutex so reader threads can wait for the
  // worker to publish a connected client_fd atomically.
  pthread_mutex_lock(&g_mutex);
  DEBUG_TRACE("eventlog_socket_wait: initial client_fd=%d\n", g_client_fd);
  while (g_client_fd == -1) {
    DEBUG_TRACE("eventlog_socket_wait: blocking for connection\n");
    int ret = pthread_cond_wait(&g_new_conn_cond, &g_mutex);
    if (ret != 0) {
      DEBUG_ERROR("failed to wait on condition variable: %s\n", strerror(ret));
    }
    DEBUG_TRACE("eventlog_socket_wait: woke up, client_fd=%d\n", g_client_fd);
  }
  DEBUG_TRACE("eventlog_socket_wait: proceeding with client_fd=%d\n",
              g_client_fd);
  pthread_mutex_unlock(&g_mutex);
}

int eventlog_socket_hs_main(int argc, char *argv[], RtsConfig conf,
                            StgClosure *main_closure) {
  SchedulerStatus status;
  int exit_status;

  hs_init_ghc(&argc, &argv, conf);

  // Signal that the RTS is ready so the eventlog writer can accept control
  // connections.
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
static void eventlog_socket_start(const struct listener_config *config,
                                  bool wait) {
  ensure_initialized();

  if (!listener_config_valid(config)) {
    return;
  }

  if (eventLogStatus() == EVENTLOG_NOT_SUPPORTED) {
    DEBUG_ERROR("eventlog is not supported.\n");
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
  // Presume that the RTS is already running and we're ready if you're directly
  // using this function.
  signal_control_ready();
  if (wait) {
    switch (config->kind) {
    case LISTENER_UNIX:
      DEBUG_TRACE("ghc-eventlog-socket: Waiting for connection to %s...\n",
                  config->sock_path);
      break;
    case LISTENER_TCP: {
      const char *host = config->tcp_host ? config->tcp_host : "*";
      DEBUG_TRACE(
          "ghc-eventlog-socket: Waiting for TCP connection on %s:%s...\n", host,
          config->tcp_port);
      break;
    }
    default:
      break;
    }
    eventlog_socket_wait();
  }
}

void eventlog_socket_start_unix(const char *sock_path, bool wait) {
  struct listener_config config = {
      .kind = LISTENER_UNIX,
      .sock_path = sock_path,
      .tcp_host = NULL,
      .tcp_port = NULL,
  };
  eventlog_socket_start(&config, wait);
}

void eventlog_socket_start_tcp(const char *host, const char *port, bool wait) {
  struct listener_config config = {
      .kind = LISTENER_TCP,
      .sock_path = NULL,
      .tcp_host = host,
      .tcp_port = port,
  };
  eventlog_socket_start(&config, wait);
}
