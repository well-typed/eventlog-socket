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

#include "./debug.h"
#include "./error.h"
#include "./init_state.h"
#include "./poll.h"
#include "./string.h"
#include "./worker.h"

#define LISTEN_BACKLOG 5

#ifndef NI_MAXHOST
#define NI_MAXHOST 1025
#endif
#ifndef NI_MAXSERV
#define NI_MAXSERV 32
#endif

// variables read and written by worker only:
static int g_listen_fd = -1;
static const char *g_sock_path = NULL;
static int g_wake_pipe[2] = {-1, -1};

/// @brief The state that is shared with the worker thread.
///
/// **NOTE**: Initialising with @c {0} should guarantee that all pointers are
/// NULL pointers.
static WorkerState g_worker_state = {0};

/// @brief Wait for the signal that the GHC RTS is ready.
static void es_worker_wait_rts_ready(void) {
  assert(g_worker_state.init_state_ptr != NULL);
  assert(g_worker_state.mutex_ptr != NULL);
  assert(g_worker_state.ghc_rts_ready_cond_ptr != NULL);
  pthread_mutex_lock(g_worker_state.mutex_ptr);
  while (true) {
    // Check whether or not the GHC RTS is ready.
    DEBUG_DEBUG("%s", "Checking if GHC RTS is ready.");
    const EventlogSocketInitState init_state = *g_worker_state.init_state_ptr;
    DEBUG_TRACE("init_state: %d", init_state);
    const bool is_rts_ready = init_state & EVENTLOG_SOCKET_SIG_RTS_READY;
    DEBUG_TRACE("is_rts_ready: %s", is_rts_ready ? "true" : "false");
    if (is_rts_ready) {
      // If the GHC RTS is ready, break from the loop.
      break;
    } else {
      // If the GHC RTS is NOT ready, wait on the relevant condition and
      // re-enter the loop.
      //
      // NOTE: This call acts as the cancellation point for this infinite loop.
      DEBUG_DEBUG("%s", "Waiting for signal that GHC RTS is ready.");
      pthread_cond_wait(g_worker_state.ghc_rts_ready_cond_ptr,
                        g_worker_state.mutex_ptr);
    }
  }
  pthread_mutex_unlock(g_worker_state.mutex_ptr);
}

/// @brief Cleanup handler for the worker thread.
///
/// TODO: Move the first portion of this handler into a pthread_cleanup_push
/// handler.
static void es_worker_cleanup(void) {
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
  // Stop the worker thread.
  if (g_worker_state.worker_thread_ptr != NULL) {
    DEBUG_DEBUG("%s", "Cancelling worker thread.");
    if (pthread_cancel(*g_worker_state.worker_thread_ptr) != 0) {
      DEBUG_ERRNO("pthread_cancel() failed for worker thread");
    } else {
      if (pthread_join(*g_worker_state.worker_thread_ptr, NULL) != 0) {
        DEBUG_ERRNO("pthread_join() failed for worker thread");
      }
    }
    free((void *)g_worker_state.worker_thread_ptr);
  }
}

#define EVENTLOG_SOCKET_WORKER_CHUNK_SIZE 32

static void es_worker_wake_drain(void) {
  if (g_wake_pipe[0] == -1) {
    return;
  }
  uint8_t chunk[EVENTLOG_SOCKET_WORKER_CHUNK_SIZE];
  while (true) {
    // NOTE: The call to read acts as the cancellation point for this infinite
    // loop.
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

/* HIDDEN - see documentation in worker.h */
HIDDEN void es_worker_wake(void) {
  if (g_wake_pipe[1] == -1) {
    return;
  }

  const uint8_t byte = 1;
  const ssize_t success_or_error = write(g_wake_pipe[1], &byte, sizeof(byte));
  if (success_or_error == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
    DEBUG_ERRNO("write() failed");
  }
}

static void es_worker_step_listen(void) {
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
    // NOTE: The call to poll acts as the cancellation point for this infinite
    // loop.
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

  // Wait for the GHC RTS to be ready.
  es_worker_wait_rts_ready();

  // Figure out whether we should restart event logging.
  //
  // Do we need this mutex? Who else can edit `*g_worker_state.init_state_ptr`?
  //
  // - It is edited by `worker_init`, which should not run a second time.
  //   Moreover, `worker_init` is monotone.
  //
  // - It is edited by this thread. Since the calls to listen and accept are
  //   in this very function, there is no risk of a second connection being
  //   accepted as this is being evaluated.
  //
  // - It is edited by `worker_stop`. This could happen if another user thread
  //   concurrently decides to end event logging. This COULD happen, but only
  //   if the user is doing some shady things that we aren't anticipating.
  //
  // - It is edited by `eventlog_socket_signal_rts_ready`. This can happen if
  //   the user signals that the GHC RTS is ready while a new client connects.
  //   This is actually VERY LIKELY to happen.
  //
  // If `startEventLogging` is called before the GHC RTS is initialised, the
  // call completes _successfully_, but the event log writer is overwritten
  // when the GHC RTS is initialised without calling `endEventLogging`.
  //
  pthread_mutex_lock(g_worker_state.mutex_ptr);
  const EventlogSocketInitState init_state = *g_worker_state.init_state_ptr;
  // TODO: Wait for RTS init.
  pthread_mutex_unlock(g_worker_state.mutex_ptr);
  DEBUG_TRACE("init_state: %d", init_state);

  // Whether the RTS is currently initialised.
  const bool is_rts_ready = init_state & EVENTLOG_SOCKET_SIG_RTS_READY;
  DEBUG_TRACE("is_rts_ready: %s", is_rts_ready ? "true" : "false");

  // Whether SocketEventLogWriter is currently attached.
  const bool is_attached = init_state & EVENTLOG_SOCKET_SIG_ATTACHED;
  DEBUG_TRACE("is_attached: %s", is_attached ? "true" : "false");

  // Whether we've had our first connection.
  const bool had_first_connection =
      init_state & EVENTLOG_SOCKET_SIG_HAD_FIRST_CONNECTION;
  DEBUG_TRACE("had_first_connection: %s", is_attached ? "true" : "false");

  // Whether event logging is currently running.
  const bool is_running = eventLogStatus() == EVENTLOG_RUNNING;
  DEBUG_TRACE("is_running: %s", is_running ? "true" : "false");

  // We should STOP event logging if...
  const bool should_stop =
      // ...some eventlog writer is currently running and either:
      is_running && (
                        // ...(1) it's not SocketEventLogWriter, or...
                        !is_attached ||
                        // ...(2) we've already had our first connection.
                        //
                        // NOTE: This forces the RTS to resent the init events.
                        had_first_connection);
  DEBUG_TRACE("should_stop: %s", should_stop ? "true" : "false");

  // We should START event logging if...
  const bool should_start =
      // ...(1) we stopped it, or...
      should_stop
      // ...(2) it wasn't running in the first place...
      || !is_running;
  DEBUG_TRACE("should_start: %s", should_start ? "true" : "false");

  // We stop event logging.
  if (should_stop) {
    DEBUG_DEBUG("%s", "Stopping current event logger.");
    endEventLogging();
  }

  pthread_mutex_lock(g_worker_state.mutex_ptr);
  // Publish the client ID to `g_client_id`.
  DEBUG_TRACE("publishing client_fd=%d (previous=%d)", client_fd,
              *g_worker_state.client_fd_ptr);
  *g_worker_state.client_fd_ptr = client_fd;
  // Trigger the `g_new_conn_cond` condition.
  pthread_cond_broadcast(g_worker_state.new_connection_cond_ptr);
  pthread_mutex_unlock(g_worker_state.mutex_ptr);

  // We start event logging with SocketEventLogWriter.
  if (should_start) {
    DEBUG_DEBUG("%s", "Starting new event logger.");
    // TODO: Add retry loop.
    const bool is_started = startEventLogging(&SocketEventLogWriter);
    DEBUG_TRACE("is_started: %s", is_started ? "true" : "false");
  }

  // Update *g_worker_state.init_state_ptr to record that we've seen our first
  // connection.
  if (!had_first_connection) {
    pthread_mutex_lock(g_worker_state.mutex_ptr);
    *g_worker_state.init_state_ptr |= EVENTLOG_SOCKET_SIG_HAD_FIRST_CONNECTION;
    pthread_mutex_unlock(g_worker_state.mutex_ptr);
  }
}

// nothing to write iteration.
//
// we poll only for whether the connection is closed.
static void es_worker_step_nonwrite(int fd) {
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
    es_worker_wake_drain();
    return;
  }

  if (pfds[0].revents & POLLHUP) {
    DEBUG_TRACE("(%d) POLLRDHUP", fd);

    pthread_mutex_lock(g_worker_state.mutex_ptr);
    *g_worker_state.client_fd_ptr = -1;
    es_write_buffer_free(g_worker_state.write_buffer_ptr);
    pthread_mutex_unlock(g_worker_state.mutex_ptr);
    return;
  }
}

// write iteration.
//
// we poll for both: can we write, and whether the connection is closed.
static void es_worker_step_write(int fd) {
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
    pthread_mutex_lock(g_worker_state.mutex_ptr);
    assert(fd == *g_worker_state.client_fd_ptr);
    *g_worker_state.client_fd_ptr = -1;
    es_write_buffer_free(g_worker_state.write_buffer_ptr);
    pthread_mutex_unlock(g_worker_state.mutex_ptr);
    return;
  }

  if (pfds[0].revents & POLLOUT) {
    DEBUG_TRACE("(%d) POLLOUT", fd);

    // RTS writers also access wt, so consume queued buffers under the mutex.
    pthread_mutex_lock(g_worker_state.mutex_ptr);
    while (g_worker_state.write_buffer_ptr->head) {
      WriteBufferItem *item = g_worker_state.write_buffer_ptr->head;
      const ssize_t num_bytes_written_or_err =
          write(*g_worker_state.client_fd_ptr, item->data, item->size);

      if (num_bytes_written_or_err == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          // couldn't write anything, shouldn't happen.
          // do nothing.
        } else if (errno == EPIPE) {
          *g_worker_state.client_fd_ptr = -1;
          es_write_buffer_free(g_worker_state.write_buffer_ptr);
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
          es_write_buffer_pop(g_worker_state.write_buffer_ptr);
          continue;
        } else {
          item->size -= num_bytes_written;
          item->data += num_bytes_written;
          break;
        }
      }
    }
    pthread_mutex_unlock(g_worker_state.mutex_ptr);
  }
}

static void es_worker_step(void) {
  // Snapshot shared state under lock so worker decisions (listen vs write)
  // align with the current connection/queue state.
  pthread_mutex_lock(g_worker_state.mutex_ptr);
  const int client_fd = *g_worker_state.client_fd_ptr;
  const bool write_buffer_empty = g_worker_state.write_buffer_ptr->head == NULL;
  pthread_mutex_unlock(g_worker_state.mutex_ptr);
  if (client_fd != -1) {
    if (write_buffer_empty) {
      es_worker_step_nonwrite(client_fd);
    } else {
      es_worker_step_write(client_fd);
    }
  } else {
    es_worker_step_listen();
  }
}

/* Main loop of eventlog-socket own thread:
 * Currently it is two states:
 * - either we have connection, then we poll for writes (and drop of
 * connection).
 * - or we don't have, then we poll for accept.
 */
static void *es_worker_loop(void *arg) {
  (void)arg;
  while (true) {
    // Ensure the loop has a cancellation point.
    pthread_testcancel();

    // Perform one worker step.
    es_worker_step();
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
es_worker_socket_init_unix(const EventlogSocketUnixAddr *const unix_addr,
                           const EventlogSocketOpts *const opts) {
  DEBUG_TRACE("init Unix listener: %s", unix_addr->esa_unix_path);

  // Create a socket.
  g_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (g_listen_fd == -1) {
    return STATUS_FROM_ERRNO(); // `setsockopt` sets errno.
  }

  // Record the sock_path so it can be unlinked at exit
  g_sock_path = es_strdup(unix_addr->esa_unix_path);
  if (g_sock_path == NULL) {
    return STATUS_FROM_ERRNO(); // `es_strdup` sets errno.
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

  // Set socket linger.
  if (opts != NULL && opts->eso_linger > 0) {
    const struct linger so_linger = {
        .l_onoff = true,
        .l_linger = opts->eso_linger,
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
es_worker_socket_init_inet(const EventlogSocketInetAddr *const inet_addr,
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
/// If `EVENTLOG_SOCKET_ERR_SYS` is returned, @c error_code is set to one of
/// the following error codes:
///
/// `EACCES`, `EAGAIN`, `EBADF`, `EFAULT`, `EINVAL`, `EMFILE`, `ENFILE`,
/// `ENFILE`, or `ENOPKG`.
/// @endparblock
HIDDEN EventlogSocketStatus es_worker_init(const WorkerState worker_state) {
  assert(worker_state.worker_thread_ptr != NULL);
  assert(worker_state.client_fd_ptr != NULL);
  assert(worker_state.write_buffer_ptr != NULL);
  assert(worker_state.mutex_ptr != NULL);
  assert(worker_state.init_state_ptr != NULL);
  assert(worker_state.new_connection_cond_ptr != NULL);
  assert(worker_state.ghc_rts_ready_cond_ptr != NULL);
  DEBUG_DEBUG("%s", "Initialising worker thread.");
  memcpy(&g_worker_state, &worker_state, sizeof(WorkerState));
  assert(g_worker_state.worker_thread_ptr != NULL);
  assert(g_worker_state.client_fd_ptr != NULL);
  assert(g_worker_state.write_buffer_ptr != NULL);
  assert(g_worker_state.mutex_ptr != NULL);
  assert(g_worker_state.init_state_ptr != NULL);
  assert(g_worker_state.new_connection_cond_ptr != NULL);
  assert(g_worker_state.ghc_rts_ready_cond_ptr != NULL);
  {
    const int success_or_errno = pthread_mutex_lock(g_worker_state.mutex_ptr);
    if (success_or_errno != 0) {
      return STATUS_FROM_PTHREAD_ERROR(success_or_errno);
    }
  }
  if (!(*g_worker_state.init_state_ptr & EVENTLOG_SOCKET_SIG_INITIALIZED)) {
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
    atexit(es_worker_cleanup);
    *g_worker_state.init_state_ptr |= EVENTLOG_SOCKET_SIG_INITIALIZED;
  }
  {
    const int success_or_errno = pthread_mutex_unlock(g_worker_state.mutex_ptr);
    if (success_or_errno != 0) {
      return STATUS_FROM_PTHREAD_ERROR(success_or_errno);
    }
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
/// If `EVENTLOG_SOCKET_ERR_GAI` is returned, @c error_code is set to one of
/// the following error codes:
///
/// `EAI_ADDRFAMILY`, `EAI_AGAIN`, `EAI_BADFLAGS`, `EAI_FAIL`, `EAI_FAMILY`,
/// `EAI_MEMORY`, `EAI_NODATA`, `EAI_NONAME`, `EAI_SERVICE`, `EAI_SOCKTYPE`, or
/// `EAI_SYSTEM`.
///
/// If `EVENTLOG_SOCKET_ERR_SYS` is returned, @c error_code is set to one of
/// the following error codes:
///
/// `EACCES`, `EADDRINUSE`, `EADDRNOTAVAIL`, `EAFNOSUPPORT`, `EAGAIN`,
/// `EALREADY`, `EBADF`, `EDESTADDRREQ`, `EDOM`, `EFAULT`, `EINPROGRESS`,
/// `EINVAL`, `EIO`, `EISCONN`, `EISDIR`, `ELOOP`, `EMFILE`, `ENAMETOOLONG`,
/// `ENFILE`, `ENOBUFS`, `ENOENT`, `ENOMEM`, `ENOPKG`, `ENOPROTOOPT`, `ENOTDIR`,
/// `ENOTSOCK`, `EOPNOTSUPP`, `EPERM`, `EPROTONOSUPPORT`, or `EROFS`.
/// @endparblock
HIDDEN EventlogSocketStatus
es_worker_start(const EventlogSocketAddr *const eventlog_socket_addr,
                const EventlogSocketOpts *const eventlog_socket_opts) {
  // Bind the eventlog socket.
  DEBUG_TRACE("%s", "Binding eventlog socket.");
  switch (eventlog_socket_addr->esa_tag) {
  case EVENTLOG_SOCKET_UNIX: {
    RETURN_ON_ERROR(es_worker_socket_init_unix(
        &eventlog_socket_addr->esa_unix_addr, eventlog_socket_opts));
    break;
  }
  case EVENTLOG_SOCKET_INET: {
    RETURN_ON_ERROR(es_worker_socket_init_inet(
        &eventlog_socket_addr->esa_inet_addr, eventlog_socket_opts));
    break;
  }
  default: {
    DEBUG_ERROR("%s", "unknown listener kind");
    errno = EINVAL;
    return STATUS_FROM_ERRNO();
  }
  }

  // Start the worker thread.
  DEBUG_TRACE("%s", "Starting worker thread.");
  const int success_or_errno = pthread_create(g_worker_state.worker_thread_ptr,
                                              NULL, es_worker_loop, NULL);
  if (success_or_errno != 0) {
    DEBUG_ERRNO("pthread_create() failed");
    return STATUS_FROM_PTHREAD_ERROR(success_or_errno);
  }
  return STATUS_FROM_CODE(EVENTLOG_SOCKET_OK);
}
