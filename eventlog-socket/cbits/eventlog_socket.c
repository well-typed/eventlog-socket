// For POLLRDHUP
#define _GNU_SOURCE

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
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

/*********************************************************************************
 * Overview
 *********************************************************************************/

// The functions in this file run concurrently.
// There are three concurrent thread groups:
//
// 1. The GHC RTS threads.
//    Functions that are called from these threads are prefixed with "writer_".
// 2. The worker thread spawned by the `start_worker` function.
//    This thread reads from g_write_buffer and writes to the eventlog socket.
//    Functions that are called from this thread are prefixed with "worker_".
// 3. The control_receiver thread spawned by `start_control_receiver`.
//    This thread reads control commands from the eventlog socket and handles them.
//    Functions that are called from this thread are prefixed with "control_".

/*********************************************************************************
 * Logging Macros
 *********************************************************************************/

// PRINT_ERR formats an error message and writes it to stderr, unconditionally.
#define PRINT_ERR(...) fprintf(stderr, "ghc-eventlog-socket: " __VA_ARGS__)

// DEBUG_ERR formats an error message and writes it to stderr, if compiled with DEBUG.
// DEBUG0_ERR does not do printf-style formatting.
#ifdef DEBUG
#define DEBUG_ERR(fmt, ...) fprintf(stderr, "ghc-eventlog-socket %s: " fmt, __func__, __VA_ARGS__)
#define DEBUG0_ERR(fmt) fprintf(stderr, "ghc-eventlog-socket %s: " fmt, __func__)
#else
#define DEBUG_ERR(fmt, ...)
#define DEBUG0_ERR(fmt)
#endif

/*********************************************************************************
 * Definitions
 *********************************************************************************/

/* SocketEventLogWriter
 *********************************************************************************/

// This file defines the `SocketEventLogWriter` constant.
// It is an instance of the `EventLogWriter` struct exposed by the GHC RTS and
// is intended to be passed to the GHC RTS using `startEventLogging`.
static const EventLogWriter SocketEventLogWriter;

static void writer_init(void);
static bool writer_write(void *eventlog, size_t size);
static void writer_flush(void);
static void writer_stop(void);
static void writer_wake_worker(void);
static void writer_enqueue(uint8_t *data, size_t size);

static const EventLogWriter SocketEventLogWriter = {
  .initEventLogWriter = writer_init,
  .writeEventLog = writer_write,
  .flushEventLog = writer_flush,
  .stopEventLogWriter = writer_stop
};

/* global constants
 *********************************************************************************/

// Used in `worker_listen_iteration`.
#define LISTEN_BACKLOG 5

// Used in `worker_listen_iteration`.
#define POLL_LISTEN_TIMEOUT 10000

// Used in `worker_write_iteration` and `control_wait_for_data`.
#define POLL_WRITE_TIMEOUT 1000

/* eventlog socket configuration
 *********************************************************************************/

static bool eventlog_socket_valid(const eventlog_socket_t *eventlog_socket);

/* write buffer
 *********************************************************************************/

// The eventlog writer maintains a write buffer which stores any events that
// cannot immediately be sent over the socket.
//
// The write buffer is distinct from the socket send buffer.
//
// The write buffer is a queue and is implemented as a linked-list of
// `write_buffer_item` items. Each item stores a pointer to the next item.
// The buffer itself stores a pointer to the head and the last item.
//
// Warning: The write buffer size is unbounded. If the pressure from the eventlog
// outpaces the bandwidth of the socket, the write buffer consumes an unbounded
// amount of space. This is not the intended behaviour and should not be relied
// upon. Future implementations of the write buffer will drop messages.
//
// Invariant: The head and last fields are either both NULL or both not NULL.
struct write_buffer;

struct write_buffer_item {
  uint8_t *orig; // original data pointer (which we free)
  uint8_t *data;
  size_t size; // invariant: size is not zero
  struct write_buffer_item *next;
};

struct write_buffer {
  struct write_buffer_item *head;
  struct write_buffer_item *last;
};

static void write_buffer_push(struct write_buffer *buf, uint8_t *data, size_t size);
static void write_buffer_pop(struct write_buffer *buf);
static void write_buffer_free(struct write_buffer *buf);

/* concurrent global variables
 *********************************************************************************/

static pthread_t g_worker_thread;
static pthread_cond_t g_new_conn_cond;

// Global mutex guarding all shared state between RTS threads, the worker
// thread, and the detached control receiver. Both `g_client_fd` and
// `g_write_buffer` need protection, but a single mutex ensures we keep their
// updates consistent.
static pthread_mutex_t g_mutex;

// This variable holds the client file descriptor. It is written by worker in
// `writer_stop` and written by the control receiver in ??? to signal when a
// client connects or disconnects.
//
// This variable is guarded by the mutex `g_mutex`.
static volatile int g_client_fd = -1;

// This is the write buffer, which is queue of pending eventlog chunks. The RTS
// thread pushes to the write buffer, while the worker thread pops from the
// write buffer.
//
// This variable is guarded by the mutex `g_mutex`.
static struct write_buffer g_write_buffer = {
  .head = NULL,
  .last = NULL,
};

/* initialisation
 *********************************************************************************/

// See: eventlog-socket/include/eventlog_socket.h

// TODO: replace `inix_X_listener` with generic `eventlog_socket_init_listener`
//       which accepts `eventlog_socket` and `eventlog_socket_opts`.

static void init_unix_listener(const char *unix_socket_path);
static void init_tcp_listener(const char *host, const char *port);
static void start_worker(const eventlog_socket_t *eventlog_socket);
static void cleanup_socket(void);
static void ensure_initialized(void);
static void signal_control_ready(void);
static void eventlog_socket_init(const eventlog_socket_t *eventlog_socket);
static void eventlog_socket_ready(void);

/* worker
 *********************************************************************************/

// global variables

static bool g_initialized = false;
static int g_sock_fd = -1;
static const char *g_unix_socket_path = NULL;
static int g_wake_pipe[2] = { -1, -1 };

// functions

static void *worker(void *arg);
static void worker_iteration(void);
static void worker_listen_iteration(void);
static void worker_write_iteration(int fd);
static void worker_nonwrite_iteration(int fd);
static void worker_wake_drain(void);

/* control receiver
 *********************************************************************************/

// global variables

// Signal to the control thread to start.
static pthread_cond_t g_control_ready_cond;

// Whether we have started the control thread.
static volatile bool g_control_ready = false;

// Whether the controller thread is ready to start.
static bool g_control_ready_armed = false;

// The control receiver accepts control signals encoded as byte strings, where
// control signal is preceded by the sequence `CONTROL_MAGIC` followed by a
// value from the `control_command` enum encoded as a byte.

#define CONTROL_MAGIC "GCTL"
#define CONTROL_MAGIC_LEN 4

enum control_command {
  CONTROL_CMD_START_HEAP_PROFILING = 0x01,
  CONTROL_CMD_STOP_HEAP_PROFILING = 0x02,
  CONTROL_CMD_REQUEST_HEAP_PROFILE = 0x03,
};

// The control receiver reports its status using values from the
// `control_recv_status` enum. This is intended to keep the socket alive as
// long as we can still read from the peer, even if the payload is malformed.

enum control_recv_status {
  CONTROL_RECV_OK,
  CONTROL_RECV_PROTOCOL_ERROR,
  CONTROL_RECV_DISCONNECTED,
};

static void start_control_receiver(int fd);
static void *control_receiver(void *arg);
static void control_connection_closed(int fd);
static enum control_recv_status control_receive_command(int fd, enum control_command *cmd_out);
static void control_handle_command(enum control_command cmd);

/*********************************************************************************
 * Implementations
 *********************************************************************************/

/* EventLogWriter
 *********************************************************************************/

static void writer_init(void)
{
  // nothing
}

static bool writer_write(void *eventlog, size_t size)
{
  DEBUG_ERR("size: %lu\n", size);
  // Serialize against worker/control threads so that g_client_fd and
  // g_write_buffer are read atomically with respect to connection
  // establishment and teardown.
  pthread_mutex_lock(&g_mutex);
  int fd = g_client_fd;
  if (fd < 0) {
    goto exit;
  }

  DEBUG_ERR("g_client_fd = %d; g_write_buffer.head = %p\n", fd, g_write_buffer.head);

  if (g_write_buffer.head != NULL) {
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
  pthread_mutex_unlock(&g_mutex);
  return true;
}

static void writer_flush(void)
{
  // no-op
}

static void writer_stop(void)
{
  // RTS shutdown path must hold mutex so updates to g_client_fd and
  // g_write_buffer stay ordered with the worker thread noticing the
  // disconnect.
  pthread_mutex_lock(&g_mutex);
  if (g_client_fd >= 0) {
    close(g_client_fd);
    g_client_fd = -1;
    write_buffer_free(&g_write_buffer);
  }
  pthread_mutex_unlock(&g_mutex);
}

static void writer_enqueue(uint8_t *data, size_t size)
{
  DEBUG_ERR("size: %p %lu\n", data, size);
  bool was_empty = g_write_buffer.head == NULL;

  // TODO: check the size of the queue
  // if it's too big, we can start dropping blocks.

  // for now, we just push everythinb to the back of the buffer.
  write_buffer_push(&g_write_buffer, data, size);

  DEBUG_ERR("g_write_buffer.head = %p\n", g_write_buffer.head);
  if (was_empty) {
    writer_wake_worker();
  }
}

static void writer_wake_worker(void)
{
  if (g_wake_pipe[1] == -1)
    return;

  uint8_t byte = 1;
  ssize_t ret = write(g_wake_pipe[1], &byte, sizeof(byte));
  if (ret == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
    PRINT_ERR("failed to wake worker: %s\n", strerror(errno));
  }
}

/* eventlog socket configuration
 *********************************************************************************/

static bool eventlog_socket_valid(const eventlog_socket_t *eventlog_socket) {
  if (eventlog_socket == NULL) {
    return false;
  }

  switch (eventlog_socket->addr_family) {
    case EVENTLOG_UNIX:
      return eventlog_socket->addr.unix_socket.path != NULL;
    case EVENTLOG_INET:
      return eventlog_socket->addr.inet_socket.port != NULL;
    default:
      return false;
  }
}

/* write buffer
 *********************************************************************************/

// push to the back.
// Caller must serialize externally (writer_write/worker_write_iteration hold mutex)
// so that head/last invariants stay intact.
static void write_buffer_push(struct write_buffer *buf, uint8_t *data, size_t size) {
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

  DEBUG_ERR("%p %p %p\n", buf, &g_write_buffer, buf->head);
}

// pop from the front.
// Requires the same external synchronization as write_buffer_push.
static void write_buffer_pop(struct write_buffer *buf) {
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
static void write_buffer_free(struct write_buffer *buf) {
  // not the most efficient implementation,
  // but should be obviously correct.
  while (buf->head) {
    write_buffer_pop(buf);
  }
}

/* initialisation
 *********************************************************************************/

// Initialize the Unix-domain listener socket and bind it to the provided path.
// This function does not start any threads; start_worker() completes the setup.
static void init_unix_listener(const char *unix_socket_path)
{
  DEBUG_ERR("init Unix listener: %s\n", unix_socket_path);

  g_sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);

  // Set the send buffer size (SO_SNDBUF):
  //
  // TODO: add SO_SNDBUF and SO_SNDLOWAT as parameters to eventlog-socket.
  if (setsockopt(g_sock_fd, SOL_SOCKET, SO_SNDBUF, &(int){212992}, sizeof(int)) == -1) {
    PRINT_ERR("setsockopt(SO_SNDBUF) failed: %s\n", strerror(errno));
  }
  if (setsockopt(g_sock_fd, SOL_SOCKET, SO_SNDLOWAT, &(int){1}, sizeof(int)) == -1) {
    PRINT_ERR("setsockopt(SO_SNDLOWAT) failed: %s\n", strerror(errno));
  }

  // Record the unix_socket_path so it can be unlinked at exit
  g_unix_socket_path = strdup(unix_socket_path);

  struct sockaddr_un local;
  memset(&local, 0, sizeof(local));
  local.sun_family = AF_UNIX;
  strncpy(local.sun_path, unix_socket_path, sizeof(local.sun_path) - 1);
  // TODO: only unlink the file if this is specified in the options and the existing file is a socket
  unlink(unix_socket_path);
  if (bind(g_sock_fd, (struct sockaddr *) &local,
           sizeof(struct sockaddr_un)) == -1) {
    PRINT_ERR("failed to bind socket %s: %s\n", unix_socket_path, strerror(errno));
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
    g_sock_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (g_sock_fd == -1) {
      continue;
    }

    int reuse = 1;
    if (setsockopt(g_sock_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == -1) {
      PRINT_ERR("setsockopt(SO_REUSEADDR) failed: %s\n", strerror(errno));
      close(g_sock_fd);
      g_sock_fd = -1;
      continue;
    }

    if (bind(g_sock_fd, rp->ai_addr, rp->ai_addrlen) == 0) {
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
    close(g_sock_fd);
    g_sock_fd = -1;
  }

  freeaddrinfo(res);

  if (g_sock_fd == -1) {
    PRINT_ERR("unable to bind TCP listener\n");
    abort();
  }
}

static void start_worker(const eventlog_socket_t *eventlog_socket)
{
  switch (eventlog_socket->addr_family) {
    case EVENTLOG_UNIX:
      init_unix_listener(eventlog_socket->addr.unix_socket.path);
      break;
    case EVENTLOG_INET:
      init_tcp_listener(eventlog_socket->addr.inet_socket.host, eventlog_socket->addr.inet_socket.port);
      break;
    default:
      PRINT_ERR("unknown listener kind\n");
      abort();
  }

  int ret = pthread_create(&g_worker_thread, NULL, worker, NULL);
  if (ret != 0) {
    PRINT_ERR("failed to spawn thread: %s\n", strerror(ret));
    abort();
  }
}

static void cleanup_socket(void)
{
    if (g_unix_socket_path) unlink(g_unix_socket_path);
    if (g_wake_pipe[0] != -1) {
      close(g_wake_pipe[0]);
      g_wake_pipe[0] = -1;
    }
    if (g_wake_pipe[1] != -1) {
      close(g_wake_pipe[1]);
      g_wake_pipe[1] = -1;
    }
}

static void ensure_initialized(void)
{
  if (!g_initialized) {
    pthread_mutex_init(&g_mutex, NULL);
    pthread_cond_init(&g_new_conn_cond, NULL);
    pthread_cond_init(&g_control_ready_cond, NULL);
    g_control_ready = false;
    if (pipe(g_wake_pipe) == -1) {
      PRINT_ERR("failed to create wake pipe: %s\n", strerror(errno));
      abort();
    }
    for (int i = 0; i < 2; i++) {
      int flags = fcntl(g_wake_pipe[i], F_GETFL, 0);
      if (flags == -1 || fcntl(g_wake_pipe[i], F_SETFL, flags | O_NONBLOCK) == -1) {
        PRINT_ERR("failed to set wake pipe nonblocking: %s\n", strerror(errno));
        abort();
      }
    }
    atexit(cleanup_socket);
    g_initialized = true;
  }
}

static void signal_control_ready(void)
{
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
static void eventlog_socket_init(const eventlog_socket_t *eventlog_socket)
{
  ensure_initialized();

  if (!eventlog_socket_valid(eventlog_socket))
    return;

  pthread_mutex_lock(&g_mutex);
  g_control_ready = false;
  g_control_ready_armed = true;
  pthread_mutex_unlock(&g_mutex);

  start_worker(eventlog_socket);
}

static void eventlog_socket_ready(void)
{
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

void eventlog_socket_init_unix(const char *unix_socket_path)
{
 eventlog_socket_t eventlog_socket = {
    .addr_family = EVENTLOG_UNIX,
    .addr.unix_socket.path = unix_socket_path,
  };
  eventlog_socket_init(&eventlog_socket);
}

void eventlog_socket_init_tcp(const char *tcp_host, const char *tcp_port)
{
 eventlog_socket_t eventlog_socket = {
    .addr_family = EVENTLOG_INET,
    .addr.inet_socket = {
      .host = tcp_host,
      .port = tcp_port,
    }
  };
  eventlog_socket_init(&eventlog_socket);
}

void eventlog_socket_wait(void)
{
  // Condition variable pairs with the mutex so reader threads can wait for the
  // worker to publish a connected g_client_fd atomically.
  pthread_mutex_lock(&g_mutex);
  DEBUG_ERR("eventlog_socket_wait: initial g_client_fd=%d\n", g_client_fd);
  while (g_client_fd == -1) {
    DEBUG0_ERR("eventlog_socket_wait: blocking for connection\n");
    int ret = pthread_cond_wait(&g_new_conn_cond, &g_mutex);
    if (ret != 0) {
      PRINT_ERR("failed to wait on condition variable: %s\n", strerror(ret));
    }
    DEBUG_ERR("eventlog_socket_wait: woke up, g_client_fd=%d\n", g_client_fd);
  }
  DEBUG_ERR("eventlog_socket_wait: proceeding with g_client_fd=%d\n", g_client_fd);
  pthread_mutex_unlock(&g_mutex);
}

int eventlog_socket_wrap_hs_main(int argc, char *argv[], RtsConfig conf, StgClosure *main_closure)
{
  SchedulerStatus status;
  int exit_status;

  // Set the eventlog writer:
  conf.eventlog_writer = &SocketEventLogWriter;

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
static void eventlog_socket_start(const eventlog_socket_t *eventlog_socket, bool wait)
{
  ensure_initialized();

  if (!eventlog_socket_valid(eventlog_socket)) {
    PRINT_ERR("eventlog socket configuration is not valid.\n");
    return;
  }

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

  start_worker(eventlog_socket);
  // Presume that the RTS is already running and we're ready if you're directly using this
  // function.
  signal_control_ready();
  if (wait) {
    switch (eventlog_socket->addr_family) {
      case EVENTLOG_UNIX:
        DEBUG_ERR("ghc-eventlog-socket: Waiting for connection to %s...\n", eventlog_socket->addr.unix_socket.path);
        break;
      case EVENTLOG_INET: {
        DEBUG_ERR("ghc-eventlog-socket: Waiting for TCP connection on %s:%s...\n", eventlog_socket->addr.inet_socket.host ? eventlog_socket->addr.inet_socket.host : "*", eventlog_socket->addr.inet_socket.port);
        break;
      }
      default:
        break;
    }
    eventlog_socket_wait();
  }
}

void eventlog_socket_start_unix(const char *unix_socket_path, bool wait)
{
 eventlog_socket_t eventlog_socket = {
    .addr_family = EVENTLOG_UNIX,
    .addr.unix_socket.path = unix_socket_path,
  };
  eventlog_socket_start(&eventlog_socket, wait);
}

void eventlog_socket_start_tcp(const char *tcp_host, const char *tcp_port, bool wait)
{
 eventlog_socket_t eventlog_socket = {
    .addr_family = EVENTLOG_INET,
    .addr = {
      .inet_socket.host = tcp_host,
      .inet_socket.port = tcp_port,
    }
  };
  eventlog_socket_start(&eventlog_socket, wait);
}

/* worker
 *********************************************************************************/

// POLLRDHUP has been defined since Linux 2.6.17.
// In older versions, we define it to be equal to POLLHUP.
//
// See: https://www.man7.org/linux/man-pages/man2/poll.2.html
//
// TODO: Should we undefine this at the end of this file?
#ifndef POLLRDHUP
#define POLLRDHUP POLLHUP
#endif

/* Main loop of eventlog-socket own thread:
 * Currently it is two states:
 * - either we have connection, then we poll for writes (and drop of connection).
 * - or we don't have, then we poll for accept.
 */
static void *worker(void *arg)
{
  (void)arg;
  while (true) {
    worker_iteration();
  }

  return NULL; // unreachable
}

static void worker_iteration(void) {
  // Snapshot shared state under lock so worker decisions (listen vs write)
  // align with the current connection/queue state.
  pthread_mutex_lock(&g_mutex);
  int fd = g_client_fd;
  bool empty = g_write_buffer.head == NULL;
  DEBUG_ERR("fd = %d, g_write_buffer.head = %p\n", fd, g_write_buffer.head);
  pthread_mutex_unlock(&g_mutex);

  if (fd != -1) {
    if (empty) {
      worker_nonwrite_iteration(fd);
    } else {
      worker_write_iteration(fd);
    }
  } else {
    worker_listen_iteration();
  }
}

static void worker_listen_iteration(void) {
  bool start_eventlog = false;

  if (listen(g_sock_fd, LISTEN_BACKLOG) == -1) {
    PRINT_ERR("listen() failed: %s\n", strerror(errno));
    abort();
  }

  struct sockaddr_storage remote;
  socklen_t len = sizeof(remote);

  struct pollfd pfd_accept = {
    .fd = g_sock_fd,
    .events = POLLIN,
    .revents = 0,
  };

  DEBUG_ERR("worker_listen_iteration: waiting for accept on fd %d\n", g_sock_fd);

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
  int fd = accept(g_sock_fd, (struct sockaddr *) &remote, &len);
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
  pthread_mutex_lock(&g_mutex);
  DEBUG_ERR("publishing g_client_fd=%d (previous=%d)\n", fd, g_client_fd);
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
static void worker_nonwrite_iteration(int fd) {
  DEBUG_ERR("(%d)\n", fd);

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
    PRINT_ERR("poll() failed: %s\n", strerror(errno));
    return;
  }

  if (nfds == 2 && (pfds[1].revents & POLLIN)) {
    worker_wake_drain();
    return;
  }

  if (pfds[0].revents & POLLHUP) {
    DEBUG_ERR("(%d) POLLRDHUP\n", fd);

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
static void worker_write_iteration(int fd) {
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

  // reset g_client_fd on RDHUP.
  if (pfd.revents & POLLHUP) {
    DEBUG_ERR("(%d) POLLRDHUP\n", fd);

    // reset g_client_fd
    // Protect concurrent access to g_client_fd and g_write_buffer during teardown.
    pthread_mutex_lock(&g_mutex);
    assert(fd == g_client_fd);
    g_client_fd = -1;
    write_buffer_free(&g_write_buffer);
    pthread_mutex_unlock(&g_mutex);
    return;
  }

  if (pfd.revents & POLLOUT) {
    DEBUG_ERR("(%d) POLLOUT\n", fd);

    // RTS writers also access g_write_buffer, so consume queued buffers under the mutex.
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
          PRINT_ERR("failed to write: %s\n", strerror(errno));
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

static void worker_wake_drain(void)
{
  if (g_wake_pipe[0] == -1)
    return;

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
      PRINT_ERR("failed to drain wake pipe: %s\n", strerror(errno));
      break;
    }
  }
}

/* control receiver
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

static void control_handle_command(enum control_command cmd)
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
  // so clear g_client_fd/g_write_buffer within the shared mutex.
  pthread_mutex_lock(&g_mutex);
  if (g_client_fd == fd) {
    g_client_fd = -1;
    write_buffer_free(&g_write_buffer);
  }
  pthread_mutex_unlock(&g_mutex);
}

static void *control_receiver(void *arg)
{
  int fd = (int)(intptr_t)arg;

  pthread_mutex_lock(&g_mutex);
  while (!g_control_ready) {
    pthread_cond_wait(&g_control_ready_cond, &g_mutex);
  }
  pthread_mutex_unlock(&g_mutex);

  while (true) {
    // Grab consistent snapshot of g_client_fd; connection teardown may happen
    // at any time so we must hold the mutex while comparing.
    pthread_mutex_lock(&g_mutex);
    int current_fd = g_client_fd;
    pthread_mutex_unlock(&g_mutex);
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

    control_handle_command(cmd);
  }

  return NULL;
}
