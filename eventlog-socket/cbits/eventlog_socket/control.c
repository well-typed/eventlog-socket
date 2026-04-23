/// @file      control.c
/// @brief     The control thread.
/// @details   This module defines the control thread, which listens on the
///            eventlog socket and handles incoming control command requests.
///            Control commands can be registered using the public C API.
///            See `eventlog_socket_control_register_namespace`
///            and `eventlog_socket_control_register_command`.
/// @author    Wen Kokke
/// @author    Matthew Pickering
/// @version   0.1.3.0
/// @date      2025-2026
/// @copyright BSD-3-Clause License.
///

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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

#include "./control.h"
#include "./debug.h"
#include "./error.h"
#include "./poll.h"
#include "eventlog_socket.h"
#include "init_state.h"
#include "rts/EventLogWriter.h"

// CONTROL_MAGIC should be the UTF-8 encoding of some code point between
// U+010000 and U+10FFFF. Let's pick code point U+01E5CC, for Eventlog
// 5oscket Control Command. That's:
//
// Unicode
//
//   0    1    E    5    C    C
//   u    vvvv wwww xxxx yyyy zzzz
//   0    0001 1110 0101 1100 1100
//
// UTF-8
//
//   1111 0uvv 10vv wwww 10xx xxyy 10yy zzzz
//   1111 0000 1001 1110 1001 0111 1000 1100
//   F    0    9    E    9    7    8    C
//
// To validate this, you can use the following Python expression:
//
// chr(0x01E5CC).encode("utf-8")
// => b'\xf0\x9e\x97\x8c'
//

/// @brief The length of the magic bytestring that starts a control protocol
/// message.
///
/// See `g_control_magic`.
#define CONTROL_MAGIC_LEN 4

/// @brief The magic bytestring that starts a control protocol message.
static const uint8_t g_control_magic[CONTROL_MAGIC_LEN] = {
    [0] = 0xF0,
    [1] = 0x9E,
    [2] = 0x97,
    [3] = 0x8C,
};

/// @brief The name of the builtin namespace.
#define BUILTIN_NAMESPACE "eventlog-socket"

/// @brief The ID of the builtin `StartProfiling` command.
#define BUILTIN_COMMAND_ID_START_PROFILING 1

/// @brief The ID of the builtin `StopProfiling` command.
#define BUILTIN_COMMAND_ID_STOP_PROFILING 2

/// @brief The ID of the builtin `StartHeapProfiling` command.
#define BUILTIN_COMMAND_ID_START_HEAP_PROFILING 3

/// @brief The ID of the builtin `StopHeapProfiling` command.
#define BUILTIN_COMMAND_ID_STOP_HEAP_PROFILING 4

/// @brief The ID of the builtin `RequestHeapCensus` command.
#define BUILTIN_COMMAND_ID_REQUEST_HEAP_CENSUS 5

/// @brief The ID of the builtin `StartSchedulerTracing` command.
#define BUILTIN_COMMAND_ID_START_SCHEDULER_TRACING 6

/// @brief The ID of the builtin `StopSchedulerTracing` command.
#define BUILTIN_COMMAND_ID_STOP_SCHEDULER_TRACING 7

/// @brief The ID of the builtin `StartGcTracing` command.
#define BUILTIN_COMMAND_ID_START_GC_TRACING 8

/// @brief The ID of the builtin `StopGcTracing` command.
#define BUILTIN_COMMAND_ID_STOP_GC_TRACING 9

/// @brief The ID of the builtin `StartNonmovingGcTracing` command.
#define BUILTIN_COMMAND_ID_START_NONMOVING_GC_TRACING 10

/// @brief The ID of the builtin `StopNonmovingGcTracing` command.
#define BUILTIN_COMMAND_ID_STOP_NONMOVING_GC_TRACING 11

/// @brief The ID of the builtin `StartSparkSampledTracing` command.
#define BUILTIN_COMMAND_ID_START_SPARK_SAMPLED_TRACING 12

/// @brief The ID of the builtin `StopSparkSampledTracing` command.
#define BUILTIN_COMMAND_ID_STOP_SPARK_SAMPLED_TRACING 13

/// @brief The ID of the builtin `StartSparkFullTracing` command.
#define BUILTIN_COMMAND_ID_START_SPARK_FULL_TRACING 14

/// @brief The ID of the builtin `StopSparkFullTracing` command.
#define BUILTIN_COMMAND_ID_STOP_SPARK_FULL_TRACING 15

/// @brief The ID of the builtin `StartUserTracing` command.
#define BUILTIN_COMMAND_ID_START_USER_TRACING 16

/// @brief The ID of the builtin `StopUserTracing` command.
#define BUILTIN_COMMAND_ID_STOP_USER_TRACING 17

/// @brief The ID of the builtin `StartCapTracing` command.
#define BUILTIN_COMMAND_ID_START_CAP_TRACING 18

/// @brief The ID of the builtin `StopCapTracing` command.
#define BUILTIN_COMMAND_ID_STOP_CAP_TRACING 19

/******************************************************************************
 * Handlers for Builtin Commands
 ******************************************************************************/

/// @brief Handler for "StartProfiling" command (eventlog-socket::1).
static void es_control_start_profiling(
    const EventlogSocketControlNamespace *const namespace,
    const EventlogSocketControlCommandId command_id, const void *user_data) {
  (void)namespace;
  (void)command_id;
  (void)user_data;
  startProfTimer();
  DEBUG_DEBUG("%s", "Started profiling.");
}

/// @brief Handler for "StopProfiling" command (eventlog-socket::2).
static void
es_control_stop_profiling(const EventlogSocketControlNamespace *const namespace,
                          const EventlogSocketControlCommandId command_id,
                          const void *user_data) {
  (void)namespace;
  (void)command_id;
  (void)user_data;
  stopProfTimer();
  DEBUG_DEBUG("%s", "Stopped profiling.");
}

/// @brief Handler for "StartHeapProfiling" command (eventlog-socket::3).
static void es_control_start_heap_profiling(
    const EventlogSocketControlNamespace *const namespace,
    const EventlogSocketControlCommandId command_id, const void *user_data) {
  (void)namespace;
  (void)command_id;
  (void)user_data;
  startHeapProfTimer();
  DEBUG_DEBUG("%s", "Started heap profiling.");
}

/// @brief Handler for "StopHeapProfiling" command (eventlog-socket::4).
static void es_control_stop_heap_profiling(
    const EventlogSocketControlNamespace *const namespace,
    const EventlogSocketControlCommandId command_id, const void *user_data) {
  (void)namespace;
  (void)command_id;
  (void)user_data;
  stopHeapProfTimer();
  DEBUG_DEBUG("%s", "Stopped heap profiling.");
}

/// @brief Handler for "RequestHeapCensus" command (eventlog-socket::5).
static void es_control_request_heap_census(
    const EventlogSocketControlNamespace *const namespace,
    const EventlogSocketControlCommandId command_id, const void *user_data) {
  (void)namespace;
  (void)command_id;
  (void)user_data;
  requestHeapCensus();
  DEBUG_DEBUG("%s", "Requested heap census.");
}

/******************************************************************************
 * Dynamic Trace Flags (Supported by GHC>=10)
 ******************************************************************************/

// Dynamic trace flags are supported by GHC version 10 and later, but both the
// macros GHC_HAS_SET_TRACE_FLAG and FORCE_DYNAMIC_TRACE_FLAGS can be used to
// override the GHC version check and force support for dynamic trace flags.

#ifndef GHC_HAS_SET_TRACE_FLAG
#ifdef FORCE_DYNAMIC_TRACE_FLAGS
#define GHC_HAS_SET_TRACE_FLAG 1
#else /* FORCE_DYNAMIC_TRACE_FLAGS */
#ifdef MIN_VERSION_GLASGOW_HASKELL
#if MIN_VERSION_GLASGOW_HASKELL(10, 0, 0, 0)
#define GHC_HAS_SET_TRACE_FLAG 1
#endif /* MIN_VERSION_GLASGOW_HASKELL(10,0,0,0) */
#endif /* MIN_VERSION_GLASGOW_HASKELL */
#endif /* FORCE_DYNAMIC_TRACE_FLAGS */
#endif /* GHC_HAS_SET_TRACE_FLAG */

#ifdef GHC_HAS_SET_TRACE_FLAG

/// @brief A trace flag assignment.
typedef struct {
  const RUNTIME_TRACE_FLAG flag;
  const bool value;
} EventlogSocketRuntimeTraceFlagAssignment;

/// @brief Create a `EventlogSocketRuntimeTraceFlagAssignment` value.
#define RUNTIME_TRACE_FLAG_VALUE(my_flag, my_value)                            \
  (&(EventlogSocketRuntimeTraceFlagAssignment){.flag = (my_flag),              \
                                               .value = (my_value)})

/// @brief Show a trace flag name.
static char *
es_show_RUNTIME_TRACE_FLAG(const RUNTIME_TRACE_FLAG runtime_trace_flag) {
  switch (runtime_trace_flag) {
  case TRACE_SCHEDULER:
    return "TRACE_SCHEDULER";
  case TRACE_GC:
    return "TRACE_GC";
  case TRACE_NONMOVING_GC:
    return "TRACE_NONMOVING_GC";
  case TRACE_SPARK_SAMPLED:
    return "TRACE_SPARK_SAMPLED";
  case TRACE_SPARK_FULL:
    return "TRACE_SPARK_FULL";
  case TRACE_USER:
    return "TRACE_USER";
  case TRACE_CAP:
    return "TRACE_CAP";
  }
}

/// @brief Handler for "SetRuntimeTraceFlag" commands
/// (eventlog-socket::6-eventlog-socket::19).
static void es_control_set_runtime_trace_flag(
    const EventlogSocketControlNamespace *const namespace,
    const EventlogSocketControlCommandId command_id, const void *user_data) {
  (void)namespace;
  (void)command_id;
  const EventlogSocketRuntimeTraceFlagAssignment *const
      runtime_trace_flag_value = user_data;
  setTraceFlag(runtime_trace_flag_value->flag, runtime_trace_flag_value->value);
  DEBUG_DEBUG("%s %s.", runtime_trace_flag_value->value ? "Start" : "Stop",
              es_show_RUNTIME_TRACE_FLAG(runtime_trace_flag_value->flag));
}
#else /* GHC_HAS_SET_TRACE_FLAG */

/// @brief Always return @c NULL.
#define RUNTIME_TRACE_FLAG_VALUE(my_flag, my_value) NULL

// Shims for the values of the RUNTIME_TRACE_FLAG enum.
#define TRACE_SCHEDULER ((void)0)
#define TRACE_GC ((void)0)
#define TRACE_NONMOVING_GC ((void)0)
#define TRACE_SPARK_SAMPLED ((void)0)
#define TRACE_SPARK_FULL ((void)0)
#define TRACE_USER ((void)0)
#define TRACE_CAP ((void)0)

/// @brief Handler for "SetRuntimeTraceFlag" commands
/// (eventlog-socket::6-eventlog-socket::19).
static void es_control_set_runtime_trace_flag(
    const EventlogSocketControlNamespace *const namespace,
    const EventlogSocketControlCommandId command_id, const void *user_data) {
  (void)namespace;
  (void)command_id;
  (void)user_data;
  DEBUG_ERROR("%s",
              "This binary was built without support for dynamic trace flags.");
}
#endif /* GHC_HAS_SET_TRACE_FLAG */

/******************************************************************************
 * Namespace and Command Registry
 ******************************************************************************/

/// @brief An entry in the command registry.
///
/// See `eventlog_socket_control_command`.
typedef struct EventlogSocketControlCommand EventlogSocketControlCommand;

/// @brief An entry in the command registry.
///
/// The command registry is a linked-list of `EventlogSocketControlCommand`
/// values. Each `EventlogSocketControlCommand` entry should have a stable
/// address. Once `EventlogSocketControlNamespace::command_registry` or
/// `EventlogSocketControlCommand::next` are assigned a nonnull value, they
/// must never change.
struct EventlogSocketControlCommand {
  /// The user-provided command ID.
  const EventlogSocketControlCommandId command_id;
  /// The user-provided command handler.
  EventlogSocketControlCommandHandler *const command_handler;
  /// The user-provided data for the command handler.
  const void *command_data;
  /// The pointer to the next entry in the command registry.
  EventlogSocketControlCommand *next;
};

/// @brief An entry in the namespace registry.
///
/// See `eventlog_socket_control_namespace`.
typedef struct EventlogSocketControlNamespace EventlogSocketControlNamespace;

/// @brief An entry in the namespace registry.
///
/// The namespace registry is a linked-list of
/// `eventlog_socket_control_namespace` values. Each
/// `EventlogSocketControlCommand` entry should have a stable address. Once
/// `g_control_namespace_registry` or `eventlog_socket_control_namespace::next`
/// are assigned a nonnull value, they must never change.
struct EventlogSocketControlNamespace {
  /// The length of the user-provided namespace.
  const uint8_t namespace_len;
  /// The user-provided namespace.
  ///
  /// This must be a null-terminated string of length `namespace_len + 1`
  /// (including the null byte).
  const char *const namespace;
  /// The pointer to the next entry in the namespace registry.
  EventlogSocketControlNamespace *next;
  /// The pointer to the first entry in the command registry for this namespace.
  EventlogSocketControlCommand *command_registry;
};

/// @brief Create a builtin command registry entry.
#define BUILTIN_COMMAND(my_id, my_handler, my_data, my_next)                   \
  (&(EventlogSocketControlCommand){.command_id = (my_id),                      \
                                   .command_handler = (my_handler),            \
                                   .command_data = (my_data),                  \
                                   .next = (my_next)})

/// @brief The global namespace registry.
///
/// See `eventlog_socket_control_namespace`.
///
/// This is initialised with the builtin namespace and the builtin commands.
// clang-format off
static EventlogSocketControlNamespace g_control_namespace_registry = {
    .namespace = BUILTIN_NAMESPACE,
    .namespace_len = strlen(BUILTIN_NAMESPACE),
    .next = NULL,
    .command_registry =
    BUILTIN_COMMAND(
      BUILTIN_COMMAND_ID_START_PROFILING,
      es_control_start_profiling,
      NULL,
    BUILTIN_COMMAND(
      BUILTIN_COMMAND_ID_STOP_PROFILING,
      es_control_stop_profiling,
      NULL,
    BUILTIN_COMMAND(
      BUILTIN_COMMAND_ID_START_HEAP_PROFILING,
      es_control_start_heap_profiling,
      NULL,
    BUILTIN_COMMAND(
      BUILTIN_COMMAND_ID_STOP_HEAP_PROFILING,
      es_control_stop_heap_profiling,
      NULL,
    BUILTIN_COMMAND(
      BUILTIN_COMMAND_ID_REQUEST_HEAP_CENSUS,
      es_control_request_heap_census,
      NULL,
    BUILTIN_COMMAND(
      BUILTIN_COMMAND_ID_START_SCHEDULER_TRACING,
      es_control_set_runtime_trace_flag,
      RUNTIME_TRACE_FLAG_VALUE(TRACE_SCHEDULER, true),
    BUILTIN_COMMAND(
      BUILTIN_COMMAND_ID_STOP_SCHEDULER_TRACING,
      es_control_set_runtime_trace_flag,
      RUNTIME_TRACE_FLAG_VALUE(TRACE_SCHEDULER, false),
    BUILTIN_COMMAND(
      BUILTIN_COMMAND_ID_START_GC_TRACING,
      es_control_set_runtime_trace_flag,
      RUNTIME_TRACE_FLAG_VALUE(TRACE_GC, true),
    BUILTIN_COMMAND(
      BUILTIN_COMMAND_ID_STOP_GC_TRACING,
      es_control_set_runtime_trace_flag,
      RUNTIME_TRACE_FLAG_VALUE(TRACE_GC, false),
    BUILTIN_COMMAND(
      BUILTIN_COMMAND_ID_START_NONMOVING_GC_TRACING,
      es_control_set_runtime_trace_flag,
      RUNTIME_TRACE_FLAG_VALUE(TRACE_NONMOVING_GC, true),
    BUILTIN_COMMAND(
      BUILTIN_COMMAND_ID_STOP_NONMOVING_GC_TRACING,
      es_control_set_runtime_trace_flag,
      RUNTIME_TRACE_FLAG_VALUE(TRACE_NONMOVING_GC, false),
    BUILTIN_COMMAND(
      BUILTIN_COMMAND_ID_START_SPARK_SAMPLED_TRACING,
      es_control_set_runtime_trace_flag,
      RUNTIME_TRACE_FLAG_VALUE(TRACE_SPARK_SAMPLED, true),
    BUILTIN_COMMAND(
      BUILTIN_COMMAND_ID_STOP_SPARK_SAMPLED_TRACING,
      es_control_set_runtime_trace_flag,
      RUNTIME_TRACE_FLAG_VALUE(TRACE_SPARK_SAMPLED, false),
    BUILTIN_COMMAND(
      BUILTIN_COMMAND_ID_START_SPARK_FULL_TRACING,
      es_control_set_runtime_trace_flag,
      RUNTIME_TRACE_FLAG_VALUE(TRACE_SPARK_FULL, true),
    BUILTIN_COMMAND(
      BUILTIN_COMMAND_ID_STOP_SPARK_FULL_TRACING,
      es_control_set_runtime_trace_flag,
      RUNTIME_TRACE_FLAG_VALUE(TRACE_SPARK_FULL, false),
    BUILTIN_COMMAND(
      BUILTIN_COMMAND_ID_START_USER_TRACING,
      es_control_set_runtime_trace_flag,
      RUNTIME_TRACE_FLAG_VALUE(TRACE_USER, true),
    BUILTIN_COMMAND(
      BUILTIN_COMMAND_ID_STOP_USER_TRACING,
      es_control_set_runtime_trace_flag,
      RUNTIME_TRACE_FLAG_VALUE(TRACE_USER, false),
    BUILTIN_COMMAND(
      BUILTIN_COMMAND_ID_START_CAP_TRACING,
      es_control_set_runtime_trace_flag,
      RUNTIME_TRACE_FLAG_VALUE(TRACE_CAP, true),
    BUILTIN_COMMAND(
      BUILTIN_COMMAND_ID_STOP_CAP_TRACING,
      es_control_set_runtime_trace_flag,
      RUNTIME_TRACE_FLAG_VALUE(TRACE_CAP, false),
      NULL
    ))))))))))))))))))),
};
// clang-format on

/// @brief The mutex that guards the global namespace registry.
///
/// See `g_control_namespace_registry`.
static pthread_mutex_t g_control_namespace_registry_mutex =
    PTHREAD_MUTEX_INITIALIZER;

/******************************************************************************
 * Control Thread Variables and Entry-Point
 *****************************************************************************/

/// @brief The entry-point for the control thread.
static void *es_control_loop(void *arg);

/// @brief The control thread reads chunks of this size from the eventlog
/// socket.
#define CHUNK_SIZE 256

/// @brief The state that is shared with the control thread.
///
/// **NOTE**: Initialising with @c {0} should guarantee that all pointers are
/// NULL pointers.
static ControlState g_control_state = {0};

/// @brief A stable view the eventlog socket file descriptor.
///
/// See `g_control_state`.
static int g_client_fd = -1;

/******************************************************************************
 * Public API
 *
 * These functions run in user threads. They should return a status on error.
 ******************************************************************************/

/// @brief Control cleanup.
static void es_control_cleanup(void) {
  if (g_control_state.control_thread_ptr != NULL) {
    DEBUG_DEBUG("%s", "Cancelling control thread.");
    if (pthread_cancel(*g_control_state.control_thread_ptr) != 0) {
      DEBUG_ERRNO("pthread_cancel() failed for control thread");
    } else {
      if (pthread_join(*g_control_state.control_thread_ptr, NULL) != 0) {
        DEBUG_ERRNO("pthread_join() failed for control thread");
      }
    }
    free((void *)g_control_state.control_thread_ptr);
  }
}

/* HIDDEN - see documentation in control.h */
HIDDEN EventlogSocketStatus es_control_start(const ControlState control_state) {
  assert(control_state.control_thread_ptr != NULL);
  assert(control_state.client_fd_ptr != NULL);
  assert(control_state.mutex_ptr != NULL);
  assert(control_state.init_state_ptr != NULL);
  assert(control_state.new_connection_cond_ptr != NULL);
  assert(control_state.ghc_rts_ready_cond_ptr != NULL);
  DEBUG_DEBUG("%s", "Starting control thread.");
  memcpy(&g_control_state, &control_state, sizeof(ControlState));
  assert(g_control_state.control_thread_ptr != NULL);
  assert(g_control_state.client_fd_ptr != NULL);
  assert(g_control_state.mutex_ptr != NULL);
  assert(g_control_state.init_state_ptr != NULL);
  assert(g_control_state.new_connection_cond_ptr != NULL);
  assert(g_control_state.ghc_rts_ready_cond_ptr != NULL);
  RETURN_ON_ERROR(STATUS_FROM_PTHREAD(pthread_create(
      g_control_state.control_thread_ptr, NULL, es_control_loop, NULL)));
  RETURN_ON_ERROR(
      STATUS_FROM_PTHREAD(pthread_detach(*g_control_state.control_thread_ptr)));
  atexit(es_control_cleanup);
  // Return OK.
  return STATUS_FROM_CODE(EVENTLOG_SOCKET_OK);
}

/// @brief Check if the given entry in the namespace registry matches a given
/// name.
///
/// @pre namespace_entry != NULL
static bool es_control_namespace_store_match(
    const EventlogSocketControlNamespace *const namespace_entry,
    const size_t namespace_len, const char namespace[namespace_len]) {
  // if namespace_entry == NULL, then...
  if (namespace_entry == NULL) {
    // ...that's definitely not a match...
    return false;
  }
  // otherwise, compare the longest valid prefix of the namespaces...
  const size_t min_namespace_len =
      namespace_len < namespace_entry->namespace_len
          ? namespace_len
          : namespace_entry->namespace_len;
  const int namespace_cmp =
      strncmp(namespace, namespace_entry->namespace, min_namespace_len);
  return namespace_cmp == 0;
}

/* HIDDEN - see documentation in control.h */
HIDDEN const char *
es_control_strnamespace(EventlogSocketControlNamespace *namespace) {
  return namespace == NULL ? NULL : namespace->namespace;
}

/* HIDDEN - see documentation in control.h */
HIDDEN EventlogSocketStatus es_control_register_namespace(
    const uint8_t namespace_len, const char namespace[namespace_len],
    EventlogSocketControlNamespace **namespace_out) {

  // Check if namespace_out is NULL.
  if (namespace_out == NULL) {
    errno = EINVAL;
    return STATUS_FROM_ERRNO();
  }

  // Acquire the lock on g_control_namespace_registry.
  RETURN_ON_ERROR(STATUS_FROM_PTHREAD(
      pthread_mutex_lock(&g_control_namespace_registry_mutex)));

  // Initialise the namespace_entry pointer.
  EventlogSocketControlNamespace *namespace_entry =
      &g_control_namespace_registry;

  // Is the requested namespace already registered?
  do {
    // Is this the namespace you are trying to register?
    if (es_control_namespace_store_match(namespace_entry, namespace_len,
                                         namespace)) {
      // If so, return false.
      pthread_mutex_unlock(&g_control_namespace_registry_mutex);
      return STATUS_FROM_CODE(EVENTLOG_SOCKET_ERR_CTL_EXISTS);
    }
    // Is this the last namespace_entry?
    if (namespace_entry->next == NULL) {
      // If so, stop.
      break;
    }
    // Otherwise, continue with the next entry.
    namespace_entry = namespace_entry->next;

    // Ensure the loop has a cancellation point.
    pthread_testcancel();
  } while (true);

  // Register the requested namespace.
  assert(namespace_entry != NULL);
  assert(namespace_entry->next == NULL);
  char *const next_namespace = malloc(namespace_len + 1);
  if (next_namespace == NULL) {
    // If so, return false.
    pthread_mutex_unlock(&g_control_namespace_registry_mutex);
    return STATUS_FROM_ERRNO(); // `malloc` sets errno.
  }
  strncpy(next_namespace, namespace, namespace_len);
  next_namespace[namespace_len] = '\0';
  const EventlogSocketControlNamespace next = (EventlogSocketControlNamespace){
      .namespace_len = namespace_len,
      .namespace = next_namespace,
      .next = NULL,
  };
  namespace_entry->next = malloc(sizeof(EventlogSocketControlNamespace));
  if (namespace_entry->next == NULL) {
    // If so, return false.
    pthread_mutex_unlock(&g_control_namespace_registry_mutex);
    return STATUS_FROM_ERRNO(); // `malloc` sets errno.
  }
  memcpy(namespace_entry->next, &next, sizeof(EventlogSocketControlNamespace));
  DEBUG_DEBUG("Registered namespace %.*s", (int)namespace_len, namespace);

  // Release the lock on g_control_namespace_registry.
  RETURN_ON_ERROR(STATUS_FROM_PTHREAD(
      pthread_mutex_unlock(&g_control_namespace_registry_mutex)));

  // Write out the namespace entry.
  *namespace_out = namespace_entry->next;
  // Return OK.
  return STATUS_FROM_CODE(EVENTLOG_SOCKET_OK);
}

/* HIDDEN - see documentation in control.h */
HIDDEN EventlogSocketStatus
es_control_register_command(EventlogSocketControlNamespace *const namespace,
                            const EventlogSocketControlCommandId command_id,
                            EventlogSocketControlCommandHandler command_handler,
                            const void *command_data) {

  // Check if namespace is NULL.
  if (namespace == NULL) {
    errno = EINVAL;
    return STATUS_FROM_ERRNO();
  }

  // Check if namespace is the builtin namespace.
  if (namespace == &g_control_namespace_registry) {
    errno = EINVAL;
    return STATUS_FROM_ERRNO();
  }

  DEBUG_TRACE("Received request to register command 0x%02x in namespace %.*s",
              command_id, namespace->namespace_len, namespace->namespace);

  // Acquire the lock on g_control_namespace_registry.
  RETURN_ON_ERROR(STATUS_FROM_PTHREAD(
      pthread_mutex_lock(&g_control_namespace_registry_mutex)));

  // Create the data for the new command_entry.
  const EventlogSocketControlCommand next = (EventlogSocketControlCommand){
      .command_id = command_id,
      .command_handler = command_handler,
      .command_data = command_data,
      .next = NULL,
  };

  // Initialise the command_entry pointer.
  EventlogSocketControlCommand *command_entry;

  // If there are no commands in the command_store, then...
  if (namespace->command_registry == NULL) {
    // Allocate memory for the new command_entry.
    namespace->command_registry = malloc(sizeof(EventlogSocketControlCommand));
    if (namespace->command_registry == NULL) {
      // If so, return false.
      pthread_mutex_unlock(&g_control_namespace_registry_mutex);
      return STATUS_FROM_ERRNO(); // `malloc` sets errno.
    }
    command_entry = namespace->command_registry;
  }
  // Otherwise, traverse the command_store to the last position...
  else {
    do {
      // Initialise the command_entry with the head of the command_store.
      command_entry = namespace->command_registry;

      // Is the requested namespace already registered?
      if (command_entry->command_id == command_id) {
        DEBUG_ERROR("Command 0x%02x already registered for namespace %p.",
                    command_id, (void *)namespace);
        // If so, fail.
        pthread_mutex_unlock(&g_control_namespace_registry_mutex);
        return STATUS_FROM_CODE(EVENTLOG_SOCKET_ERR_CTL_EXISTS);
      }
    } while (command_entry->next != NULL);
    assert(command_entry != NULL);
    assert(command_entry->next == NULL);

    // Allocate memory for the new command_entry.
    command_entry->next = malloc(sizeof(EventlogSocketControlCommand));
    if (command_entry->next == NULL) {
      pthread_mutex_unlock(&g_control_namespace_registry_mutex);
      return STATUS_FROM_ERRNO(); // `malloc` sets errno.
    }
    command_entry = command_entry->next;
  }
  // Write the data for the new command_entry.
  DEBUG_TRACE("Registered command 0x%02x in namespace %.*s", command_id,
              namespace->namespace_len, namespace->namespace);
  memcpy(command_entry, &next, sizeof(EventlogSocketControlCommand));

  // Release the lock on g_control_namespace_registry.
  RETURN_ON_ERROR(STATUS_FROM_PTHREAD(
      pthread_mutex_unlock(&g_control_namespace_registry_mutex)));
  // Return OK.
  return STATUS_FROM_CODE(EVENTLOG_SOCKET_OK);
}

/******************************************************************************
 * Control Status
 ******************************************************************************/

/// @brief The current status of the control thread.
static EventlogSocketStatus g_status = STATUS_FROM_CODE(EVENTLOG_SOCKET_OK);

/// @brief The mutex that protects @c g_status.
static pthread_mutex_t g_status_mutex = PTHREAD_MUTEX_INITIALIZER;

/* HIDDEN - see documentation in control.h */
HIDDEN EventlogSocketStatus es_control_status(void) {
  pthread_mutex_lock(&g_status_mutex);
  const EventlogSocketStatus status = g_status;
  pthread_mutex_unlock(&g_status_mutex);
  return status;
}

/******************************************************************************
 * Control Thread
 *
 * These functions run in the control thread. Most of these function should
 * return a status on error, with the exception of `es_control_loop`.
 ******************************************************************************/

/// @brief Resolve a namespace by name.
///
/// @return Upon successful completion, the function returns a status with code
/// `EVENTLOG_SOCKET_OK`. If the namespace was found, then @p namespace_out
/// contains a stable pointer to the namespace that should not be freed. If the
/// namespace was not found and @p namespace_out contains @c NULL.
static EventlogSocketStatus es_control_namespace_store_resolve(
    const size_t namespace_len, const char namespace[namespace_len],
    EventlogSocketControlNamespace **namespace_out) {

  // Acquire a lock on g_control_namespace_registry.
  RETURN_ON_ERROR(STATUS_FROM_PTHREAD(
      pthread_mutex_lock(&g_control_namespace_registry_mutex)));

  // Initialise the namespace_entry pointer.
  EventlogSocketControlNamespace *namespace_entry =
      &g_control_namespace_registry;

  while (namespace_entry != NULL) {
    // Is this the namespace you are looking for?
    if (es_control_namespace_store_match(namespace_entry, namespace_len,
                                         namespace)) {

      // Release the lock on g_control_namespace_registry.
      RETURN_ON_ERROR(STATUS_FROM_PTHREAD(
          pthread_mutex_unlock(&g_control_namespace_registry_mutex)));

      // Write out the namespace.
      *namespace_out = namespace_entry;

      // Return OK.
      return STATUS_FROM_CODE(EVENTLOG_SOCKET_OK);
    }
    // Otherwise, continue with the next namespace_entry.
    namespace_entry = namespace_entry->next;
  }
  // Release the lock on g_control_namespace_registry.
  RETURN_ON_ERROR(STATUS_FROM_PTHREAD(
      pthread_mutex_unlock(&g_control_namespace_registry_mutex)));

  // Write out NULL.
  *namespace_out = NULL;

  // Return OK.
  return STATUS_FROM_CODE(EVENTLOG_SOCKET_OK);
}

/// @brief Call a command by namespace and ID.
static EventlogSocketStatus
es_control_command_handle(const EventlogSocketControlNamespace *const namespace,
                          const EventlogSocketControlCommandId command_id) {

  assert(namespace != NULL);

  DEBUG_TRACE("Handle command 0x%02x in namespace %.*s", command_id,
              namespace->namespace_len, namespace->namespace);

  // Acquire the lock on g_control_namespace_registry.
  RETURN_ON_ERROR(STATUS_FROM_PTHREAD(
      pthread_mutex_lock(&g_control_namespace_registry_mutex)));

  // Traverse the command_registry to find the command....
  {
    EventlogSocketControlCommand *command_entry = namespace->command_registry;
    while (command_entry != NULL) {
      // If this is the command we're looking for, then...
      if (command_entry->command_id == command_id) {
        // ...call the command handler...
        assert(command_entry->command_handler != NULL);
        command_entry->command_handler(namespace, command_id,
                                       command_entry->command_data);
        // ...release the lock on g_control_namespace_registry...
        RETURN_ON_ERROR(STATUS_FROM_PTHREAD(
            pthread_mutex_unlock(&g_control_namespace_registry_mutex)));
        // ...and return OK.
        return STATUS_FROM_CODE(EVENTLOG_SOCKET_OK);
      }
      // Otherwise, continue with the next command...
      command_entry = command_entry->next;
    }
  }
  // If the command was not found, then...
  // ...log an error...
  DEBUG_ERROR("Could not resolve command 0x%02x in namespace %.*s", command_id,
              namespace->namespace_len, namespace->namespace);
  // ...release the lock on g_control_namespace_registry...
  RETURN_ON_ERROR(STATUS_FROM_PTHREAD(
      pthread_mutex_unlock(&g_control_namespace_registry_mutex)));
  // ...and return OK.
  // NOTE: Other than debug logging, there is no distinction between "found" and
  // "not found", because the control thread must be robust against noise.
  return STATUS_FROM_CODE(EVENTLOG_SOCKET_OK);
}

/******************************************************************************
 * Command Parser
 ******************************************************************************/

/// @brief The tag for the command parser state.
///
/// See `ControlCommandParserState`.
typedef enum ControlCommandParserStateTag {
  /// The command parser is expecting some byte from the magic bytestring.
  ///
  /// See `g_control_magic`.
  CONTROL_COMMAND_PARSER_STATE_MAGIC,
  /// The command parser is expecting the protocol version.
  CONTROL_COMMAND_PARSER_STATE_PROTOCOL_VERSION,
  /// The command parser is expecting the namespace string length.
  CONTROL_COMMAND_PARSER_STATE_NAMESPACE_LEN,
  /// The command parser is expecting some byte from the namespace string.
  CONTROL_COMMAND_PARSER_STATE_NAMESPACE,
  /// The command parser is expecting the command ID.
  CONTROL_COMMAND_PARSER_STATE_COMMAND_ID,
} ControlCommandParserStateTag;

/// @brief Show a value of type `ControlCommandParserStateTag` as a
/// string.
static const char *
es_show_ControlCommandParserStateTag(ControlCommandParserStateTag tag) {
  switch (tag) {
  case CONTROL_COMMAND_PARSER_STATE_MAGIC:
    return "CONTROL_COMMAND_PARSER_STATE_MAGIC";
  case CONTROL_COMMAND_PARSER_STATE_PROTOCOL_VERSION:
    return "CONTROL_COMMAND_PARSER_STATE_PROTOCOL_VERSION";
  case CONTROL_COMMAND_PARSER_STATE_NAMESPACE_LEN:
    return "CONTROL_COMMAND_PARSER_STATE_NAMESPACE_LEN";
  case CONTROL_COMMAND_PARSER_STATE_NAMESPACE:
    return "CONTROL_COMMAND_PARSER_STATE_NAMESPACE";
  case CONTROL_COMMAND_PARSER_STATE_COMMAND_ID:
    return "CONTROL_COMMAND_PARSER_STATE_COMMAND_ID";
  }
}

/// @brief The command parser state.
typedef struct {
  /// The tag that determines which member of the union is set.
  ControlCommandParserStateTag tag;

  /// The untagged command parser state. The value of `tag` determines which
  /// member is set.
  ///
  /// - `CONTROL_COMMAND_PARSER_STATE_MAGIC`:
  ///   * Must set `header_pos`.
  ///
  /// - `CONTROL_COMMAND_PARSER_STATE_PROTOCOL_VERSION`:
  ///   * Must set *no member*.
  ///
  /// - `CONTROL_COMMAND_PARSER_STATE_NAMESPACE_LEN`:
  ///   * Must set *no member*.
  ///
  /// - `CONTROL_COMMAND_PARSER_STATE_NAMESPACE`:
  ///   * Must set `namespace_buffer_len`.
  ///   * Must set `namespace_buffer_pos`.
  ///   * Must set `namespace_buffer`.
  ///
  /// - `CONTROL_COMMAND_PARSER_STATE_COMMAND`:
  ///   * Must set `namespace`.
  ///
  union {
    /// The position of the next header byte.
    ///
    /// @invariant `header_pos < CONTROL_MAGIC_LEN`
    uint8_t header_pos;

    struct {
      /// The expected length of the namespace string.
      ///
      /// @invariant `namespace_buffer_len > 0`
      uint8_t namespace_buffer_len;
      /// The position of the next namespace string byte.
      ///
      /// @invariant `namespace_buffer_pos < namespace_buffer_len`
      uint8_t namespace_buffer_pos;
      /// The buffer for the namespace string.
      ///
      /// @invariant `sizeof(namespace_buffer) == namespace_buffer_len`
      char *namespace_buffer;
    };

    /// The resolved namespace for the command.
    const EventlogSocketControlNamespace *namespace;
  };
} ControlCommandParserState;

/// @brief The global command parser state.
static ControlCommandParserState g_control_command_parser_state = {
    .tag = CONTROL_COMMAND_PARSER_STATE_MAGIC,
    .header_pos = 0,
};

/// @brief Move the command parser to a new state.
///
/// This function frees any resources held by the current state, changes the
/// state tag, and initialises the new state. This function should not be used
/// for state changes *within* the same tag, as it will overwrite the previous
/// state.
///
/// If this function is used to *reset* the state, e.g., after a protocol error,
/// then a pointer to the current byte may be provided as the second argument.
/// This function will attempt to parse this byte as the first byte of the magic
/// bytestring.
///
/// If the target tag is  `CONTROL_COMMAND_PARSER_STATE_NAMESPACE` state, then a
/// pointer to the namespace length may be provided as the second argument. This
/// function will use this byte to initialise the new state.
static EventlogSocketStatus
es_control_command_parser_enter_state(const ControlCommandParserStateTag tag,
                                      const uint8_t *const data) {
  DEBUG_TRACE(
      "%s -> %s",
      es_show_ControlCommandParserStateTag(g_control_command_parser_state.tag),
      es_show_ControlCommandParserStateTag(tag));

  // this should only be called when restarting or moving to a different
  // state...
  assert(tag == CONTROL_COMMAND_PARSER_STATE_MAGIC ||
         g_control_command_parser_state.tag != tag);

  // if the parser is leaving CONTROL_COMMAND_PARSER_STATE_NAMESPACE, then...
  if (g_control_command_parser_state.tag ==
      CONTROL_COMMAND_PARSER_STATE_NAMESPACE) {
    // ...free the namespace buffer...
    free(g_control_command_parser_state.namespace_buffer);
  }

  // update the control state tag...
  g_control_command_parser_state.tag = tag;

  // initialise the control state appropriately...
  switch (tag) {
  case CONTROL_COMMAND_PARSER_STATE_MAGIC: {
    // if restarting, handle current_byte...
    if (tag == CONTROL_COMMAND_PARSER_STATE_MAGIC && data != NULL &&
        *data == g_control_magic[0]) {
      // ...start at the second header byte...
      g_control_command_parser_state.header_pos = 1;
    } else {
      // ...start at the first header byte...
      g_control_command_parser_state.header_pos = 0;
    }
    break;
  }
  case CONTROL_COMMAND_PARSER_STATE_PROTOCOL_VERSION: {
    assert(data == NULL);
    break;
  }
  case CONTROL_COMMAND_PARSER_STATE_NAMESPACE_LEN: {
    assert(data == NULL);
    break;
  }
  case CONTROL_COMMAND_PARSER_STATE_NAMESPACE: {
    assert(data != NULL);
    const size_t namespace_len = *data;
    g_control_command_parser_state.namespace_buffer_len = namespace_len;
    g_control_command_parser_state.namespace_buffer_pos = 0;
    // allocate space for the namespace, with one additional byte to ensure
    // that the string is always null-terminated in memory.
    g_control_command_parser_state.namespace_buffer = malloc(namespace_len + 1);
    if (g_control_command_parser_state.namespace_buffer == NULL) {
      RETURN_ON_ERROR(STATUS_FROM_ERRNO()); // `malloc` sets errno.
    }
    g_control_command_parser_state.namespace_buffer[namespace_len] = '\0';
    break;
  }
  case CONTROL_COMMAND_PARSER_STATE_COMMAND_ID: {
    assert(data == NULL);
    break;
  }
  }
  return STATUS_FROM_CODE(EVENTLOG_SOCKET_OK);
}

/// @brief Parse a chunk.
///
/// This is the incremental command parser. It parses a chunk of bytes and
/// updates the command parser state.
static EventlogSocketStatus
es_control_command_parser_handle_chunk(const size_t chunk_size,
                                       const uint8_t chunk[chunk_size]) {
  DEBUG_DEBUG("Received chunk of size %zd.", chunk_size);
  // iterate over the bytes in the chunk...
  for (size_t chunk_index = 0; chunk_index < chunk_size; ++chunk_index) {
    // get the next byte from the chunk...
    const uint8_t current_byte = chunk[chunk_index];
    switch (g_control_command_parser_state.tag) {
    // the parser is currently reading the header...
    case CONTROL_COMMAND_PARSER_STATE_MAGIC: {
      // invariant: header_pos should be a valid index into control_magic
      assert(0 <= g_control_command_parser_state.header_pos);
      assert(g_control_command_parser_state.header_pos < CONTROL_MAGIC_LEN);
      const uint8_t expected_byte =
          g_control_magic[g_control_command_parser_state.header_pos];
      // if the next byte is the expected byte...
      if (current_byte == expected_byte) {
        DEBUG_DEBUG("Matched control_magic byte %d",
                    g_control_command_parser_state.header_pos);
        // ...move on the the next state...
        ++g_control_command_parser_state.header_pos;
        // if header_pos moves out of control_magic...
        if (g_control_command_parser_state.header_pos >= CONTROL_MAGIC_LEN) {
          // ...continue reading the namespace length...
          RETURN_ON_ERROR(es_control_command_parser_enter_state(
              CONTROL_COMMAND_PARSER_STATE_PROTOCOL_VERSION, NULL));
        }
        // ...continue processing with the _next_ byte...
        continue;
      }
      // if the next byte is not the expected byte...
      else {
        // ...there has been a protocol error...
        // ...restart with the _current_ byte...
        RETURN_ON_ERROR(es_control_command_parser_enter_state(
            CONTROL_COMMAND_PARSER_STATE_MAGIC, &current_byte));
        // ...continue processing with the _next_ byte...
        continue;
      }
    }
    // the parser is currently reading the protocol version...
    case CONTROL_COMMAND_PARSER_STATE_PROTOCOL_VERSION: {
      DEBUG_DEBUG("Matched protocol version byte %d", current_byte);
      // if the message version matches the protocol version...
      if (current_byte == EVENTLOG_SOCKET_CONTROL_PROTOCOL_VERSION) {
        // ...then we should be able to parse the message...
        // ...continue processing with the _next_ byte...
        RETURN_ON_ERROR(es_control_command_parser_enter_state(
            CONTROL_COMMAND_PARSER_STATE_NAMESPACE_LEN, NULL));
        continue;
      } else {
        // ...otherwise, let's not try and parse this message...
        // ...restart with the _current_ byte...
        RETURN_ON_ERROR(es_control_command_parser_enter_state(
            CONTROL_COMMAND_PARSER_STATE_MAGIC, &current_byte));
      }
    }
    // the parser is currently reading the namespace length...
    case CONTROL_COMMAND_PARSER_STATE_NAMESPACE_LEN: {
      // if current_byte == 0, then...
      if (current_byte == 0) {
        // ...there has been a protocol error...
        // todo: enforce this in the register function
        // todo: write an error to the eventlog
        DEBUG_ERROR("%s", "Received namespace length 0");
        // ...restart with the _current_ byte...
        RETURN_ON_ERROR(es_control_command_parser_enter_state(
            CONTROL_COMMAND_PARSER_STATE_MAGIC, &current_byte));
        continue;
      } else {
        DEBUG_DEBUG("Matched namespace_len byte %d", current_byte);
        // otherwise, accept the namespace length and move to the next state...
        RETURN_ON_ERROR(es_control_command_parser_enter_state(
            CONTROL_COMMAND_PARSER_STATE_NAMESPACE, &current_byte));
        continue;
      }
    }
    case CONTROL_COMMAND_PARSER_STATE_NAMESPACE: {
      // calculate the number of bytes still required for the namespace.
      // note: subtraction is safe due to the invariant on namespace_buffer_pos.
      assert(g_control_command_parser_state.namespace_buffer_len > 0);
      assert(g_control_command_parser_state.namespace_buffer_pos <
             g_control_command_parser_state.namespace_buffer_len);
      const uint8_t required_bytes_for_namespace =
          g_control_command_parser_state.namespace_buffer_len -
          g_control_command_parser_state.namespace_buffer_pos;
      assert(required_bytes_for_namespace > 0);

      // calculate the number of bytes still available in the chunk.
      // note: subtraction is safe due to the loop invariant.
      assert(chunk_index < chunk_size);
      const uint8_t remaining_bytes_in_chunk = chunk_size - chunk_index;
      assert(remaining_bytes_in_chunk > 0);

      // calculate the number of bytes that are available to be copied to the
      // namespace buffer.
      const uint8_t available_bytes =
          remaining_bytes_in_chunk < required_bytes_for_namespace
              ? remaining_bytes_in_chunk
              : required_bytes_for_namespace;
      assert(available_bytes > 0);

      // copy all available bytes to the namespace buffer.
      void *cpy_dest = g_control_command_parser_state.namespace_buffer +
                       g_control_command_parser_state.namespace_buffer_pos;
      const void *cpy_src = chunk + chunk_index;
      memcpy(cpy_dest, cpy_src, available_bytes);
      // move namespace_buffer_pos.
      g_control_command_parser_state.namespace_buffer_pos += available_bytes;

      // if the namespace is incomplete, then...
      if (g_control_command_parser_state.namespace_buffer_pos <
          g_control_command_parser_state.namespace_buffer_len) {
        // move chunk_index by the number of copied bytes less one,
        // because the chunk_index will be updated when we reenter the for loop.
        // note: the subtraction is safe because available_bytes > 0
        chunk_index += available_bytes - 1;
        // ...continue processing with the _next_ byte...
        continue;
      }

      // otherwise, the namespace is complete...
      // note: this relies on the fact that the string is null-terminated!
      DEBUG_DEBUG("Matched namespace %.*s",
                  g_control_command_parser_state.namespace_buffer_len,
                  g_control_command_parser_state.namespace_buffer);

      // ...try to resolve the namespace...
      EventlogSocketControlNamespace *namespace = NULL;
      RETURN_ON_ERROR(es_control_namespace_store_resolve(
          g_control_command_parser_state.namespace_buffer_len,
          g_control_command_parser_state.namespace_buffer, &namespace));
      // if the namespace was successfully resolved, then...
      if (namespace != NULL) {
        DEBUG_DEBUG("Resolved namespace %.*s",
                    g_control_command_parser_state.namespace_buffer_len,
                    g_control_command_parser_state.namespace_buffer);
        // move chunk_index by the number of copied bytes less one,
        // because the chunk_index will be updated when we reenter the for loop.
        // note: the subtraction is safe because available_bytes > 0
        chunk_index += available_bytes - 1;
        // ...move to the next state...
        RETURN_ON_ERROR(es_control_command_parser_enter_state(
            CONTROL_COMMAND_PARSER_STATE_COMMAND_ID, NULL));
        g_control_command_parser_state.namespace = namespace;
        // ...continue processing with the _next_ byte...
        continue;
      }

      // otherwise, the namespace was not successfully resolved...
      else {
        // ...there has been a protocol error...
        // note: If the socket is noisy and happens to produce the sequence
        //       of control_magic bytes, the subsequent byte is interpreted
        //       as namespace_len and the parser unconditionally consumes the
        //       next namespace_len bytes. It then fails _at this point_, when
        //       it fails to resolve the namespace.
        //
        //       If we continue from the current byte onwards, that means that
        //       we skip namespace_len bytes, which may have a valid command.
        //
        //       However, I don't think it's unreasonable to assume that the
        //       common case is a message with an unregistered namespace.
        //       In this case, it'd be reasonable to continue from the current
        //       byte onwards, or – ideally – skip the command_id byte and
        //       continue from _there on_.
        //
        //       In order to distinguish between noise and an unregistered
        //       namespace, it may help to require that the namespace bytes
        //       are separated from the command ID with a null byte.
        //       While noise _could_ produce that pattern, it's vastly less
        //       likely that random noise or messages from another protocol
        //       would produce:
        //
        //         <control_magic bytes>
        //           + <namespace_len byte>
        //           + <namespace_len number of bytes>
        //           + '\0'
        //
        // todo: write an error to the eventlog
        DEBUG_ERROR("Unknown namespace %.*s",
                    g_control_command_parser_state.namespace_buffer_len,
                    g_control_command_parser_state.namespace_buffer);
        // ...restart with the _current_ byte...
        RETURN_ON_ERROR(es_control_command_parser_enter_state(
            CONTROL_COMMAND_PARSER_STATE_MAGIC, &current_byte));
        // ...continue processing with the _next_ byte...
        continue;
      }
    }
    case CONTROL_COMMAND_PARSER_STATE_COMMAND_ID: {
      DEBUG_DEBUG("Matched command_id byte 0x%02x", current_byte);
      // Handle the command.
      RETURN_ON_ERROR(es_control_command_handle(
          g_control_command_parser_state.namespace, current_byte));
      // ...restart _without_ the current byte...
      RETURN_ON_ERROR(es_control_command_parser_enter_state(
          CONTROL_COMMAND_PARSER_STATE_MAGIC, NULL));
      // ...continue processing with the _next_ byte...
      continue;
    }
    }
  }
  return STATUS_FROM_CODE(EVENTLOG_SOCKET_OK);
}

/******************************************************************************
 * Control Thread
 ******************************************************************************/

/// Reset the control thread state when the connection changes.
///
/// @param new_control_fd The new eventlog socket file descriptor. May be `-1`.
static EventlogSocketStatus es_control_fd_reset_to(const int new_control_fd) {
  DEBUG_DEBUG("%s", "Resetting control server state.");
  // Reset eventlog socket file descriptor.
  g_client_fd = new_control_fd;
  // Reset parser state.
  RETURN_ON_ERROR(es_control_command_parser_enter_state(
      CONTROL_COMMAND_PARSER_STATE_MAGIC, NULL));
  // Return OK.
  return STATUS_FROM_CODE(EVENTLOG_SOCKET_OK);
}

/// @brief Wait for the signal that the GHC RTS is ready.
static EventlogSocketStatus es_control_wait_ghc_rts_ready(void) {
  DEBUG_DEBUG("%s", "Waiting for signal that GHC RTS is ready.");
  assert(g_control_state.init_state_ptr != NULL);
  assert(g_control_state.mutex_ptr != NULL);
  assert(g_control_state.ghc_rts_ready_cond_ptr != NULL);
  RETURN_ON_ERROR(
      STATUS_FROM_PTHREAD(pthread_mutex_lock(g_control_state.mutex_ptr)));
  while (true) {
    // Check whether or not the GHC RTS is ready.
    const bool ghc_rts_ready =
        (*g_control_state.init_state_ptr) & EVENTLOG_SOCKET_SIG_RTS_READY;
    if (ghc_rts_ready) {
      // If the GHC RTS is ready, break from the loop.
      break;
    } else {
      // If the GHC RTS is NOT ready, wait on the relevant condition and
      // re-enter the loop.
      //
      // NOTE: This call acts as the cancellation point for this infinite loop.
      RETURN_ON_ERROR_CLEANUP(STATUS_FROM_PTHREAD(pthread_cond_wait(
                                  g_control_state.ghc_rts_ready_cond_ptr,
                                  g_control_state.mutex_ptr)),
                              pthread_mutex_unlock(g_control_state.mutex_ptr));
    }
  }
  RETURN_ON_ERROR(
      STATUS_FROM_PTHREAD(pthread_mutex_unlock(g_control_state.mutex_ptr)));
  // Return OK.
  return STATUS_FROM_CODE(EVENTLOG_SOCKET_OK);
}

/// @brief Wait for a new connection.
///
/// @pre The caller must have a lock on `g_control_fd_mutex_ptr`.
/// @post The caller will have a lock on `g_control_fd_mutex_ptr`.
static EventlogSocketStatus es_control_wait_for_connection(void) {
  assert(g_control_state.mutex_ptr != NULL);
  assert(g_control_state.new_connection_cond_ptr != NULL);
  DEBUG_DEBUG("%s", "Waiting to be notified of new connection.");
  RETURN_ON_ERROR(STATUS_FROM_PTHREAD(pthread_cond_wait(
      g_control_state.new_connection_cond_ptr, g_control_state.mutex_ptr)));
  return STATUS_FROM_CODE(EVENTLOG_SOCKET_OK);
}

/// @brief The memory for the current chunk.
static uint8_t chunk[CHUNK_SIZE] = {0};

/// @brief The entry-point for the control thread.
static void *es_control_loop(void *arg) {
  (void)arg;

  assert(g_control_state.client_fd_ptr != NULL);
  assert(g_control_state.mutex_ptr != NULL);
  assert(g_control_state.new_connection_cond_ptr != NULL);

  // Wait for the GHC RTS to become ready.
  EXIT_ON_ERROR(es_control_wait_ghc_rts_ready());

  /* BEGIN: The main control loop. */
  while (true) {
    DEBUG_TRACE("%s", "Starting new control iteration.");

    // Ensure the loop has a cancellation point.
    pthread_testcancel();

    /* BEGIN: Wake up. */
    // At the start of each control iteration, we update the eventlog socket
    // file descriptor.

    // Acquire the lock on the connection file description.
    EXIT_ON_ERROR(
        STATUS_FROM_PTHREAD(pthread_mutex_lock(g_control_state.mutex_ptr)));

    // Read current connection file description.
    const int new_control_fd = *g_control_state.client_fd_ptr;
    if (g_client_fd != new_control_fd) {
      DEBUG_TRACE("Old connection fd: %d", g_client_fd);
      DEBUG_TRACE("New connection fd: %d", new_control_fd);
    }

    // If there WAS NO connection and there IS NO connection, then...
    if (g_client_fd == -1 && new_control_fd == -1) {
      DEBUG_TRACE("%s", "There WAS NO connection and there IS NO connection.");
      // ...wait to be notified of a new connection...
      EXIT_ON_ERROR_CLEANUP(es_control_wait_for_connection(),
                            pthread_mutex_unlock(g_control_state.mutex_ptr));
      // ...release the lock...
      EXIT_ON_ERROR(
          STATUS_FROM_PTHREAD(pthread_mutex_unlock(g_control_state.mutex_ptr)));
      // ...and re-enter the loop.
      continue;
    }

    // If there WAS NO connection but there IS A connection, then...
    else if (g_client_fd == -1 && new_control_fd != -1) {
      DEBUG_TRACE("%s", "There WAS NO connection but there IS A connection.");
      // ...DON'T wait to be notified of a new connection...
      // ...we may we have already missed the signal...
      // ...reset the control server state...
      EXIT_ON_ERROR_CLEANUP(es_control_fd_reset_to(new_control_fd),
                            pthread_mutex_unlock(g_control_state.mutex_ptr));
      // ...continue to try to handle a command.
    }

    // If there WAS A connection but there IS NO connection, then...
    else if (g_client_fd != -1 && new_control_fd == -1) {
      DEBUG_TRACE("%s", "There WAS A connection but there IS NO connection.");
      // ...reset the control server state...
      EXIT_ON_ERROR_CLEANUP(es_control_fd_reset_to(new_control_fd),
                            pthread_mutex_unlock(g_control_state.mutex_ptr));
      // ...wait to be notified of a new connection...
      EXIT_ON_ERROR_CLEANUP(es_control_wait_for_connection(),
                            pthread_mutex_unlock(g_control_state.mutex_ptr));
      // ...release the lock...
      EXIT_ON_ERROR(
          STATUS_FROM_PTHREAD(pthread_mutex_unlock(g_control_state.mutex_ptr)));
      // ...and re-enter the loop.
      continue;
    }

    // If there WAS A connection and there IS A connection, then...
    else if (g_client_fd != -1 && new_control_fd != -1) {
      // If it is A DIFFERENT connection, then...
      if (g_client_fd != new_control_fd) {
        DEBUG_TRACE(
            "%s",
            "There WAS A connection and there IS A DIFFERENT connection.");
        // ...DON'T wait to be notified of a new connection...
        // ...we may we have already missed the signal...
        // ...reset the control server state...
        EXIT_ON_ERROR_CLEANUP(es_control_fd_reset_to(new_control_fd),
                              pthread_mutex_unlock(g_control_state.mutex_ptr));
        // ...continue to try to handle a command.
      }
      // If it is THE SAME connection, then...
      else {
        // ...continue to try to handle a command.
        DEBUG_TRACE("%s",
                    "There WAS A connection and there IS THE SAME connection.");
        // ...continue to try to handle a command.
      }
    }

    // These conditions should be covering, so throw an error otherwise.
    else {
      assert(false);
    }

    // Release the lock on the connection file description.
    EXIT_ON_ERROR(
        STATUS_FROM_PTHREAD(pthread_mutex_unlock(g_control_state.mutex_ptr)));

    // Check that g_control_fd is up-to-date:
    assert(g_client_fd == new_control_fd);
    /* END: Wake up. */

    /* BEGIN: Wait for input. */
    // The eventlog socket is marked as non-blocking. If we try to receive
    // data, the `recv` function returns immediately. This works, but causes
    // us to go through the control loop *very* quickly. Instead, we wait for
    // input data.

    // note: POLLHUP and POLLRDHUP are output only and are ignored input.
    struct pollfd pfds[1] = {{
        .fd = g_client_fd,
        .events = POLLIN,
        .revents = 0,
    }};
    const int ready_or_error = poll(pfds, 1, POLL_LISTEN_TIMEOUT);
    // if ready_or_error is -1, an error occurred...
    if (ready_or_error == -1) {
      // poll may fail with EFAULT, EINTR, EINVAL, EINVAL, or ENOMEM,
      // none of which are recoverable...
      EXIT_ON_ERROR(STATUS_FROM_ERRNO());
    }
    // if ready_or_error is 0, the call to poll timed out...
    else if (ready_or_error == 0) {
      DEBUG_TRACE("%s", "poll() timed out");
      continue;
    }
    // otherwise ready_or_error is 1, and the file descriptor is
    // ready_or_error...
    else {
      assert(ready_or_error == 1); // poll invariant: ready_or_error <= |pfds|
      const int revents = pfds[0].revents;
      // if either of the POLLERR, POLLHUP, or POLLNVAL bits are set,
      // the file descriptor is closed...
      // note: in the case of POLLHUP there may still be buffered input,
      //       so this condition should be checked _after_ POLLIN.
      if ((revents & POLLNVAL) || (revents & POLLHUP) || (revents & POLLERR)) {
        // todo: wait for a new connection...
        DEBUG_TRACE("Connection on fd %d closed.", g_client_fd);
        continue;
      }
      // otherwise, the POLLIN bit should be set...
      assert(revents & POLLIN);
      // ...so the file descriptor is ready with input...
      // ...continue with the main loop...
    }
    /* END: Wait for input. */

    /* BEGIN: Handle up to one chunk of input. */
    // Once we know that there is some input, we read and handle one chunk.

    // read a chunk:
    const ssize_t chunk_size_or_error = recv(g_client_fd, chunk, CHUNK_SIZE, 0);
    // if num_bytes_or_error == -1, an error occurred...
    if (chunk_size_or_error == -1) {
      // if errno is EGAIN or EWOULDBLOCK, recv timed out...
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        DEBUG_TRACE("%s", "recv() timed out or was interrupted.");
        // note: the socket should have SO_RCVTIMEO set.
        continue;
      }
      // if errno is ECONNREFUSED, ENOTCONN, or ENOTSOCK, it may recover wit a
      // new connection...
      else if (errno == ECONNREFUSED || errno == ENOTCONN ||
               errno == ENOTSOCK) {
        DEBUG_ERRNO("recv() failed");
        continue;
      }
      // otherwise, the error is non-recoverable...
      else if (errno == EINTR) {
        EXIT_ON_ERROR(STATUS_FROM_ERRNO());
      }
    }
    // if num_bytes_or_error == 0, the connection was closed...
    else if (chunk_size_or_error == 0) {
      // todo: wait for a new connection...
      DEBUG_TRACE("Connection closed on fd: %d", g_client_fd);
      continue;
    }
    // otherwise, handle the received chunk...
    else {
      DEBUG_TRACE("Received %zd bytes.", chunk_size_or_error);
      assert(chunk_size_or_error > 0);
      EXIT_ON_ERROR(
          es_control_command_parser_handle_chunk(chunk_size_or_error, chunk));
    }
    /* END: Handle up to one chunk of input. */
  }
  /* END: The main control loop. */
  return NULL;
}
