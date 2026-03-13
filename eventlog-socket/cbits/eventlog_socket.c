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

#ifdef EVENTLOG_SOCKET_FEATURE_CONTROL
#include "./eventlog_socket/control.h"
#endif /* EVENTLOG_SOCKET_FEATURE_CONTROL */

#include "./eventlog_socket/debug.h"
#include "./eventlog_socket/error.h"
#include "./eventlog_socket/init_state.h"
#include "./eventlog_socket/string.h"
#include "./eventlog_socket/worker.h"
#include "./eventlog_socket/write_buffer.h"
#include "eventlog_socket.h"

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

// Worker thread handle.
static pthread_t *g_worker_thread_ptr = NULL;

// Control thread handle.
#ifdef EVENTLOG_SOCKET_FEATURE_CONTROL
static pthread_t *g_control_thread_ptr = NULL;
#endif /* EVENTLOG_SOCKET_FEATURE_CONTROL */

/// @brief The condition on which to wait for the signal that a new connection
/// has been established.
static pthread_cond_t g_new_connection_cond = PTHREAD_COND_INITIALIZER;

/// @brief The condition on which to wait for the signal that the GHC RTS is
/// ready.
static pthread_cond_t g_ghc_rts_ready_cond = PTHREAD_COND_INITIALIZER;

// Global mutex guarding all shared state between RTS threads, the worker
// thread, and the detached control receiver. Only client_fd and wt need
// protection, but using a single mutex ensures we keep their updates
// consistent.
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

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

/// @brief This variable is used to track the state of the eventlog writer.
///
/// 1. Whether or not `eventlog_socket_init` was called. This function
///    allocates resources that are used throughout the lifetime of the
///    process and it should not be called twice.
///
/// 2. Whether or not `SocketEventLogWriter` was attached in a C main.
///    If it was, then we don't want to restart event logging when the first
///    client connects, because that would drop all buffered events.
///
///    **NOTE**: While our advertised purpose for a C main is the ability to
///    attach an `SocketEventLogWriter`, it's possible to write a C main that
///    only calls `eventlog_socket_init` and
///    `eventlog_socket_signal_ghc_rts_ready`, which behaves exactly like
///    `eventlog_socket_start`.
///
/// The state tracks four signals:
///
/// 1. `EVENTLOG_SOCKET_SIG_INITIALIZED` is set when `eventlog_socket_init` is
/// called.
/// 2. `EVENTLOG_SOCKET_SIG_ATTACHED` is set when `SocketEventLogWriter` member
/// @c initEventLogWriter is called.
/// 3. `EVENTLOG_SOCKET_SIG_RTS_READY` is set when
/// `eventlog_socket_signal_ghc_rts_ready` is called.
/// 4. `EVENTLOG_SOCKET_SIG_HAD_FIRST_CONNECTION` is set when the first client
/// connects to the eventlog socket.
///
/// If `SocketEventLogWriter` was attached in a C main, then these signals are
/// received in order (1, 2, 3, 4).
///
/// If `eventlog_socket_start` is used, then these signals are received in
/// order (1, 4, 3, 2).
///
static volatile EventlogSocketInitState g_init_state = 0;

/*********************************************************************************
 * EventLogWriter
 *********************************************************************************/

static void writer_init(void) {
  DEBUG_DEBUG("%s", "Attached eventlog-socket writer.");
  pthread_mutex_lock(&g_mutex);
  g_init_state |= EVENTLOG_SOCKET_SIG_ATTACHED;
  pthread_mutex_unlock(&g_mutex);
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
    es_worker_wake();
  }
}

static bool writer_write(void *eventlog, size_t size) {
  DEBUG_TRACE("size: %lu", size);
  // Serialize against worker/control threads so that client_fd and wt are read
  // atomically with respect to connection establishment/teardown.
  pthread_mutex_lock(&g_mutex);
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
  // Mark SocketEventLogWriter as detached.
  g_init_state &= ~EVENTLOG_SOCKET_SIG_ATTACHED;

  // Close the connection.
  if (g_client_fd >= 0) {
    close(g_client_fd);
    g_client_fd = -1;
    write_buffer_free(&g_write_buffer);
  }
  pthread_mutex_unlock(&g_mutex);
}

/// @brief The eventlog socket writer.
///
/// @warning It is only safe to pass this value to the GHC RTS *after*
/// `eventlog_socket_init` or `eventlog_socket_start` is called.
const EventLogWriter SocketEventLogWriter = {.initEventLogWriter = writer_init,
                                             .writeEventLog = writer_write,
                                             .flushEventLog = writer_flush,
                                             .stopEventLogWriter = writer_stop};

/*********************************************************************************
 * Main worker (in own thread)
 *********************************************************************************/

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

  // Check if this version of the GHC RTS supports the eventlog.
  if (eventLogStatus() == EVENTLOG_NOT_SUPPORTED) {
    DEBUG_ERROR("%s", "eventlog is not supported.");
    return STATUS_FROM_CODE(EVENTLOG_SOCKET_ERR_RTS_NOSUPPORT);
  }

  // Start worker thread.
  g_worker_thread_ptr = (pthread_t *)malloc(sizeof(pthread_t));
  if (g_worker_thread_ptr == NULL) {
    return STATUS_FROM_ERRNO(); // `malloc` sets errno.
  }
  const WorkerState worker_state = {
      .worker_thread_ptr = g_worker_thread_ptr,
      .client_fd_ptr = &g_client_fd,
      .write_buffer_ptr = &g_write_buffer,
      .mutex_ptr = &g_mutex,
      .init_state_ptr = &g_init_state,
      .new_connection_cond_ptr = &g_new_connection_cond,
      .ghc_rts_ready_cond_ptr = &g_ghc_rts_ready_cond,
  };
  RETURN_ON_ERROR(es_worker_init(worker_state));
  RETURN_ON_ERROR(es_worker_start(eventlog_socket_addr, eventlog_socket_opts));

  // Start control thread.
#ifdef EVENTLOG_SOCKET_FEATURE_CONTROL
  g_control_thread_ptr = (pthread_t *)malloc(sizeof(pthread_t));
  if (g_control_thread_ptr == NULL) {
    return STATUS_FROM_ERRNO(); // `malloc` sets errno.
  }
  const ControlState control_state = {
      .control_thread_ptr = g_control_thread_ptr,
      .client_fd_ptr = &g_client_fd,
      .mutex_ptr = &g_mutex,
      .init_state_ptr = &g_init_state,
      .new_connection_cond_ptr = &g_new_connection_cond,
      .ghc_rts_ready_cond_ptr = &g_ghc_rts_ready_cond};
  RETURN_ON_ERROR(control_start(control_state));
#endif /* EVENTLOG_SOCKET_FEATURE_CONTROL */

  // Wait for a connection.
  if (eventlog_socket_opts->eso_wait) {
    DEBUG_DEBUG("%s", "Waiting for a connection...");
    RETURN_ON_ERROR(eventlog_socket_wait());
  }

  return STATUS_FROM_CODE(EVENTLOG_SOCKET_OK);
}

/* PUBLIC - see documentation in eventlog_socket.h */
EventlogSocketStatus eventlog_socket_wait(void) {
  // Condition variable pairs with the mutex so reader threads can wait for the
  // worker to publish a connected client_fd atomically.
  {
    const int success_or_errno = pthread_mutex_lock(&g_mutex);
    if (success_or_errno != 0) {
      return STATUS_FROM_PTHREAD_ERROR(success_or_errno);
    }
  }
  DEBUG_TRACE("initial client_fd=%d", g_client_fd);
  while (g_client_fd == -1) {
    DEBUG_TRACE("%s", "blocking for connection");
    const int success_or_errno =
        pthread_cond_wait(&g_new_connection_cond, &g_mutex);
    if (success_or_errno != 0) {
      return STATUS_FROM_PTHREAD_ERROR(success_or_errno);
    }
    DEBUG_TRACE("woke up, client_fd=%d", g_client_fd);
  }
  DEBUG_TRACE("proceeding with client_fd=%d", g_client_fd);
  {
    const int success_or_errno = pthread_mutex_unlock(&g_mutex);
    if (success_or_errno != 0) {
      return STATUS_FROM_PTHREAD_ERROR(success_or_errno);
    }
  }
  return STATUS_FROM_CODE(EVENTLOG_SOCKET_OK);
}

/* PUBLIC - see documentation in eventlog_socket.h */
RtsConfig eventlog_socket_attach_rts_config(RtsConfig rts_config) {
  rts_config.eventlog_writer = &SocketEventLogWriter;
  return rts_config;
}

/* PUBLIC - see documentation in eventlog_socket.h */
EventlogSocketStatus eventlog_socket_signal_ghc_rts_ready(void) {
  DEBUG_DEBUG("%s", "Received signal that the GHC RTS is ready.");

  // Acquire a lock on the global mutex.
  {
    const int success_or_errno = pthread_mutex_lock(&g_mutex);
    if (success_or_errno != 0) {
      return STATUS_FROM_PTHREAD_ERROR(success_or_errno);
    }
  }
  // If the RTS is not marked as ready...
  if (!(g_init_state & EVENTLOG_SOCKET_SIG_RTS_READY)) {
    // ...broadcast on the relevant condition and...
    const int success_or_errno = pthread_cond_broadcast(&g_ghc_rts_ready_cond);
    if (success_or_errno != 0) {
      pthread_mutex_unlock(&g_mutex);
      return STATUS_FROM_PTHREAD_ERROR(success_or_errno);
    }
    // ...mark the GHC RTS as ready.
    g_init_state |= EVENTLOG_SOCKET_SIG_RTS_READY;
  }
  // Release the log on the global mutex.
  {
    const int success_or_errno = pthread_mutex_unlock(&g_mutex);
    if (success_or_errno != 0) {
      return STATUS_FROM_PTHREAD_ERROR(success_or_errno);
    }
  }
  return STATUS_FROM_CODE(EVENTLOG_SOCKET_OK);
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

/// @brief Show an `EventLogStatus`.
const char *showEventLogStatus(enum EventLogStatus status) {
  switch (status) {
  case EVENTLOG_NOT_SUPPORTED:
    return "EVENTLOG_NOT_SUPPORTED";
  case EVENTLOG_NOT_CONFIGURED:
    return "EVENTLOG_NOT_CONFIGURED";
  case EVENTLOG_RUNNING:
    return "EVENTLOG_RUNNING";
  }
}

/* PUBLIC - see documentation in eventlog_socket.h */
EventlogSocketStatus
eventlog_socket_start(const EventlogSocketAddr *eventlog_socket_addr,
                      const EventlogSocketOpts *eventlog_socket_opts) {

  // If eventlog_socket_opts->eso_wait is set, mask it with false.
  // This is so that we deliver on the promised init order.
  // For this, we make a local copy of eventlog_socket_opts.
  EventlogSocketOpts eventlog_socket_opts_copy = {0};
  memcpy(&eventlog_socket_opts_copy, eventlog_socket_opts,
         sizeof(EventlogSocketOpts));
  eventlog_socket_opts_copy.eso_wait = false;

  // Initialize eventlog_socket with the local copy.
  RETURN_ON_ERROR(
      eventlog_socket_init(eventlog_socket_addr, &eventlog_socket_opts_copy));

  // Signal that the GHC RTS is ready.
  RETURN_ON_ERROR(eventlog_socket_signal_ghc_rts_ready());

  // If eventlog_socket_opts->eso_wait is set, wait for a connection.
  if (eventlog_socket_opts->eso_wait) {
    DEBUG_DEBUG("%s", "Waiting for a connection...");
    RETURN_ON_ERROR(eventlog_socket_wait());
  }

  return STATUS_FROM_CODE(EVENTLOG_SOCKET_OK);
}

/* PUBLIC - see documentation in eventlog_socket.h */
void eventlog_socket_opts_init(EventlogSocketOpts *eventlog_socket_opts) {
  if (eventlog_socket_opts == NULL) {
    return;
  }
  eventlog_socket_opts->eso_wait = false;
  eventlog_socket_opts->eso_sndbuf = 0;
  eventlog_socket_opts->eso_linger = 0;
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
  EventlogSocketStatusCode status_code = EVENTLOG_SOCKET_ERR_ENV_NOADDR;

  // Try to construct a Unix domain socket address:
  char *unix_path = getenv(EVENTLOG_SOCKET_ENV_UNIX_PATH); // NOLINT
  if (unix_path != NULL) {
    // Determine the maximum length of a Unix domain socket path.
    const size_t unix_path_max = get_unix_path_max();

    // Check that unix_path does not exceed the maximum unix_path length:
    const size_t unix_path_len = strlen(unix_path);
    if (unix_path_len > unix_path_max) {
      status_code = EVENTLOG_SOCKET_ERR_ENV_TOOLONG;
    }

    // Write the configuration:
    char *unix_path_copy = ess_strndup(unix_path_len, unix_path);
    if (unix_path_copy == NULL) {
      return STATUS_FROM_ERRNO(); // `ess_strndup` sets errno.
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
                                 ? ess_strndup(strlen(inet_host), inet_host)
                                 : ess_strndup(0, "");
      if (inet_host_copy == NULL) {
        return STATUS_FROM_ERRNO(); // `ess_strndup` sets errno.
      }
      // Copy the inet_port:
      char *inet_port_copy = (has_inet_port)
                                 ? ess_strndup(strlen(inet_port), inet_port)
                                 : ess_strndup(0, "");
      if (inet_port_copy == NULL) {
        free(inet_host_copy);
        return STATUS_FROM_ERRNO(); // `ess_strndup` sets errno.
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
        status_code = EVENTLOG_SOCKET_ERR_ENV_NOHOST;
      } else if (!has_inet_port) {
        status_code = EVENTLOG_SOCKET_ERR_ENV_NOPORT;
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

/* PUBLIC - see documentation in eventlog_socket.h */
const char *eventlog_socket_control_strnamespace(
    EventlogSocketControlNamespace *namespace) {
#ifdef EVENTLOG_SOCKET_FEATURE_CONTROL
  return control_strnamespace(namespace);
#else
  (void)namespace;
  return NULL;
#endif /* EVENTLOG_SOCKET_FEATURE_CONTROL */
}

/* PUBLIC - see documentation in eventlog_socket.h */
EventlogSocketStatus eventlog_socket_control_register_namespace(
    uint8_t namespace_len, const char namespace[namespace_len],
    EventlogSocketControlNamespace **namespace_out) {
#ifdef EVENTLOG_SOCKET_FEATURE_CONTROL
  return control_register_namespace(namespace_len, namespace, namespace_out);
#else
  (void)namespace_len;
  (void)namespace;
  (void)namespace_out;
  return STATUS_FROM_CODE(EVENTLOG_SOCKET_ERR_CTL_NOSUPPORT);
#endif /* EVENTLOG_SOCKET_FEATURE_CONTROL */
}

/* PUBLIC - see documentation in eventlog_socket.h */
EventlogSocketStatus eventlog_socket_control_register_command(
    EventlogSocketControlNamespace *namespace,
    EventlogSocketControlCommandId command_id,
    EventlogSocketControlCommandHandler *command_handler,
    const void *command_data) {
#ifdef EVENTLOG_SOCKET_FEATURE_CONTROL
  return control_register_command(namespace, command_id, command_handler,
                                  command_data);
#else
  (void)namespace;
  (void)command_id;
  (void)command_handler;
  (void)command_data;
  return STATUS_FROM_CODE(EVENTLOG_SOCKET_ERR_CTL_NOSUPPORT);
#endif /* EVENTLOG_SOCKET_FEATURE_CONTROL */
}
