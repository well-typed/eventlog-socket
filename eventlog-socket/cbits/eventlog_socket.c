#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
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
#include "./eventlog_socket/error.h"
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
 * 2. worker spawned by worker_start (this writes to the socket)
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
static WriteBuffer g_write_buffer = {
    .head = NULL,
    .last = NULL,
};

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
    if (pthread_cancel(*g_control_thread_ptr) != 0) {
      DEBUG_ERRNO("pthread_cancel() failed for control thread");
    } else {
      if (pthread_join(*g_control_thread_ptr, NULL) != 0) {
        DEBUG_ERRNO("pthread_join() failed for control thread");
      }
    }
    free((void *)g_control_thread_ptr);
  }
  // Stop the worker thread.
  if (g_listen_thread_ptr != NULL) {
    DEBUG_DEBUG("%s", "Cancelling worker thread.");
    if (pthread_cancel(*g_listen_thread_ptr) != 0) {
      DEBUG_ERRNO("pthread_cancel() failed for worker thread");
    } else {
      if (pthread_join(*g_listen_thread_ptr, NULL) != 0) {
        DEBUG_ERRNO("pthread_join() failed for worker thread");
      }
    }
    free((void *)g_listen_thread_ptr);
  }
}

#define EVENTLOG_SOCKET_WORKER_CHUNK_SIZE 32

static void drain_worker_wake(void) {
  if (g_wake_pipe[0] == -1) {
    return;
  }
  uint8_t chunk[EVENTLOG_SOCKET_WORKER_CHUNK_SIZE];
  while (true) {
    const ssize_t success_or_error = read(g_wake_pipe[0], chunk, sizeof(chunk));
    if (success_or_error > 0) {
      continue;
    } else if (success_or_error == 0) {
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

  const uint8_t byte = 1;
  const ssize_t success_or_error = write(g_wake_pipe[1], &byte, sizeof(byte));
  if (success_or_error == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
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

/// @brief The eventlog socket writer.
///
/// @warning It is only safe to pass this value to the GHC RTS *after*
/// `eventlog_socket_init` or `eventlog_socket_start` is called.
static const EventLogWriter SocketEventLogWriter = {
    .initEventLogWriter = writer_init,
    .writeEventLog = writer_write,
    .flushEventLog = writer_flush,
    .stopEventLogWriter = writer_stop};

/*********************************************************************************
 * Main worker (in own thread)
 *********************************************************************************/

static void worker_step_listen(void) {
  bool start_eventlog = false;

  if (listen(g_listen_fd, LISTEN_BACKLOG) == -1) {
    DEBUG_ERRNO("listen() failed");
    abort();
  }

  struct sockaddr_storage remote;
  socklen_t remote_len = sizeof(remote);

  struct pollfd pfds[1] = {{
      .fd = g_listen_fd,
      .events = POLLIN,
      .revents = 0,
  }};

  DEBUG_TRACE("listen_iteration: waiting for accept on fd %d", g_listen_fd);

  // poll until we can accept
  while (true) {
    const int ready_or_error = poll(pfds, 1, POLL_LISTEN_TIMEOUT);
    if (ready_or_error == -1) {
      DEBUG_ERRNO("poll() failed");
      return;
    } else if (ready_or_error == 0) {
      DEBUG_TRACE("%s", "accept poll timed out");
    } else {
      // got connection
      DEBUG_TRACE("%s", "accept poll ready");
      break;
    }
  }

  // accept
  const int client_fd =
      accept(g_listen_fd, (struct sockaddr *)&remote, &remote_len);
  if (client_fd == -1) {
    DEBUG_ERRNO("accept() failed");
    return;
  }
  DEBUG_TRACE("accepted new connection fd=%d", client_fd);

  // set socket into non-blocking mode
  const int flags = fcntl(client_fd, F_GETFL);
  if (flags == -1) {
    DEBUG_ERRNO("fnctl() failed for F_GETFL");
  }
  if (fcntl(client_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
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
  DEBUG_TRACE("publishing client_fd=%d (previous=%d)", client_fd, g_client_fd);
  g_client_fd = client_fd;
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
static void worker_step_nonwrite(int fd) {
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

  const int ready_or_error = poll(pfds, nfds, -1);
  if (ready_or_error == -1) {
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
static void worker_step_write(int fd) {
  DEBUG_TRACE("(%d)", fd);

  // Wait for socket to disconnect
  struct pollfd pfds[1] = {{
      .fd = fd,
      .events = POLLOUT | POLLRDHUP,
      .revents = 0,
  }};

  const int num_ready_or_err = poll(pfds, 1, POLL_WRITE_TIMEOUT);
  if (num_ready_or_err == -1 && errno != EAGAIN) {
    // error
    DEBUG_ERRNO("poll() failed");
    return;
  } else if (num_ready_or_err == 0) {
    // timeout
    return;
  }

  // reset client_fd on RDHUP.
  if (pfds[0].revents & POLLHUP) {
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

  if (pfds[0].revents & POLLOUT) {
    DEBUG_TRACE("(%d) POLLOUT", fd);

    // RTS writers also access wt, so consume queued buffers under the mutex.
    pthread_mutex_lock(&g_write_buffer_and_client_fd_mutex);
    while (g_write_buffer.head) {
      WriteBufferItem *item = g_write_buffer.head;
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

static void worker_step(void) {
  // Snapshot shared state under lock so worker decisions (listen vs write)
  // align with the current connection/queue state.
  pthread_mutex_lock(&g_write_buffer_and_client_fd_mutex);
  const int client = g_client_fd;
  const bool write_buffer_empty = g_write_buffer.head == NULL;
  DEBUG_TRACE("fd = %d, wt.head = %p", client, (void *)g_write_buffer.head);
  pthread_mutex_unlock(&g_write_buffer_and_client_fd_mutex);

  if (client != -1) {
    if (write_buffer_empty) {
      worker_step_nonwrite(client);
    } else {
      worker_step_write(client);
    }
  } else {
    worker_step_listen();
  }
}

/* Main loop of eventlog-socket own thread:
 * Currently it is two states:
 * - either we have connection, then we poll for writes (and drop of
 * connection).
 * - or we don't have, then we poll for accept.
 */
static void *worker_loop(void *arg) {
  (void)arg;
  while (true) {
    worker_step();
  }
  return NULL; // unreachable
}

/// @brief Initialize the Unix domain socket and bind it to the provided path.
///
/// This function does not start any threads; @c worker_start() completes the
/// setup.
///
/// @return See `EventlogSocketStatus`.
///
/// @par Errors
/// @parblock
/// This function may return the following system errors:
/// `EADDRINUSE`, `EADDRNOTAVAIL`, `EAFNOSUPPORT`, `EBADF`, `EDOM`,
/// `EFAULT`, `EINVAL`, `EISCONN`, `ELOOP`, `EMFILE`, `ENAMETOOLONG`, `ENFILE`,
/// `ENOBUFS`, `ENOENT`, `ENOMEM`, `ENOPROTOOPT`, `ENOTDIR`, `ENOTSOCK`,
/// `EPROTONOSUPPORT`, or `EROFS`.
/// @endparblock
static EventlogSocketStatus
worker_socket_init_unix(const EventlogSocketUnixAddr *const unix_addr,
                        const EventlogSocketOpts *const opts) {
  DEBUG_TRACE("init Unix listener: %s", unix_addr->esa_unix_path);

  // Create a socket.
  g_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (g_listen_fd == -1) {
    return STATUS_FROM_ERRNO(); // `setsockopt` sets errno.
  }

  // Record the sock_path so it can be unlinked at exit
  g_sock_path = strdup(unix_addr->esa_unix_path);
  if (g_sock_path == NULL) {
    return STATUS_FROM_ERRNO(); // `strdup` sets errno.
  }

  // Set socket linger.
  {
    const struct linger so_linger = {
        .l_onoff = true,
        .l_linger = 10,
    };
    if (setsockopt(g_listen_fd, SOL_SOCKET, SO_LINGER, &so_linger,
                   sizeof(so_linger)) == -1) {
      DEBUG_ERRNO("setsockopt() failed for SO_LINGER");
      const int setsockopt_errno = errno; // `setsockopt` sets errno.
      close(g_listen_fd);
      g_listen_fd = -1;
      errno = setsockopt_errno;
      return STATUS_FROM_ERRNO();
    }
  }

  // Set socket receive low water mark.
  if (setsockopt(g_listen_fd, SOL_SOCKET, SO_RCVLOWAT, &(int){1},
                 sizeof(int)) == -1) {
    DEBUG_ERRNO("setsockopt() failed for SO_RCVLOWAT");
    const int setsockopt_errno = errno; // `setsockopt` sets errno.
    close(g_listen_fd);
    g_listen_fd = -1;
    errno = setsockopt_errno;
    return STATUS_FROM_ERRNO();
  }

  // Set socket send buffer size.
  if (opts != NULL && opts->eso_sndbuf > 0) {
    if (setsockopt(g_listen_fd, SOL_SOCKET, SO_SNDBUF, &opts->eso_sndbuf,
                   sizeof(opts->eso_sndbuf)) == -1) {
      DEBUG_ERRNO("setsockopt() failed for SO_SNDBUF");
      const int setsockopt_errno = errno; // `setsockopt` sets errno.
      close(g_listen_fd);
      g_listen_fd = -1;
      errno = setsockopt_errno;
      return STATUS_FROM_ERRNO();
    }
  }

  struct sockaddr_un local;
  memset(&local, 0, sizeof(local));
  local.sun_family = AF_UNIX;
  strncpy(local.sun_path, unix_addr->esa_unix_path, sizeof(local.sun_path) - 1);
  unlink(unix_addr->esa_unix_path);
  if (bind(g_listen_fd, (struct sockaddr *)&local,
           sizeof(struct sockaddr_un)) == -1) {
    DEBUG_ERRNO("bind() failed");
    return STATUS_FROM_ERRNO(); // `bind` sets errno.
  }
  return STATUS_FROM_CODE(EVENTLOG_SOCKET_OK);
}

/// @brief Initialize the TCP/IP socket and bind it to the provided address.
///
/// This function does not start any threads; @c worker_start() completes the
/// setup.
///
/// Either host or port may be NULL, in which case the defaults used by
/// @c getaddrinfo apply.
///
/// @return See `EventlogSocketStatus`.
///
/// @par Errors
/// @parblock
/// This function may return the following system errors:
/// `EADDRINUSE`, `EADDRNOTAVAIL`, `EAFNOSUPPORT`, `EAGAIN`, `EALREADY`,
/// `EBADF`, `EDESTADDRREQ`, `EDOM`, `EFAULT`, `EINPROGRESS`, `EINVAL`, `EIO`,
/// `EISCONN`, `EISDIR`, `ELOOP`, `EMFILE`, `ENAMETOOLONG`, `ENFILE`, `ENOBUFS`,
/// `ENOENT`, `ENOMEM`, `ENOPROTOOPT`, `ENOTDIR`, `ENOTSOCK`, `EOPNOTSUPP`,
/// `EPROTONOSUPPORT`, or `EROFS`.
///
/// This function may return the following @c getaddrinfo errors:
/// `EAI_ADDRFAMILY`, `EAI_AGAIN`, `EAI_BADFLAGS`, `EAI_FAIL`, `EAI_FAMILY`,
/// `EAI_MEMORY`, `EAI_NODATA`, `EAI_NONAME`, `EAI_SERVICE`, `EAI_SOCKTYPE`, or
/// `EAI_SYSTEM`.
/// @endparblock
static EventlogSocketStatus
worker_socket_init_inet(const EventlogSocketInetAddr *const inet_addr,
                        const EventlogSocketOpts *const opts) {
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  struct addrinfo *res = NULL;
  const int success_or_gai_error = getaddrinfo(
      inet_addr->esa_inet_host, inet_addr->esa_inet_port, &hints, &res);
  if (success_or_gai_error != 0) {
    DEBUG_ERROR("getaddrinfo(\"%s\", \"%s\") failed: %s",
                inet_addr->esa_inet_host, inet_addr->esa_inet_port,
                gai_strerror(success_or_gai_error));
    return STATUS_FROM_GAI_ERROR(success_or_gai_error);
  }

  struct addrinfo *rp;
  for (rp = res; rp != NULL; rp = rp->ai_next) {
    g_listen_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (g_listen_fd == -1) {
      continue;
    }

    // Set socket reuse address.
    if (setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &(int){1},
                   sizeof(int)) != 0) {
      DEBUG_ERRNO("setsockopt() failed for SO_REUSEADDR");
      close(g_listen_fd);
      g_listen_fd = -1;
      continue;
    }

    // set socket send buffer size
    if (opts != NULL && opts->eso_sndbuf > 0) {
      if (setsockopt(g_listen_fd, SOL_SOCKET, SO_SNDBUF, &opts->eso_sndbuf,
                     sizeof(opts->eso_sndbuf)) != 0) {
        DEBUG_ERRNO("setsockopt() failed for SO_SNDBUF");
        close(g_listen_fd);
        g_listen_fd = -1;
        continue;
      }
    }

    if (bind(g_listen_fd, rp->ai_addr, rp->ai_addrlen) == -1) {
      DEBUG_ERRNO("bind() failed");
      close(g_listen_fd);
      g_listen_fd = -1;
      continue;
    } else {
      char hostbuf[NI_MAXHOST];
      char servbuf[NI_MAXSERV];
      if (getnameinfo(rp->ai_addr, rp->ai_addrlen, hostbuf, sizeof(hostbuf),
                      servbuf, sizeof(servbuf),
                      NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
        DEBUG_TRACE("Bound TCP listener to %s:%s", hostbuf, servbuf);
      }
      break; // success
    }
  }

  freeaddrinfo(res);

  if (g_listen_fd == -1) {
    DEBUG_ERROR("%s", "Unable to bind TCP listener");
    errno = EAGAIN;
    return STATUS_FROM_ERRNO();
  }

  return STATUS_FROM_CODE(EVENTLOG_SOCKET_OK);
}

/// @brief Initialize the worker thread.
///
/// @return See `EventlogSocketStatus`.
///
/// @par Errors
/// @parblock
/// This function may return the following system errors:
/// `EACCES`, `EAGAIN`, `EBADF`, `EFAULT`, `EINVAL`, `EMFILE`, `ENFILE`,
/// `ENFILE`, or `ENOPKG`.
/// @endparblock
static EventlogSocketStatus worker_init(void) {
  if (!g_initialized) {
    if (pipe(g_wake_pipe) == -1) {
      DEBUG_ERRNO("pipe() failed");
      return STATUS_FROM_ERRNO();
    }
    for (int i = 0; i < 2; i++) {
      const int flags = fcntl(g_wake_pipe[i], F_GETFL, 0);
      if (flags == -1) {
        DEBUG_ERRNO("fcntl() failed for F_GETFL");
        return STATUS_FROM_ERRNO();
      }
      if (fcntl(g_wake_pipe[i], F_SETFL, flags | O_NONBLOCK) == -1) {
        DEBUG_ERRNO("fcntl() failed for F_SETFL");
        return STATUS_FROM_ERRNO();
      }
    }
    atexit(cleanup);
    g_initialized = true;
  }
  return STATUS_FROM_CODE(EVENTLOG_SOCKET_OK);
}

/// @brief Start the worker thread.
///
/// @return Upon successful completion, 0 is returned.
///
/// @return On error, one of the following error codes is returned.
///
/// @par Errors
/// @parblock
/// `EAI_ADDRFAMILY`, `EAI_AGAIN`, `EAI_BADFLAGS`, `EAI_FAIL`, `EAI_FAMILY`,
/// `EAI_MEMORY`, `EAI_NODATA`, `EAI_NONAME`, `EAI_SERVICE`, `EAI_SOCKTYPE`, or
/// `EAI_SYSTEM`.
///
/// If `EAI_SYSTEM` is returned, errno is set to one of the following error
/// codes, in addition to any of the system errors that may be produced by
/// `getaddrinfo`:
///
/// `EACCES`, `EADDRINUSE`, `EADDRNOTAVAIL`, `EAFNOSUPPORT`, `EAGAIN`,
/// `EALREADY`, `EBADF`, `EDESTADDRREQ`, `EDOM`, `EFAULT`, `EINPROGRESS`,
/// `EINVAL`, `EIO`, `EISCONN`, `EISDIR`, `ELOOP`, `EMFILE`, `ENAMETOOLONG`,
/// `ENFILE`, `ENOBUFS`, `ENOENT`, `ENOMEM`, `ENOPKG`, `ENOPROTOOPT`, `ENOTDIR`,
/// `ENOTSOCK`, `EOPNOTSUPP`, `EPERM`, `EPROTONOSUPPORT`, or `EROFS`.
/// @endparblock
static EventlogSocketStatus
worker_start(const EventlogSocketAddr *const eventlog_socket_addr,
             const EventlogSocketOpts *const eventlog_socket_opts) {
  DEBUG_TRACE("%s", "Starting worker thread.");
  switch (eventlog_socket_addr->esa_tag) {
  case EVENTLOG_SOCKET_UNIX: {
    RETURN_ON_ERROR(worker_socket_init_unix(
        &eventlog_socket_addr->esa_unix_addr, eventlog_socket_opts));
    break;
  }
  case EVENTLOG_SOCKET_INET: {
    RETURN_ON_ERROR(worker_socket_init_inet(
        &eventlog_socket_addr->esa_inet_addr, eventlog_socket_opts));
    break;
  }
  default: {
    DEBUG_ERROR("%s", "unknown listener kind");
    errno = EINVAL;
    return STATUS_FROM_ERRNO();
  }
  }

  // start the worker thread
  g_listen_thread_ptr = (pthread_t *)malloc(sizeof(pthread_t));
  const int success_or_errno =
      pthread_create(g_listen_thread_ptr, NULL, worker_loop, NULL);
  if (success_or_errno != 0) {
    DEBUG_ERRNO("pthread_create() failed");
    return STATUS_FROM_PTHREAD_ERROR(success_or_errno);
  }
  return STATUS_FROM_CODE(EVENTLOG_SOCKET_OK);
}

/*********************************************************************************
 * Internal Helpers
 *********************************************************************************/

/// @brief Get the maximum length of a Unix domain socket path.
static size_t get_unix_path_max(void) {
  const struct sockaddr_un test_unix_path_max;
  return sizeof(test_unix_path_max.sun_path);
}

/*********************************************************************************
 * Public interface
 *********************************************************************************/

/* PUBLIC - see documentation in eventlog_socket.h */
EventlogSocketStatus
eventlog_socket_init(const EventlogSocketAddr *const eventlog_socket_addr,
                     const EventlogSocketOpts *const eventlog_socket_opts) {

  // Initialise worker thread.
  RETURN_ON_ERROR(worker_init());

  // Start worker thread.
  RETURN_ON_ERROR(worker_start(eventlog_socket_addr, eventlog_socket_opts));

  // Start control thread.
  g_control_thread_ptr = (pthread_t *)malloc(sizeof(pthread_t));
  if (g_control_thread_ptr == NULL) {
    return STATUS_FROM_ERRNO(); // `malloc` sets errno.
  }
  RETURN_ON_ERROR(eventlog_socket_control_start(
      g_control_thread_ptr, &g_client_fd, &g_write_buffer_and_client_fd_mutex,
      &g_new_conn_cond));
  return STATUS_FROM_CODE(EVENTLOG_SOCKET_OK);
}

/* PUBLIC - see documentation in eventlog_socket.h */
EventlogSocketStatus eventlog_socket_wait(void) {
  // Condition variable pairs with the mutex so reader threads can wait for the
  // worker to publish a connected client_fd atomically.
  {
    const int success_or_errno =
        pthread_mutex_lock(&g_write_buffer_and_client_fd_mutex);
    if (success_or_errno != 0) {
      return STATUS_FROM_PTHREAD_ERROR(success_or_errno);
    }
  }
  DEBUG_TRACE("initial client_fd=%d", g_client_fd);
  while (g_client_fd == -1) {
    DEBUG_TRACE("%s", "blocking for connection");
    const int success_or_errno = pthread_cond_wait(
        &g_new_conn_cond, &g_write_buffer_and_client_fd_mutex);
    if (success_or_errno != 0) {
      return STATUS_FROM_PTHREAD_ERROR(success_or_errno);
    }
    DEBUG_TRACE("woke up, client_fd=%d", g_client_fd);
  }
  DEBUG_TRACE("proceeding with client_fd=%d", g_client_fd);
  const int success_or_errno =
      pthread_mutex_unlock(&g_write_buffer_and_client_fd_mutex);
  if (success_or_errno != 0) {
    return STATUS_FROM_PTHREAD_ERROR(success_or_errno);
  }
  return STATUS_FROM_CODE(EVENTLOG_SOCKET_OK);
}

/* PUBLIC - see documentation in eventlog_socket.h */
RtsConfig eventlog_socket_attach_rts_config(RtsConfig rts_config) {
  rts_config.eventlog_writer = &SocketEventLogWriter;
  return rts_config;
}

/* PUBLIC - see documentation in eventlog_socket.h */
void eventlog_socket_wrap_hs_main(int argc, char *argv[], RtsConfig rts_config,
                                  StgClosure *main_closure) {
  SchedulerStatus status;
  int exit_status;

  // Set the eventlog socket writer.
  rts_config = eventlog_socket_attach_rts_config(rts_config);

  // Initialize the GHC RTS.
  hs_init_ghc(&argc, &argv, rts_config);

  // Tell the control thread that the GHC RTS is initialised.
  // TODO: print error message
  eventlog_socket_signal_ghc_rts_ready();

  // Evaluate the Haskell main closure.
  {
    Capability *cap = rts_lock();
    rts_evalLazyIO(&cap, main_closure, NULL);
    status = rts_getSchedStatus(cap);
    rts_unlock(cap);
  }

  // Handle the return status.
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

  // Shut down the GHC RTS and exit.
  shutdownHaskellAndExit(exit_status, 0 /* !fastExit */);
}

/// @brief Start event logging with `SocketEventLogWriter` and start the control
/// thread.
///
/// @pre The GHC RTS is ready.
///
/// @pre The function `eventlog_socket_init` has been called.
EventlogSocketStatus eventlog_socket_attach(void) {
  // Check if this version of the GHC RTS supports the eventlog.
  if (eventLogStatus() == EVENTLOG_NOT_SUPPORTED) {
    DEBUG_ERROR("%s", "eventlog is not supported.");
    return STATUS_FROM_CODE(EVENTLOG_SOCKET_ERROR_RTS_NOSUPPORT);
  }

  // Stop the existing eventlog writer.
  if (eventLogStatus() == EVENTLOG_RUNNING) {
    endEventLogging();
  }

  // Attach the `SocketEventLogWriter` eventlog writer.
  if (!startEventLogging(&SocketEventLogWriter)) {
    return STATUS_FROM_CODE(EVENTLOG_SOCKET_ERROR_RTS_FAIL);
  }

  return STATUS_FROM_CODE(EVENTLOG_SOCKET_OK);
}

/* PUBLIC - see documentation in eventlog_socket.h */
EventlogSocketStatus
eventlog_socket_start(const EventlogSocketAddr *eventlog_socket_addr,
                      const EventlogSocketOpts *eventlog_socket_opts) {

  // Initialize eventlog_socket.
  RETURN_ON_ERROR(
      eventlog_socket_init(eventlog_socket_addr, eventlog_socket_opts));

  // Attach the eventlog writer to the GHC RTS.
  RETURN_ON_ERROR(eventlog_socket_attach());

  // Signal that the GHC RTS is ready.
  RETURN_ON_ERROR(eventlog_socket_signal_ghc_rts_ready());

  return STATUS_FROM_CODE(EVENTLOG_SOCKET_OK);
}

/* PUBLIC - see documentation in eventlog_socket.h */
void eventlog_socket_opts_init(EventlogSocketOpts *eventlog_socket_opts) {
  if (eventlog_socket_opts == NULL) {
    return;
  }
  eventlog_socket_opts->eso_wait = false;
  eventlog_socket_opts->eso_sndbuf = 0;
}

/* PUBLIC - see documentation in eventlog_socket.h */
void eventlog_socket_addr_free(EventlogSocketAddr *eventlog_socket) {
  if (eventlog_socket == NULL) {
    return;
  }
  switch (eventlog_socket->esa_tag) {
  case EVENTLOG_SOCKET_UNIX:
    if (eventlog_socket->esa_unix_addr.esa_unix_path != NULL) {
      free(eventlog_socket->esa_unix_addr.esa_unix_path);
    }
    break;
  case EVENTLOG_SOCKET_INET:
    if (eventlog_socket->esa_inet_addr.esa_inet_host != NULL) {
      free(eventlog_socket->esa_inet_addr.esa_inet_host);
    }
    if (eventlog_socket->esa_inet_addr.esa_inet_port != NULL) {
      free(eventlog_socket->esa_inet_addr.esa_inet_port);
    }
    break;
  }
}

/// @brief Copy the first `str_len` characters of a string into allocated
/// memory.
///
/// @return Upon successful completion, a pointer to an allocated copy of the
/// string is returned.
///
/// @return On error, a null pointer is returned and errno is set to indicate
/// the error.
static char *strncpy_alloc(const size_t str_len, const char *const str) {
  // `calloc` sets errno.
  char *str_copy = calloc(str_len + 1, sizeof(char));
  if (str_copy == NULL) {
    return NULL;
  }
  strncpy(str_copy, str, str_len);
  return str_copy;
}

/* PUBLIC - see documentation in eventlog_socket.h */
void eventlog_socket_opts_free(EventlogSocketOpts *eventlog_socket_opts) {
  (void)eventlog_socket_opts;
  // The `EventlogSocketOpts` may be extended without a breaking change in
  // the package version. Hence, this function is included in case any future
  // version of this type includes malloc'd memory.
}

/* PUBLIC - see documentation in eventlog_socket.h */
EventlogSocketStatus
eventlog_socket_from_env(EventlogSocketAddr *eventlog_socket_addr_out,
                         EventlogSocketOpts *eventlog_socket_opts_out) {

  // Check that eventlog_socket_out is nonnull.
  if (eventlog_socket_addr_out == NULL) {
    errno = EINVAL;
    return STATUS_FROM_ERRNO();
  }

  // Allocate a variable for the return status:
  EventlogSocketStatusCode status_code = EVENTLOG_SOCKET_ERROR_CNF_NOADDR;

  // Try to construct a Unix domain socket address:
  char *unix_path = getenv(EVENTLOG_SOCKET_ENV_UNIX_PATH); // NOLINT
  if (unix_path != NULL) {
    // Determine the maximum length of a Unix domain socket path.
    const size_t unix_path_max = get_unix_path_max();

    // Check that unix_path does not exceed the maximum unix_path length:
    const size_t unix_path_len = strlen(unix_path);
    if (unix_path_len > unix_path_max) {
      status_code = EVENTLOG_SOCKET_ERROR_CNF_TOOLONG;
    }

    // Write the configuration:
    char *unix_path_copy = strncpy_alloc(unix_path_len, unix_path);
    if (unix_path_copy == NULL) {
      return STATUS_FROM_ERRNO(); // `strncpy_alloc` sets errno.
    }
    *eventlog_socket_addr_out = (EventlogSocketAddr){
        .esa_tag = EVENTLOG_SOCKET_UNIX,
        .esa_unix_addr =
            {
                .esa_unix_path = unix_path_copy,
            },
    };

    // Set the status:
    status_code = EVENTLOG_SOCKET_OK;
  }

  // Try to construct a TCP/IP address:
  else {
    char *inet_host = getenv(EVENTLOG_SOCKET_ENV_INET_HOST); // NOLINT
    char *inet_port = getenv(EVENTLOG_SOCKET_ENV_INET_PORT); // NOLINT
    const bool has_inet_host = inet_host != NULL;
    const bool has_inet_port = inet_port != NULL;
    // If either is set, construct a TCP/IP address:
    if (has_inet_host || has_inet_port) {
      // Copy the inet_host:
      char *inet_host_copy = (has_inet_host)
                                 ? strncpy_alloc(strlen(inet_host), inet_host)
                                 : strncpy_alloc(0, "");
      if (inet_host_copy == NULL) {
        return STATUS_FROM_ERRNO(); // `strncpy_alloc` sets errno.
      }
      // Copy the inet_port:
      char *inet_port_copy = (has_inet_port)
                                 ? strncpy_alloc(strlen(inet_port), inet_port)
                                 : strncpy_alloc(0, "");
      if (inet_port_copy == NULL) {
        free(inet_host_copy);
        return STATUS_FROM_ERRNO(); // `strncpy_alloc` sets errno.
      }
      // Write the configuration:
      *eventlog_socket_addr_out =
          (EventlogSocketAddr){.esa_tag = EVENTLOG_SOCKET_INET,
                               .esa_inet_addr = {
                                   .esa_inet_host = inet_host_copy,
                                   .esa_inet_port = inet_port_copy,
                               }};
      // Set the status:
      if (!has_inet_host) {
        status_code = EVENTLOG_SOCKET_ERROR_CNF_NOHOST;
      } else if (!has_inet_port) {
        status_code = EVENTLOG_SOCKET_ERROR_CNF_NOPORT;
      } else {
        status_code = EVENTLOG_SOCKET_OK;
      }
    }
  }

  // If an output address was provided for the options:
  if (eventlog_socket_opts_out != NULL) {
    eventlog_socket_opts_init(eventlog_socket_opts_out);
    eventlog_socket_opts_out->eso_wait =
        getenv(EVENTLOG_SOCKET_ENV_WAIT) != NULL; // NOLINT
  }
  return STATUS_FROM_CODE(status_code);
}
