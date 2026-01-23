// Required for `POLLRDHUP` (used in poll.h).
#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
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

#include "./eventlog_socket/control.h"
#include "./eventlog_socket/debug.h"
#include "./eventlog_socket/poll.h"
#include "./eventlog_socket/write_buffer.h"
#include "eventlog_socket.h"

#define LISTEN_BACKLOG 5

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

// concurrency variables
static pthread_t *g_listen_thread_ptr = NULL;
static pthread_cond_t g_new_conn_cond = PTHREAD_COND_INITIALIZER;
static pthread_t *g_control_thread_ptr = NULL;
// Global mutex guarding all shared state between RTS threads, the worker
// thread, and the detached control receiver. Only client_fd and wt need
// protection, but using a single mutex ensures we keep their updates
// consistent.
static pthread_mutex_t g_write_buffer_and_client_fd_mutex =
    PTHREAD_MUTEX_INITIALIZER;

// variables accessed by multiple threads and guarded by mutex:
//  * client_fd: written by worker, writer_stop, and control receiver to signal
//    when a client connects/disconnects. The lock ensures the fd value does not
//    change while other threads inspect or write to it.
//  * wt: queue of pending eventlog chunks. RTS writers append while the worker
//    thread consumes; the lock ensures push/pop operations stay consistent.
//
// Note: RTS writes client_fd in writer_stop.
static volatile int g_client_fd = -1;
static write_buffer_t g_write_buffer = {
    .head = NULL,
    .last = NULL,
};

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

static void cleanup(void) {
  // Remove socket file.
  if (g_sock_path) {
    unlink(g_sock_path);
  }
  // Close the wake pipes.
  if (g_wake_pipe[0] != -1) {
    close(g_wake_pipe[0]);
    g_wake_pipe[0] = -1;
  }
  if (g_wake_pipe[1] != -1) {
    close(g_wake_pipe[1]);
    g_wake_pipe[1] = -1;
  }
  // Stop the control thread.
  if (g_control_thread_ptr != NULL) {
    DEBUG_DEBUG("%s", "Cancelling control thread.");
    if (!pthread_cancel(*g_control_thread_ptr)) {
      DEBUG_ERRNO("pthread_cancel() failed for control thread");
    } else {
      if (!pthread_join(*g_control_thread_ptr, NULL)) {
        DEBUG_ERRNO("pthread_join() failed for control thread");
      }
    }
    free((void *)g_control_thread_ptr);
  }
  // Stop the worker thread.
  if (g_listen_thread_ptr != NULL) {
    DEBUG_DEBUG("%s", "Cancelling worker thread.");
    if (!pthread_cancel(*g_listen_thread_ptr)) {
      DEBUG_ERRNO("pthread_cancel() failed for worker thread");
    } else {
      if (!pthread_join(*g_listen_thread_ptr, NULL)) {
        DEBUG_ERRNO("pthread_join() failed for worker thread");
      }
    }
    free((void *)g_listen_thread_ptr);
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
      DEBUG_ERRNO("read() failed");
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
    DEBUG_ERRNO("write() failed");
  }
}

/*********************************************************************************
 * EventLogWriter
 *********************************************************************************/

static void writer_init(void) {
  // nothing
}

static void writer_enqueue(uint8_t *data, size_t size) {
  DEBUG_TRACE("size: %p %lu", (void *)data, size);
  bool was_empty = g_write_buffer.head == NULL;

  // TODO: check the size of the queue
  // if it's too big, we can start dropping blocks.

  // for now, we just push everythinb to the back of the buffer.
  write_buffer_push(&g_write_buffer, data, size);

  DEBUG_TRACE("wt.head = %p", (void *)g_write_buffer.head);
  if (was_empty) {
    wake_worker();
  }
}

static bool writer_write(void *eventlog, size_t size) {
  DEBUG_TRACE("size: %lu", size);
  // Serialize against worker/control threads so that client_fd and wt are read
  // atomically with respect to connection establishment/teardown.
  pthread_mutex_lock(&g_write_buffer_and_client_fd_mutex);
  int fd = g_client_fd;
  if (fd < 0) {
    goto exit;
  }

  DEBUG_TRACE("client_fd = %d; wt.head = %p", fd, (void *)g_write_buffer.head);

  if (g_write_buffer.head != NULL) {
    // if there is stuff in queue already, we enqueue the current block.
    writer_enqueue(eventlog, size);
  } else {

    // and if there isn't, we can write immediately.
    const ssize_t num_bytes_written_or_err = write(fd, eventlog, size);
    DEBUG_TRACE("write return %zd", num_bytes_written_or_err);

    if (num_bytes_written_or_err == -1) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // couldn't write anything, enqueue whole block
        writer_enqueue(eventlog, size);
        goto exit;
      } else if (errno == EPIPE) {
        // connection closed, simply exit
        goto exit;

      } else {
        DEBUG_ERRNO("write() failed");
        goto exit;
      }
    } else {
      // cast from ssize_t to size_t is safe as num_bytes_written_or_err != -1
      const size_t num_bytes_written = num_bytes_written_or_err;
      // we wrote something
      if (num_bytes_written >= size) {
        // we wrote everything, nothing to do
        goto exit;
      } else {
        // we wrote only part of the buffer
        writer_enqueue((uint8_t *)eventlog + num_bytes_written,
                       size - num_bytes_written);
      }
    }
  }

exit:
  pthread_mutex_unlock(&g_write_buffer_and_client_fd_mutex);
  return true;
}

static void writer_flush(void) {
  // no-op
}

static void writer_stop(void) {
  // RTS shutdown path must hold mutex so updates to client_fd/wt stay ordered
  // with the worker thread noticing the disconnect.
  pthread_mutex_lock(&g_write_buffer_and_client_fd_mutex);
  if (g_client_fd >= 0) {
    close(g_client_fd);
    g_client_fd = -1;
    write_buffer_free(&g_write_buffer);
  }
  pthread_mutex_unlock(&g_write_buffer_and_client_fd_mutex);
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
    DEBUG_ERRNO("listen() failed");
    abort();
  }

  struct sockaddr_storage remote;
  socklen_t len = sizeof(remote);

  struct pollfd pfd_accept = {
      .fd = g_listen_fd,
      .events = POLLIN,
      .revents = 0,
  };

  DEBUG_TRACE("listen_iteration: waiting for accept on fd %d", g_listen_fd);

  // poll until we can accept
  while (true) {
    int ret = poll(&pfd_accept, 1, POLL_LISTEN_TIMEOUT);
    if (ret == -1) {
      DEBUG_ERRNO("poll() failed");
      return;
    } else if (ret == 0) {
      DEBUG_TRACE("%s", "accept poll timed out");
    } else {
      // got connection
      DEBUG_TRACE("%s", "accept poll ready");
      break;
    }
  }

  // accept
  int fd = accept(g_listen_fd, (struct sockaddr *)&remote, &len);
  if (fd == -1) {
    DEBUG_ERRNO("accept() failed");
    return;
  }
  DEBUG_TRACE("accepted new connection fd=%d", fd);

  // set socket into non-blocking mode
  int flags = fcntl(fd, F_GETFL);
  if (flags == -1) {
    DEBUG_ERRNO("fnctl() failed for F_GETFL");
  }
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
    DEBUG_ERRNO("fnctl() failed for F_SETFL");
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
  pthread_mutex_lock(&g_write_buffer_and_client_fd_mutex);
  DEBUG_TRACE("publishing client_fd=%d (previous=%d)", fd, g_client_fd);
  g_client_fd = fd;
  pthread_cond_broadcast(&g_new_conn_cond);
  pthread_mutex_unlock(&g_write_buffer_and_client_fd_mutex);

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
  DEBUG_TRACE("(%d)", fd);

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
    DEBUG_ERRNO("poll() failed");
    return;
  }

  if (nfds == 2 && (pfds[1].revents & POLLIN)) {
    drain_worker_wake();
    return;
  }

  if (pfds[0].revents & POLLHUP) {
    DEBUG_TRACE("(%d) POLLRDHUP", fd);

    pthread_mutex_lock(&g_write_buffer_and_client_fd_mutex);
    g_client_fd = -1;
    write_buffer_free(&g_write_buffer);
    pthread_mutex_unlock(&g_write_buffer_and_client_fd_mutex);
    return;
  }
}

// write iteration.
//
// we poll for both: can we write, and whether the connection is closed.
static void write_iteration(int fd) {
  DEBUG_TRACE("(%d)", fd);

  // Wait for socket to disconnect
  struct pollfd pfd = {
      .fd = fd,
      .events = POLLOUT | POLLRDHUP,
      .revents = 0,
  };

  const int num_ready_or_err = poll(&pfd, 1, POLL_WRITE_TIMEOUT);
  if (num_ready_or_err == -1 && errno != EAGAIN) {
    // error
    DEBUG_ERRNO("poll() failed");
    return;
  } else if (num_ready_or_err == 0) {
    // timeout
    return;
  }

  // reset client_fd on RDHUP.
  if (pfd.revents & POLLHUP) {
    DEBUG_TRACE("(%d) POLLRDHUP", fd);

    // reset client_fd
    // Protect concurrent access to client_fd and wt during teardown.
    pthread_mutex_lock(&g_write_buffer_and_client_fd_mutex);
    assert(fd == g_client_fd);
    g_client_fd = -1;
    write_buffer_free(&g_write_buffer);
    pthread_mutex_unlock(&g_write_buffer_and_client_fd_mutex);
    return;
  }

  if (pfd.revents & POLLOUT) {
    DEBUG_TRACE("(%d) POLLOUT", fd);

    // RTS writers also access wt, so consume queued buffers under the mutex.
    pthread_mutex_lock(&g_write_buffer_and_client_fd_mutex);
    while (g_write_buffer.head) {
      write_buffer_item_t *item = g_write_buffer.head;
      const ssize_t num_bytes_written_or_err =
          write(g_client_fd, item->data, item->size);

      if (num_bytes_written_or_err == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          // couldn't write anything, shouldn't happen.
          // do nothing.
        } else if (errno == EPIPE) {
          g_client_fd = -1;
          write_buffer_free(&g_write_buffer);
        } else {
          DEBUG_ERRNO("write() failed");
        }

        // break out of the loop
        break;

      } else {
        // cast from ssize_t to size_t is safe as num_bytes_written_or_err != -1
        const size_t num_bytes_written = num_bytes_written_or_err;
        // we wrote something
        if (num_bytes_written >= item->size) {
          // we wrote whole element, try to write next element too
          write_buffer_pop(&g_write_buffer);
          continue;
        } else {
          item->size -= num_bytes_written;
          item->data += num_bytes_written;
          break;
        }
      }
    }
    pthread_mutex_unlock(&g_write_buffer_and_client_fd_mutex);
  }
}

static void iteration(void) {
  // Snapshot shared state under lock so worker decisions (listen vs write)
  // align with the current connection/queue state.
  pthread_mutex_lock(&g_write_buffer_and_client_fd_mutex);
  int fd = g_client_fd;
  bool empty = g_write_buffer.head == NULL;
  DEBUG_TRACE("fd = %d, wt.head = %p", fd, (void *)g_write_buffer.head);
  pthread_mutex_unlock(&g_write_buffer_and_client_fd_mutex);

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
  DEBUG_TRACE("init Unix listener: %s", sock_path);

  g_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);

  // Record the sock_path so it can be unlinked at exit
  g_sock_path = strdup(sock_path);

  // set socket linger
  const struct linger so_linger = {
      .l_onoff = true,
      .l_linger = 10,
  };
  const int so_linger_success_or_error = setsockopt(
      g_listen_fd, SOL_SOCKET, SO_LINGER, &so_linger, sizeof(so_linger));
  if (so_linger_success_or_error != 0) {
    DEBUG_ERRNO("setsockopt() failed for SO_LINGER");
  }

  // set socket receive low water mark
  const int so_rcvlowat = 1;
  const int so_rcvlowat_success_or_error = setsockopt(
      g_listen_fd, SOL_SOCKET, SO_RCVLOWAT, &so_rcvlowat, sizeof(so_rcvlowat));
  if (so_rcvlowat_success_or_error != 0) {
    DEBUG_ERRNO("setsockopt() failed for SO_RCVLOWAT");
  }

  struct sockaddr_un local;
  memset(&local, 0, sizeof(local));
  local.sun_family = AF_UNIX;
  strncpy(local.sun_path, sock_path, sizeof(local.sun_path) - 1);
  unlink(sock_path);
  if (bind(g_listen_fd, (struct sockaddr *)&local,
           sizeof(struct sockaddr_un)) == -1) {
    DEBUG_ERRNO("bind() failed");
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
    DEBUG_ERROR("getaddrinfo() failed: %s", gai_strerror(ret));
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
      DEBUG_ERRNO("setsockopt() failed for SO_REUSEADDR");
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
        DEBUG_TRACE("bound TCP listener to %s:%s", hostbuf, servbuf);
      }
      break; // success
    }

    DEBUG_ERRNO("bind() failed");
    close(g_listen_fd);
    g_listen_fd = -1;
  }

  freeaddrinfo(res);

  if (g_listen_fd == -1) {
    DEBUG_ERROR("%s", "unable to bind TCP listener");
    abort();
  }
}

static void worker_start(const struct listener_config *config) {
  DEBUG_TRACE("%s", "Starting worker thread.");
  switch (config->kind) {
  case LISTENER_UNIX:
    init_unix_listener(config->sock_path);
    break;
  case LISTENER_TCP:
    init_tcp_listener(config->tcp_host, config->tcp_port);
    break;
  default:
    DEBUG_ERROR("%s", "unknown listener kind");
    abort();
  }

  // start the worker thread
  g_listen_thread_ptr = (pthread_t *)malloc(sizeof(pthread_t));
  int ret = pthread_create(g_listen_thread_ptr, NULL, worker, NULL);
  if (ret != 0) {
    DEBUG_ERRNO("pthread_create() failed");
    abort();
  }
}

/*********************************************************************************
 * Public interface
 *********************************************************************************/

static void ensure_initialized(void) {
  if (!g_initialized) {
    if (pipe(g_wake_pipe) == -1) {
      DEBUG_ERRNO("pipe() failed");
      abort();
    }
    for (int i = 0; i < 2; i++) {
      int flags = fcntl(g_wake_pipe[i], F_GETFL, 0);
      if (flags == -1 ||
          fcntl(g_wake_pipe[i], F_SETFL, flags | O_NONBLOCK) == -1) {
        DEBUG_ERRNO("fcntl() failed for F_SETFL");
        abort();
      }
    }
    atexit(cleanup);
    g_initialized = true;
  }
}

// Use this when you install SocketEventLogWriter via RtsConfig before hs_main.
// It spawns the worker immediately but defers handling of control messages
// until eventlog_socket_ready() is invoked after RTS initialization.
static void eventlog_socket_init(const struct listener_config *config) {
  ensure_initialized();

  if (!listener_config_valid(config)) {
    return;
  }

  // start worker thread
  worker_start(config);

  // start control thread
  g_control_thread_ptr = (pthread_t *)malloc(sizeof(pthread_t));
  eventlog_socket_control_start(g_control_thread_ptr, &g_client_fd,
                                &g_write_buffer_and_client_fd_mutex,
                                &g_new_conn_cond);
}

// Unix domain socket paths are limited to 108 bytes.
// This is 107 characters and one null byte.
#define UNIX_DOMAIN_SOCKET_PATH_MAX_LEN 108

int eventlog_socket_init_from_env(void) {
  const char *ghc_eventlog_unix_socket = getenv("GHC_EVENTLOG_UNIX_SOCKET");
  if (ghc_eventlog_unix_socket != NULL) {
    const size_t ghc_eventlog_unix_socket_len =
        strnlen(ghc_eventlog_unix_socket, UNIX_DOMAIN_SOCKET_PATH_MAX_LEN + 1);
    if (ghc_eventlog_unix_socket_len >= UNIX_DOMAIN_SOCKET_PATH_MAX_LEN) {
      return -1;
    } else {
      eventlog_socket_init_unix(ghc_eventlog_unix_socket);
    }
  } else {
    const char *ghc_eventlog_tcp_host = getenv("GHC_EVENTLOG_TCP_HOST");
    const char *ghc_eventlog_tcp_port = getenv("GHC_EVENTLOG_TCP_PORT");
    if (ghc_eventlog_tcp_host != NULL && ghc_eventlog_tcp_port != NULL) {
      eventlog_socket_init_tcp(ghc_eventlog_tcp_host, ghc_eventlog_tcp_port);
    } else {
      return 0;
    }
  }
  const char *ghc_eventlog_wait = getenv("GHC_EVENTLOG_WAIT");
  if (ghc_eventlog_wait != NULL) {
    eventlog_socket_wait();
  }
  return 1;
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
  pthread_mutex_lock(&g_write_buffer_and_client_fd_mutex);
  DEBUG_TRACE("initial client_fd=%d", g_client_fd);
  while (g_client_fd == -1) {
    DEBUG_TRACE("%s", "blocking for connection");
    int ret = pthread_cond_wait(&g_new_conn_cond,
                                &g_write_buffer_and_client_fd_mutex);
    if (ret != 0) {
      DEBUG_ERRNO("pthread_cond_wait() failed");
    }
    DEBUG_TRACE("woke up, client_fd=%d", g_client_fd);
  }
  DEBUG_TRACE("proceeding with client_fd=%d", g_client_fd);
  pthread_mutex_unlock(&g_write_buffer_and_client_fd_mutex);
}

int eventlog_socket_hs_main(int argc, char *argv[], RtsConfig conf,
                            StgClosure *main_closure) {
  SchedulerStatus status;
  int exit_status;

  hs_init_ghc(&argc, &argv, conf);

  // Signal that the RTS is ready so the eventlog writer can accept control
  // connections.
  eventlog_socket_control_signal_ghc_rts_ready();

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
    DEBUG_ERROR("%s", "eventlog is not supported.");
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

  // start worker thread
  worker_start(config);

  // start control thread
  g_control_thread_ptr = (pthread_t *)malloc(sizeof(pthread_t));
  eventlog_socket_control_start(g_control_thread_ptr, &g_client_fd,
                                &g_write_buffer_and_client_fd_mutex,
                                &g_new_conn_cond);

  // Presume that the RTS is already running and we're ready if you're directly
  // using this function.
  eventlog_socket_control_signal_ghc_rts_ready();
  if (wait) {
    switch (config->kind) {
    case LISTENER_UNIX:
      DEBUG_TRACE("Waiting for connection to %s...", config->sock_path);
      break;
    case LISTENER_TCP: {
      const char *host = config->tcp_host ? config->tcp_host : "*";
      DEBUG_TRACE("Waiting for TCP connection on %s:%s...\n", host,
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
