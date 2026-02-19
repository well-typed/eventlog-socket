/// @file      control.c
/// @brief     The control thread.
/// @details   This module defines the control thread, which listens on the
///            eventlog socket and handles incoming control command requests.
///            Control commands can be registered using the public C API.
///            See `eventlog_socket_control_register_namespace`
///            and `eventlog_socket_control_register_command`.
/// @author    Wen Kokke
/// @author    Matthew Pickering
/// @version   0.1.1.0
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
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <Rts.h>
#include <rts/prof/Heap.h>

#include "./control.h"
#include "./debug.h"
#include "./error.h"
#include "./poll.h"
#include "eventlog_socket.h"

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

/// @brief The ID of the builtin `StartHeapProfiling` command.
#define BUILTIN_COMMAND_ID_START_HEAP_PROFILING 0

/// @brief The ID of the builtin `StopHeapProfiling` command.
#define BUILTIN_COMMAND_ID_STOP_HEAP_PROFILING 1

/// @brief The ID of the builtin `RequestHeapCensus` command.
#define BUILTIN_COMMAND_ID_REQUEST_HEAP_CENSUS 2

/******************************************************************************
 * Handlers for Builtin Commands
 ******************************************************************************/

/// @brief Handler for "StartHeapProfiling" command (eventlog-socket::0).
static void control_start_heap_profiling(
    const EventlogSocketControlNamespace *const namespace,
    const EventlogSocketControlCommandId command_id, const void *user_data) {
  (void)namespace;
  (void)command_id;
  (void)user_data;
  startHeapProfTimer();
  DEBUG_DEBUG("%s", "Started heap profiling.");
}

/// @brief Handler for "StopHeapProfiling" command (eventlog-socket::1).
static void control_stop_heap_profiling(
    const EventlogSocketControlNamespace *const namespace,
    const EventlogSocketControlCommandId command_id, const void *user_data) {
  (void)namespace;
  (void)command_id;
  (void)user_data;
  stopHeapProfTimer();
  DEBUG_DEBUG("%s", "Stopped heap profiling.");
}

/// @brief Handler for "RequestHeapCensus" command (eventlog-socket::2).
static void control_request_heap_census(
    const EventlogSocketControlNamespace *const namespace,
    const EventlogSocketControlCommandId command_id, const void *user_data) {
  (void)namespace;
  (void)command_id;
  (void)user_data;
  requestHeapCensus();
  DEBUG_DEBUG("%s", "Requested heap census.");
}

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

/// @brief The global namespace registry.
///
/// See `eventlog_socket_control_namespace`.
///
/// This is initialised with the builtin namespace and the builtin commands.
EventlogSocketControlNamespace g_control_namespace_registry = {
    .namespace = BUILTIN_NAMESPACE,
    .namespace_len = strlen(BUILTIN_NAMESPACE),
    .next = NULL,
    .command_registry =
        &(EventlogSocketControlCommand){
            .command_id = BUILTIN_COMMAND_ID_START_HEAP_PROFILING,
            .command_handler = control_start_heap_profiling,
            .command_data = NULL,
            .next =
                &(EventlogSocketControlCommand){
                    .command_id = BUILTIN_COMMAND_ID_STOP_HEAP_PROFILING,
                    .command_handler = control_stop_heap_profiling,
                    .command_data = NULL,
                    .next =
                        &(EventlogSocketControlCommand){
                            .command_id =
                                BUILTIN_COMMAND_ID_REQUEST_HEAP_CENSUS,
                            .command_handler = control_request_heap_census,
                            .command_data = NULL,
                            .next = NULL,
                        },
                },
        },
};

/// @brief The mutex that guards the global namespace registry.
///
/// See `g_control_namespace_registry`.
pthread_mutex_t g_control_namespace_registry_mutex = PTHREAD_MUTEX_INITIALIZER;

/// @brief Check if the given entry in the namespace registry matches a given
/// name.
///
/// @pre namespace_entry != NULL
bool control_namespace_store_match(
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

/// @brief Resolve a namespace by name.
///
/// @return If the namespace is found, this function returns a stable pointer to
/// it. Otherwise, it returns NULL. The returned pointer should not be freed.
const EventlogSocketControlNamespace *
control_namespace_store_resolve(const size_t namespace_len,
                                const char namespace[namespace_len]) {

  // Acquire a lock on g_control_namespace_store.
  pthread_mutex_lock(&g_control_namespace_registry_mutex);

  // Initialise the namespace_entry pointer.
  EventlogSocketControlNamespace *namespace_entry =
      &g_control_namespace_registry;

  while (namespace_entry != NULL) {
    // Is this the namespace you are looking for?
    if (control_namespace_store_match(namespace_entry, namespace_len,
                                      namespace)) {
      // Release the lock on g_control_namespace_store.
      pthread_mutex_unlock(&g_control_namespace_registry_mutex);
      return namespace_entry;
    }
    // Otherwise, continue with the next namespace_entry.
    namespace_entry = namespace_entry->next;
  }
  // Release the lock on g_control_namespace_store.
  pthread_mutex_unlock(&g_control_namespace_registry_mutex);
  return false;
}

/* PUBLIC - see documentation in eventlog_socket.h */
EventlogSocketControlNamespace *eventlog_socket_control_register_namespace(
    const uint8_t namespace_len, const char namespace[namespace_len]) {

  // Acquire the lock on g_control_namespace_store.
  pthread_mutex_lock(&g_control_namespace_registry_mutex);

  // Initialise the namespace_entry pointer.
  EventlogSocketControlNamespace *namespace_entry =
      &g_control_namespace_registry;

  // Is the requested namespace already registered?
  do {
    // Is this the namespace you are trying to register?
    if (control_namespace_store_match(namespace_entry, namespace_len,
                                      namespace)) {
      // If so, return false.
      pthread_mutex_unlock(&g_control_namespace_registry_mutex);
      return NULL;
    }
    // Is this the last namespace_entry?
    if (namespace_entry->next == NULL) {
      // If so, stop.
      break;
    }
    // Otherwise, continue with the next entry.
    namespace_entry = namespace_entry->next;
  } while (true);

  // Register the requested namespace.
  assert(namespace_entry != NULL);
  assert(namespace_entry->next == NULL);
  char *const next_namespace = malloc(namespace_len + 1);
  strncpy(next_namespace, namespace, namespace_len);
  next_namespace[namespace_len] = '\0';
  const EventlogSocketControlNamespace next = (EventlogSocketControlNamespace){
      .namespace_len = namespace_len,
      .namespace = next_namespace,
      .next = NULL,
  };
  namespace_entry->next = malloc(sizeof(EventlogSocketControlNamespace));
  memcpy(namespace_entry->next, &next, sizeof(EventlogSocketControlNamespace));
  DEBUG_DEBUG("Registered namespace %.*s", (int)namespace_len, namespace);

  // Release the lock on g_control_namespace_store.
  pthread_mutex_unlock(&g_control_namespace_registry_mutex);

  // Return the namespace entry.
  return namespace_entry->next;
}

/// @brief Resolve a command by namespace and ID.
///
/// @return If the command is found, this function returns a stable pointer to
/// it. Otherwise, it returns NULL. The returned pointer should not be freed.
const EventlogSocketControlCommand *control_command_store_resolve(
    const EventlogSocketControlNamespace *const namespace,
    const EventlogSocketControlCommandId command_id) {

  // Acquire the lock on g_control_namespace_store.
  pthread_mutex_lock(&g_control_namespace_registry_mutex);

  // Traverse the command_store to find the command....
  EventlogSocketControlCommand *command = namespace->command_registry;

  while (command != NULL) {
    // If this is the command we're looking for, then...
    if (command->command_id == command_id) {
      // ...release the lock on g_control_namespace_store...
      pthread_mutex_unlock(&g_control_namespace_registry_mutex);
      // ...and return the command.
      return command;
    }
    // Otherwise, continue with the next command...
    command = command->next;
  }
  // If the command was not found...
  // ...release the lock on g_control_namespace_store...
  pthread_mutex_unlock(&g_control_namespace_registry_mutex);
  // ...and return NULL.
  return NULL;
}

/* PUBLIC - see documentation in eventlog_socket.h */
EventlogSocketStatus eventlog_socket_control_register_command(
    EventlogSocketControlNamespace *const namespace,
    const EventlogSocketControlCommandId command_id,
    EventlogSocketControlCommandHandler command_handler,
    const void *command_data) {

  DEBUG_TRACE("Received request to register command 0x%02x in namespace %.*s",
              command_id, namespace->namespace_len, namespace->namespace);

  // Acquire the lock on g_control_namespace_store.
  {
    const int success_or_errno =
        pthread_mutex_lock(&g_control_namespace_registry_mutex);
    if (success_or_errno != 0) {
      return STATUS_FROM_PTHREAD_ERROR(success_or_errno);
    }
  }

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
    command_entry = namespace->command_registry;
    if (command_entry == NULL) {
      return STATUS_FROM_ERRNO();
    }
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
        return STATUS_FROM_CODE(EVENTLOG_SOCKET_ERROR_CMD_EXISTS);
      }
    } while (command_entry->next != NULL);
    assert(command_entry != NULL);
    assert(command_entry->next == NULL);

    // Allocate memory for the new command_entry.
    command_entry->next = malloc(sizeof(EventlogSocketControlCommand));
    command_entry = command_entry->next;
    if (command_entry == NULL) {
      return STATUS_FROM_ERRNO();
    }
  }
  // Write the data for the new command_entry.
  DEBUG_TRACE("Registered command 0x%02x in namespace %.*s", command_id,
              namespace->namespace_len, namespace->namespace);
  memcpy(command_entry, &next, sizeof(EventlogSocketControlCommand));

  // Release the lock on g_control_namespace_store.
  {
    const int success_or_errno =
        pthread_mutex_unlock(&g_control_namespace_registry_mutex);
    if (success_or_errno != 0) {
      return STATUS_FROM_PTHREAD_ERROR(success_or_errno);
    }
  }
  return STATUS_FROM_CODE(EVENTLOG_SOCKET_OK);
}

/// @brief Call a command by namespace and ID.
static bool
control_command_handle(const EventlogSocketControlNamespace *const namespace,
                       const EventlogSocketControlCommandId command_id) {
  DEBUG_TRACE("Handle command 0x%02x in namespace %.*s", command_id,
              namespace->namespace_len, namespace->namespace);

  // Resolve the command.
  const EventlogSocketControlCommand *command =
      control_command_store_resolve(namespace, command_id);

  // If the command was not found, then...
  if (command == NULL) {
    // ...return false.
    DEBUG_ERROR("Could not resolve command 0x%02x in namespace %.*s",
                command_id, namespace->namespace_len, namespace->namespace);
    return false;
  }
  // Otherwise...
  else {
    // ...call the command handler...
    assert(command->command_handler != NULL);
    command->command_handler(namespace, command_id, command->command_data);
    // ...and return true.
    return true;
  }
}

/******************************************************************************
 * Waiting for the GHC RTS
 ******************************************************************************/

/// @brief A global variable that tracks whether the GHC RTS is ready.
static volatile bool g_ghc_rts_ready = false;

/// @brief The condition on which to wait for the signal that the GHC RTS is
/// ready.
static pthread_cond_t g_ghc_rts_ready_cond = PTHREAD_COND_INITIALIZER;

/// @brief The mutex that corresponds to `g_ghc_rts_ready_cond`.
static pthread_mutex_t g_ghc_rts_ready_mutex = PTHREAD_MUTEX_INITIALIZER;

/// @brief Wait for the signal that the GHC RTS is ready.
static void control_wait_ghc_rts_ready(void) {
  DEBUG_DEBUG("%s", "Waiting for signal that GHC RTS is ready.");
  pthread_mutex_lock(&g_ghc_rts_ready_mutex);
  while (!g_ghc_rts_ready) {
    pthread_cond_wait(&g_ghc_rts_ready_cond, &g_ghc_rts_ready_mutex);
  }
  pthread_mutex_unlock(&g_ghc_rts_ready_mutex);
}

/* PUBLIC - see documentation in eventlog_socket.h */
EventlogSocketStatus eventlog_socket_signal_ghc_rts_ready(void) {
  DEBUG_DEBUG("%s", "Sending signal that GHC RTS is ready.");
  {
    const int success_or_errno = pthread_mutex_lock(&g_ghc_rts_ready_mutex);
    if (success_or_errno != 0) {
      return STATUS_FROM_PTHREAD_ERROR(success_or_errno);
    }
  }
  if (!g_ghc_rts_ready) {
    g_ghc_rts_ready = true;
    const int success_or_errno = pthread_cond_broadcast(&g_ghc_rts_ready_cond);
    if (success_or_errno != 0) {
      return STATUS_FROM_PTHREAD_ERROR(success_or_errno);
    }
  }
  {
    const int success_or_errno = pthread_mutex_unlock(&g_ghc_rts_ready_mutex);
    if (success_or_errno != 0) {
      return STATUS_FROM_PTHREAD_ERROR(success_or_errno);
    }
  }
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
const char *ControlCommandParserStateag_show(ControlCommandParserStateTag tag) {
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
ControlCommandParserState g_control_command_parser_state = {
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
static void
control_command_parser_enter_state(const ControlCommandParserStateTag tag,
                                   const uint8_t *const data) {
  DEBUG_TRACE(
      "%s -> %s",
      ControlCommandParserStateag_show(g_control_command_parser_state.tag),
      ControlCommandParserStateag_show(tag));

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
    g_control_command_parser_state.namespace_buffer[namespace_len] = '\0';
    break;
  }
  case CONTROL_COMMAND_PARSER_STATE_COMMAND_ID: {
    assert(data == NULL);
    break;
  }
  }
}

/// @brief Parse a chunk.
///
/// This is the incremental command parser. It parses a chunk of bytes and
/// updates the command parser state.
static void
control_command_parser_handle_chunk(const size_t chunk_size,
                                    const uint8_t chunk[chunk_size]) {
  DEBUG_TRACE("Received chunk of size %zd.", chunk_size);
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
        DEBUG_TRACE("Matched control_magic byte %d",
                    g_control_command_parser_state.header_pos);
        // ...move on the the next state...
        ++g_control_command_parser_state.header_pos;
        // if header_pos moves out of control_magic...
        if (g_control_command_parser_state.header_pos >= CONTROL_MAGIC_LEN) {
          // ...continue reading the namespace length...
          control_command_parser_enter_state(
              CONTROL_COMMAND_PARSER_STATE_PROTOCOL_VERSION, NULL);
        }
        // ...continue processing with the _next_ byte...
        continue;
      }
      // if the next byte is not the expected byte...
      else {
        // ...there has been a protocol error...
        // ...restart with the _current_ byte...
        control_command_parser_enter_state(CONTROL_COMMAND_PARSER_STATE_MAGIC,
                                           &current_byte);
        // ...continue processing with the _next_ byte...
        continue;
      }
    }
    // the parser is currently reading the protocol version...
    case CONTROL_COMMAND_PARSER_STATE_PROTOCOL_VERSION: {
      DEBUG_TRACE("Matched protocol version byte %d", current_byte);
      // if the message version matches the protocol version...
      if (current_byte == EVENTLOG_SOCKET_CONTROL_PROTOCOL_VERSION) {
        // ...then we should be able to parse the message...
        // ...continue processing with the _next_ byte...
        control_command_parser_enter_state(
            CONTROL_COMMAND_PARSER_STATE_NAMESPACE_LEN, NULL);
        continue;
      } else {
        // ...otherwise, let's not try and parse this message...
        // ...restart with the _current_ byte...
        control_command_parser_enter_state(CONTROL_COMMAND_PARSER_STATE_MAGIC,
                                           &current_byte);
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
        control_command_parser_enter_state(CONTROL_COMMAND_PARSER_STATE_MAGIC,
                                           &current_byte);
        continue;
      } else {
        DEBUG_TRACE("Matched namespace_len byte %d", current_byte);
        // otherwise, accept the namespace length and move to the next state...
        control_command_parser_enter_state(
            CONTROL_COMMAND_PARSER_STATE_NAMESPACE, &current_byte);
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
      DEBUG_TRACE("Matched namespace %.*s",
                  g_control_command_parser_state.namespace_buffer_len,
                  g_control_command_parser_state.namespace_buffer);

      // ...try to resolve the namespace...
      const EventlogSocketControlNamespace *namespace =
          control_namespace_store_resolve(
              g_control_command_parser_state.namespace_buffer_len,
              g_control_command_parser_state.namespace_buffer);
      // if the namespace was successfully resolved, then...
      if (namespace != NULL) {
        DEBUG_TRACE("Resolved namespace %.*s",
                    g_control_command_parser_state.namespace_buffer_len,
                    g_control_command_parser_state.namespace_buffer);
        // move chunk_index by the number of copied bytes less one,
        // because the chunk_index will be updated when we reenter the for loop.
        // note: the subtraction is safe because available_bytes > 0
        chunk_index += available_bytes - 1;
        // ...move to the next state...
        control_command_parser_enter_state(
            CONTROL_COMMAND_PARSER_STATE_COMMAND_ID, NULL);
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
        DEBUG_ERROR("unknown namespace %.*s",
                    g_control_command_parser_state.namespace_buffer_len,
                    g_control_command_parser_state.namespace_buffer);
        // ...restart with the _current_ byte...
        control_command_parser_enter_state(CONTROL_COMMAND_PARSER_STATE_MAGIC,
                                           &current_byte);
        // ...continue processing with the _next_ byte...
        continue;
      }
    }
    case CONTROL_COMMAND_PARSER_STATE_COMMAND_ID: {
      DEBUG_TRACE("Matched command_id byte 0x%02x", current_byte);
      // Handle the command.
      control_command_handle(g_control_command_parser_state.namespace,
                             current_byte);
      // ...restart _without_ the current byte...
      control_command_parser_enter_state(CONTROL_COMMAND_PARSER_STATE_MAGIC,
                                         NULL);
      // ...continue processing with the _next_ byte...
      continue;
    }
    }
  }
}

/******************************************************************************
 * Control Thread
 ******************************************************************************/

/// @brief The control thread reads chunks of this size from the eventlog
/// socket.
#define CHUNK_SIZE 256

/// @brief A volatile view of the eventlog socket file descriptor.
///
/// This file descriptor is *not* managed by the control thread.
static const volatile int *g_control_fd_ptr = NULL;

/// @brief A pointer to the mutex that guards the eventlog socket file
/// descriptor.
///
/// See `g_control_fd_ptr`.
static pthread_mutex_t *g_control_fd_mutex_ptr = NULL;

/// @brief A pointer to the condition used to signal a new connection on the
/// eventlog socket file descriptor.
///
/// This condition should be used with `g_control_fd_mutex_ptr`.
static pthread_cond_t *g_new_conn_cond_ptr = NULL;

/// @brief A stable view the eventlog socket file descriptor.
///
/// See `g_control_fd_ptr`.
static int g_control_fd = -1;

/// Reset the control thread state when the connection changes.
///
/// @param new_control_fd The new eventlog socket file descriptor. May be `-1`.
static void control_fd_reset_to(const int new_control_fd) {
  DEBUG_DEBUG("%s", "Resetting control server state.");
  // Reset eventlog socket file descriptor.
  g_control_fd = new_control_fd;
  // Reset parser state.
  control_command_parser_enter_state(CONTROL_COMMAND_PARSER_STATE_MAGIC, NULL);
}

/// @brief Wait for a new connection.
///
/// @pre The caller must have a lock on `g_control_fd_mutex_ptr`.
/// @post The caller will have a lock on `g_control_fd_mutex_ptr`.
static void control_fd_wait_for_connection(void) {
  DEBUG_DEBUG("%s", "Waiting to be notified of new connection.");
  pthread_cond_wait(g_new_conn_cond_ptr, g_control_fd_mutex_ptr);
}

static void *control_loop(void *arg) {
  (void)arg;

  assert(g_control_fd_ptr != NULL);
  assert(g_control_fd_mutex_ptr != NULL);
  assert(g_new_conn_cond_ptr != NULL);

  // Allocate memory for chunks:
  uint8_t *const chunk = malloc(CHUNK_SIZE);

  // Wait for the GHC RTS to become ready.
  control_wait_ghc_rts_ready();

  /* BEGIN: The main control loop. */
  while (true) {
    DEBUG_TRACE("%s", "Starting new control iteration.");

    /* BEGIN: Wake up. */
    // At the start of each control iteration, we update the eventlog socket
    // file descriptor.

    // Acquire the lock on the connection file description.
    pthread_mutex_lock(g_control_fd_mutex_ptr);

    // Read current connection file description.
    const int new_control_fd = *g_control_fd_ptr;
    if (g_control_fd != new_control_fd) {
      DEBUG_TRACE("Old connection fd: %d", g_control_fd);
      DEBUG_TRACE("New connection fd: %d", new_control_fd);
    }

    // If there WAS NO connection and there IS NO connection, then...
    if (g_control_fd == -1 && new_control_fd == -1) {
      DEBUG_TRACE("%s", "There WAS NO connection and there IS NO connection.");
      // ...wait to be notified of a new connection...
      control_fd_wait_for_connection();
      // ...release the lock...
      pthread_mutex_unlock(g_control_fd_mutex_ptr);
      // ...and re-enter the loop.
      continue;
    }

    // If there WAS NO connection but there IS A connection, then...
    else if (g_control_fd == -1 && new_control_fd != -1) {
      DEBUG_TRACE("%s", "There WAS NO connection but there IS A connection.");
      // ...DON'T wait to be notified of a new connection...
      // ...we may we have already missed the signal...
      // ...reset the control server state...
      control_fd_reset_to(new_control_fd);
      // ...continue to try to handle a command.
    }

    // If there WAS A connection but there IS NO connection, then...
    else if (g_control_fd != -1 && new_control_fd == -1) {
      DEBUG_TRACE("%s", "There WAS A connection but there IS NO connection.");
      // ...reset the control server state...
      control_fd_reset_to(new_control_fd);
      // ...wait to be notified of a new connection...
      control_fd_wait_for_connection();
      // ...release the lock...
      pthread_mutex_unlock(g_control_fd_mutex_ptr);
      // ...and re-enter the loop.
      continue;
    }

    // If there WAS A connection and there IS A connection, then...
    else if (g_control_fd != -1 && new_control_fd != -1) {
      // If it is A DIFFERENT connection, then...
      if (g_control_fd != new_control_fd) {
        DEBUG_TRACE(
            "%s",
            "There WAS A connection and there IS A DIFFERENT connection.");
        // ...DON'T wait to be notified of a new connection...
        // ...we may we have already missed the signal...
        // ...reset the control server state...
        control_fd_reset_to(new_control_fd);
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
    pthread_mutex_unlock(g_control_fd_mutex_ptr);

    // Check that g_control_fd is up-to-date:
    assert(g_control_fd == new_control_fd);
    /* END: Wake up. */

    /* BEGIN: Wait for input. */
    // The eventlog socket is marked as non-blocking. If we try to receive
    // data, the `recv` function returns immediately. This works, but causes
    // us to go through the control loop *very* quickly. Instead, we wait for
    // input data.

    // note: POLLHUP and POLLRDHUP are output only and are ignored input.
    struct pollfd pfds[1] = {{
        .fd = g_control_fd,
        .events = POLLIN,
        .revents = 0,
    }};
    const int ready_or_error = poll(pfds, 1, POLL_LISTEN_TIMEOUT);
    // if ready_or_error is -1, an error occurred...
    if (ready_or_error == -1) {
      // if errno is EINTR, the receive was interrupted...
      if (errno == EINTR) {
        goto onexit;
      }
      // if errno is anything else, there is some other error...
      else {
        DEBUG_ERRNO("poll() failed");
        continue;
      }
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
        DEBUG_TRACE("Connection on fd %d closed.", g_control_fd);
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
    const ssize_t chunk_size_or_error =
        recv(g_control_fd, chunk, CHUNK_SIZE, 0);
    // if num_bytes_or_error == -1, an error occurred...
    if (chunk_size_or_error == -1) {
      // if errno is EINTR, the receive was interrupted...
      if (errno == EINTR) {
        goto onexit;
      }
      // if errno is EGAIN or EWOULDBLOCK, recv timed out...
      else if (errno == EAGAIN || errno == EWOULDBLOCK) {
        DEBUG_TRACE("%s", "recv() timed out or was interrupted.");
        // note: the socket should have SO_RCVTIMEO set.
        continue;
      }
      // if errno is anything else, there is some other error...
      else {
        DEBUG_ERRNO("recv() failed");
        continue;
      }
    }
    // if num_bytes_or_error == 0, the connection was closed...
    else if (chunk_size_or_error == 0) {
      DEBUG_TRACE("%s", "recv() failed: the connection was closed.");
      // todo: wait for a new connection...
      DEBUG_TRACE("Connection on fd %d closed.", g_control_fd);
      continue;
    }
    // otherwise, handle the received chunk...
    else {
      DEBUG_TRACE("recv() read %zd bytes", chunk_size_or_error);
      assert(chunk_size_or_error > 0);
      control_command_parser_handle_chunk(chunk_size_or_error, chunk);
    }
    /* END: Handle up to one chunk of input. */
  }
  /* END: The main control loop. */
  goto onexit;

onexit:
  free(chunk);
  return NULL;
}

/* HIDDEN - see documentation in control.h */
EventlogSocketStatus HIDDEN eventlog_socket_control_start(
    pthread_t *const control_thread, const volatile int *const control_fd_ptr,
    pthread_mutex_t *const control_fd_mutex_ptr,
    pthread_cond_t *const new_conn_cond_ptr) {
  DEBUG_DEBUG("%s", "Starting control thread.");
  g_control_fd_ptr = control_fd_ptr;
  g_control_fd_mutex_ptr = control_fd_mutex_ptr;
  g_new_conn_cond_ptr = new_conn_cond_ptr;
  {
    const int success_or_errno =
        pthread_create(control_thread, NULL, control_loop, NULL);
    if (success_or_errno != 0) {
      DEBUG_ERRNO("pthread_create() failed");
      return STATUS_FROM_PTHREAD_ERROR(success_or_errno);
    }
  }
  {
    const int success_or_errno = pthread_detach(*control_thread);
    if (success_or_errno != 0) {
      DEBUG_ERRNO("pthread_detach() failed");
      return STATUS_FROM_PTHREAD_ERROR(success_or_errno);
    }
  }
  return STATUS_FROM_CODE(EVENTLOG_SOCKET_OK);
}
