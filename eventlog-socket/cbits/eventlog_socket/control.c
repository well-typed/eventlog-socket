/// @file control.c
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

#define CONTROL_MAGIC_LEN 4
static const uint8_t control_magic[CONTROL_MAGIC_LEN] = {
    [0] = 0xF0,
    [1] = 0x9E,
    [2] = 0x97,
    [3] = 0x8C,
};

#define BUILTIN_NAMESPACE "eventlog-socket"
#define BUILTIN_NAMESPACE_ID 0

#define BUILTIN_COMMAND_ID_START_HEAP_PROFILING 0
#define BUILTIN_COMMAND_ID_STOP_HEAP_PROFILING 1
#define BUILTIN_COMMAND_ID_REQUEST_HEAP_PROFILE 2

/******************************************************************************
 * namespace registry
 ******************************************************************************/

typedef struct control_namespace_store_entry control_namespace_store_entry_t;

struct control_namespace_store_entry {
  const size_t namespace_len;
  const char *const namespace;
  const uint8_t namespace_id;
  control_namespace_store_entry_t *next;
};

control_namespace_store_entry_t g_control_namespace_store = {
    .namespace = BUILTIN_NAMESPACE,
    .namespace_len = strlen(BUILTIN_NAMESPACE),
    .next = NULL,
};

pthread_mutex_t g_control_namespace_store_mutex = PTHREAD_MUTEX_INITIALIZER;

bool control_namespace_store_match(
    const control_namespace_store_entry_t *namespace_entry,
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

bool control_namespace_store_resolve(
    const size_t namespace_len, const char namespace[namespace_len],
    eventlog_socket_control_namespace_id_t *namespace_id_out) {

  // Acquire a lock on g_control_namespace_store.
  pthread_mutex_lock(&g_control_namespace_store_mutex);

  // Initialise the namespace_entry pointer.
  control_namespace_store_entry_t *namespace_entry = &g_control_namespace_store;

  // Let's start counting namespace ids.
  eventlog_socket_control_namespace_id_t namespace_id = 0;

  while (namespace_entry != NULL) {
    // Is this the namespace you are looking for?
    if (control_namespace_store_match(namespace_entry, namespace_len,
                                      namespace)) {
      // Write out the namespace_id.
      *namespace_id_out = namespace_id;
      // Release the lock on g_control_namespace_store.
      pthread_mutex_unlock(&g_control_namespace_store_mutex);
      return true;
    }
    // Otherwise, continue with the next namespace_entry.
    namespace_entry = namespace_entry->next;
    ++namespace_id;
  }
  // Release the lock on g_control_namespace_store.
  pthread_mutex_unlock(&g_control_namespace_store_mutex);
  return false;
}

const eventlog_socket_control_namespace_id_t *
eventlog_socket_control_register_namespace(
    const uint8_t namespace_len, const char namespace[namespace_len]) {

  // Acquire the lock on g_control_namespace_store.
  pthread_mutex_lock(&g_control_namespace_store_mutex);

  // Initialise the namespace_entry pointer.
  control_namespace_store_entry_t *namespace_entry = &g_control_namespace_store;

  // Let's start counting namespace ids.
  eventlog_socket_control_namespace_id_t namespace_id = 0;

  // Is the requested namespace already registered?
  do {
    // Is this the namespace you are trying to register?
    if (control_namespace_store_match(namespace_entry, namespace_len,
                                      namespace)) {
      // If so, return false.
      pthread_mutex_unlock(&g_control_namespace_store_mutex);
      return NULL;
    }
    // Is this the last namespace_entry?
    if (namespace_entry->next == NULL) {
      // If so, stop.
      break;
    }
    // Otherwise, continue with the next entry.
    namespace_entry = namespace_entry->next;
    ++namespace_id;
  } while (true);

  // Register the requested namespace.
  assert(namespace_entry != NULL);
  assert(namespace_entry->next == NULL);
  ++namespace_id;
  char *const next_namespace = malloc(namespace_len + 1);
  strncpy(next_namespace, namespace, namespace_len);
  next_namespace[namespace_len] = '\0';
  const control_namespace_store_entry_t next =
      (control_namespace_store_entry_t){
          .namespace_len = namespace_len,
          .namespace = next_namespace,
          .namespace_id = namespace_id,
          .next = NULL,
      };
  namespace_entry->next = malloc(sizeof(control_namespace_store_entry_t));
  memcpy(namespace_entry->next, &next, sizeof(control_namespace_store_entry_t));
  DEBUG_TRACE("Registered namespace '%s' under ID %d", namespace, namespace_id);

  // Return success.
  pthread_mutex_unlock(&g_control_namespace_store_mutex);
  return &namespace_entry->next->namespace_id;
}

/******************************************************************************
 * command registry
 ******************************************************************************/

// todo: store commands in a nested linked list, where the outer linked list
//       is indexed by namespace ids and the inner linked list is indexed by
//       command ids. (perhaps both could be arrays, because namespaces and
//       commands are added only infrequently.)
// todo: don't store the explicit namespace and command ids with each command,
//       but rather have those be an index into a data structure.

typedef struct control_command_store_entry control_command_store_entry_t;

struct control_command_store_entry {
  eventlog_socket_control_namespace_id_t namespace_id;
  eventlog_socket_control_command_id_t command_id;
  eventlog_socket_control_command_handler_t *handler;
  void *user_data;
  control_command_store_entry_t *next;
};

// Builtin control command handlers

// Start heap profiling
static void
control_start_heap_profiling(eventlog_socket_control_command_t command,
                             void *user_data) {
  (void)command;
  (void)user_data;
  DEBUG_TRACE("%s", "control: startHeapProfiling");
  startHeapProfTimer();
}

// Stop heap profiling
static void
control_stop_heap_profiling(eventlog_socket_control_command_t command,
                            void *user_data) {
  (void)command;
  (void)user_data;
  DEBUG_TRACE("%s", "control: stopHeapProfiling");
  stopHeapProfTimer();
}

// Request heap profil
static void
control_request_heap_profile(eventlog_socket_control_command_t command,
                             void *user_data) {
  (void)command;
  (void)user_data;
  DEBUG_TRACE("%s", "control: requestHeapProfile");
  requestHeapCensus();
}

// Registry of control command handlers
static control_command_store_entry_t *g_control_command_store =
    &(control_command_store_entry_t){
        .namespace_id = BUILTIN_NAMESPACE_ID,
        .command_id = BUILTIN_COMMAND_ID_START_HEAP_PROFILING,
        .handler = control_start_heap_profiling,
        .user_data = NULL,
        .next = &(control_command_store_entry_t){
            .namespace_id = BUILTIN_NAMESPACE_ID,
            .command_id = BUILTIN_COMMAND_ID_STOP_HEAP_PROFILING,
            .handler = control_stop_heap_profiling,
            .user_data = NULL,
            .next = &(control_command_store_entry_t){
                .namespace_id = BUILTIN_NAMESPACE_ID,
                .command_id = BUILTIN_COMMAND_ID_REQUEST_HEAP_PROFILE,
                .handler = control_request_heap_profile,
                .user_data = NULL,
                .next = NULL,
            }}};

static pthread_mutex_t g_control_handlers_mutex = PTHREAD_MUTEX_INITIALIZER;

bool eventlog_socket_control_register_command(
    eventlog_socket_control_command_t command,
    eventlog_socket_control_command_handler_t handler, void *user_data) {
  if (handler == NULL) {
    return false;
  }

  bool success = false;
  pthread_mutex_lock(&g_control_handlers_mutex);
  control_command_store_entry_t *entry = g_control_command_store;
  while (entry != NULL) {
    if (entry->namespace_id == command.namespace_id &&
        entry->command_id == command.command_id) {
      DEBUG_ERROR(
          "warning: duplicate registration for namespace 0x%08x command "
          "0x%02x; keeping existing handler\n",
          command.namespace_id, command.command_id);
      goto out;
    }
    entry = entry->next;
  }

  entry = malloc(sizeof(control_command_store_entry_t));
  if (entry == NULL) {
    DEBUG_ERROR("%s", "control: failed to allocate handler entry");
    goto out;
  }
  entry->namespace_id = command.namespace_id;
  entry->command_id = command.command_id;
  entry->handler = handler;
  entry->user_data = user_data;
  entry->next = g_control_command_store;
  g_control_command_store = entry;
  success = true;

out:
  pthread_mutex_unlock(&g_control_handlers_mutex);
  return success;
}

/******************************************************************************
 * control thread
 ******************************************************************************/

static int g_control_fd = -1;

typedef enum {
  CONTROL_COMMAND_PARSER_STATE_MAGIC,
  CONTROL_COMMAND_PARSER_STATE_PROTOCOL_VERSION,
  CONTROL_COMMAND_PARSER_STATE_NAMESPACE_LEN,
  CONTROL_COMMAND_PARSER_STATE_NAMESPACE,
  CONTROL_COMMAND_PARSER_STATE_COMMAND_ID,
} control_command_parser_state_tag_t;

const char *
control_command_parser_state_tag_show(control_command_parser_state_tag_t tag) {
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

typedef struct {
  control_command_parser_state_tag_t tag;
  union {
    // if .tag == CONTROL_COMMAND_PARSER_STATE_MAGIC
    // the state stores:
    // * the position of the next header byte
    /// @invariant header_pos < CONTROL_MAGIC_LEN
    uint8_t header_pos;

    // if .tag == CONTROL_COMMAND_PARSER_STATE_PROTOCOL_VERSION
    // the state stores nothing.

    // if .tag == CONTROL_COMMAND_PARSER_STATE_NAMESPACE_LEN
    // the state stores nothing.

    // if .tag == CONTROL_COMMAND_PARSER_STATE_NAMESPACE
    // the state stores:
    // * the expected namespace length in bytes
    // * the position of the next namespace byte
    // * all previously received namespace bytes
    struct {
      /// @invariant namespace_buffer_len > 0
      uint8_t namespace_buffer_len;
      /// @invariant namespace_buffer_pos < namespace_buffer_len
      uint8_t namespace_buffer_pos;
      /// @invariant sizeof(namespace_buffer) == namespace_buffer_len
      char *namespace_buffer;
    };

    // if .tag == CONTROL_COMMAND_PARSER_STATE_COMMAND
    // the state stores:
    // * the current namespace id
    eventlog_socket_control_namespace_id_t command_namespace_id;
  };
} control_command_parser_state_t;

control_command_parser_state_t g_control_command_parser_state = {
    .tag = CONTROL_COMMAND_PARSER_STATE_MAGIC,
    .header_pos = 0,
};

typedef enum {
  CONTROL_FD_STATUS_CLOSED,
  CONTROL_FD_STATUS_ERROR,
  CONTROL_FD_STATUS_RETRY,
  CONTROL_FD_STATUS_READY,
  CONTROL_FD_STATUS_INTR,
} control_fd_status_t;

static bool control_command_handle(eventlog_socket_control_command_t command) {
  DEBUG_TRACE("Handle command in namespace %02x with id %02x",
              command.namespace_id, command.command_id);
  eventlog_socket_control_command_handler_t *handler = NULL;
  void *user_data = NULL;

  pthread_mutex_lock(&g_control_handlers_mutex);
  control_command_store_entry_t *entry = g_control_command_store;
  while (entry != NULL) {
    if (entry->namespace_id == command.namespace_id &&
        entry->command_id == command.command_id) {
      handler = entry->handler;
      user_data = entry->user_data;
      break;
    }
    entry = entry->next;
  }
  pthread_mutex_unlock(&g_control_handlers_mutex);

  if (handler != NULL) {
    handler(command, user_data);
    return true;
  } else {
    DEBUG_ERROR("control: unhandled command namespace=0x%08x id=0x%02x",
                command.namespace_id, command.command_id);
    return false;
  }
}

static void
control_command_parser_enter_state(const control_command_parser_state_tag_t tag,
                                   const uint8_t *data) {
  DEBUG_TRACE(
      "%s -> %s",
      control_command_parser_state_tag_show(g_control_command_parser_state.tag),
      control_command_parser_state_tag_show(tag));

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
        *data == control_magic[0]) {
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
    assert(data != NULL);
    g_control_command_parser_state.command_namespace_id = *data;
    break;
  }
  }
}

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
          control_magic[g_control_command_parser_state.header_pos];
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
      DEBUG_TRACE("matched namespace '%s'",
                  g_control_command_parser_state.namespace_buffer);

      // ...try to resolve the namespace...
      uint8_t namespace_id = UCHAR_MAX;
      const bool namespace_id_found = control_namespace_store_resolve(
          g_control_command_parser_state.namespace_buffer_len,
          g_control_command_parser_state.namespace_buffer, &namespace_id);
      // if the namespace was successfully resolved, then...
      if (namespace_id_found) {
        DEBUG_TRACE("resolved namespace '%s' to %d",
                    g_control_command_parser_state.namespace_buffer,
                    namespace_id);
        // move chunk_index by the number of copied bytes less one,
        // because the chunk_index will be updated when we reenter the for loop.
        // note: the subtraction is safe because available_bytes > 0
        chunk_index += available_bytes - 1;
        // ...move to the next state...
        control_command_parser_enter_state(
            CONTROL_COMMAND_PARSER_STATE_COMMAND_ID, &namespace_id);
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
        DEBUG_ERROR("unknown namespace %s",
                    g_control_command_parser_state.namespace_buffer);
        // ...restart with the _current_ byte...
        control_command_parser_enter_state(CONTROL_COMMAND_PARSER_STATE_MAGIC,
                                           &current_byte);
        // ...continue processing with the _next_ byte...
        continue;
      }
    }
    case CONTROL_COMMAND_PARSER_STATE_COMMAND_ID: {
      DEBUG_TRACE("matched command_id byte '%d'", current_byte);
      // Create a command.
      const eventlog_socket_control_command_t command = {
          .namespace_id = g_control_command_parser_state.command_namespace_id,
          .command_id = current_byte,
      };
      // Handle the command.
      control_command_handle(command);
      // ...restart _without_ the current byte...
      control_command_parser_enter_state(CONTROL_COMMAND_PARSER_STATE_MAGIC,
                                         NULL);
      // ...continue processing with the _next_ byte...
      continue;
    }
    }
  }
}

static volatile bool g_ghc_rts_ready = false;
static pthread_mutex_t g_ghc_rts_ready_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_ghc_rts_ready_cond = PTHREAD_COND_INITIALIZER;

static void control_wait_ghc_rts_ready(void) {
  DEBUG_TRACE("%s", "Waiting for signal that GHC RTS is ready.");
  pthread_mutex_lock(&g_ghc_rts_ready_mutex);
  while (!g_ghc_rts_ready) {
    pthread_cond_wait(&g_ghc_rts_ready_cond, &g_ghc_rts_ready_mutex);
  }
  pthread_mutex_unlock(&g_ghc_rts_ready_mutex);
}

static const volatile int *g_control_fd_ptr = NULL;
static pthread_mutex_t *g_control_fd_mutex_ptr = NULL;
static pthread_cond_t *g_new_conn_cond_ptr = NULL;

static void control_fd_reset_to(const int new_control_fd) {
  DEBUG_TRACE("%s", "Resetting control server state.");
  g_control_fd = new_control_fd;
  // todo: reset parser state
}

/// @pre must have lock on @link g_control_fd_mutex_ptr
static void control_fd_wait_for_connection(void) {
  DEBUG_TRACE("%s", "Waiting to be notified of new connection.");
  pthread_cond_wait(g_new_conn_cond_ptr, g_control_fd_mutex_ptr);
}

static control_fd_status_t control_fd_update(void) {

  // Create the return value.
  control_fd_status_t ret = CONTROL_FD_STATUS_ERROR;

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
    ret = CONTROL_FD_STATUS_RETRY;
  }

  // If there WAS NO connection but there IS A connection, then...
  else if (g_control_fd == -1 && new_control_fd != -1) {
    DEBUG_TRACE("%s", "There WAS NO connection but there IS A connection.");
    // ...DON'T wait to be notified of a new connection...
    // ...we may we have already missed the signal...
    // ...reset the control server state...
    control_fd_reset_to(new_control_fd);
    // ...continue to try to handle a command.
    ret = CONTROL_FD_STATUS_READY;
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
    ret = CONTROL_FD_STATUS_RETRY;
  }

  // If there WAS A connection and there IS A connection, then...
  else if (g_control_fd != -1 && new_control_fd != -1) {
    // If it is A DIFFERENT connection, then...
    if (g_control_fd != new_control_fd) {
      DEBUG_TRACE(
          "%s", "There WAS A connection and there IS A DIFFERENT connection.");
      // ...DON'T wait to be notified of a new connection...
      // ...we may we have already missed the signal...
      // ...reset the control server state...
      control_fd_reset_to(new_control_fd);
      // ...continue to try to handle a command.
      ret = CONTROL_FD_STATUS_READY;
    }
    // If it is THE SAME connection, then...
    else {
      // ...continue to try to handle a command.
      DEBUG_TRACE("%s",
                  "There WAS A connection and there IS THE SAME connection.");
      // ...continue to try to handle a command.
      ret = CONTROL_FD_STATUS_READY;
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

  return ret;
}

static void *control_loop(void *arg) {
  (void)arg;

  assert(g_control_fd_ptr != NULL);
  assert(g_control_fd_mutex_ptr != NULL);
  assert(g_new_conn_cond_ptr != NULL);

  // Allocate memory for chunks:
  const long chunk_size = sysconf(_SC_PAGESIZE);
  if (chunk_size == -1) {
    DEBUG_ERRNO("sysconf(_SC_PAGESIZE) failed");
  }
  uint8_t *chunk;
  posix_memalign((void **)&chunk, chunk_size, chunk_size);

  // Wait for the GHC RTS to become ready.
  control_wait_ghc_rts_ready();

  while (true) {
    DEBUG_TRACE("%s", "Starting new control iteration.");

    // Update the connection fd:
    const control_fd_status_t update_fd_status = control_fd_update();
    switch (update_fd_status) {
    case CONTROL_FD_STATUS_READY:
      break;
    case CONTROL_FD_STATUS_CLOSED:
    case CONTROL_FD_STATUS_ERROR:
    case CONTROL_FD_STATUS_RETRY:
      continue;
    case CONTROL_FD_STATUS_INTR:
      goto onexit;
    }

    // wait for input:
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

    // read a chunk:
    const ssize_t chunk_size_or_error =
        recv(g_control_fd, chunk, chunk_size, 0);
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
  }
  goto onexit;

onexit:
  free(chunk);
  return NULL;
}

/******************************************************************************
 * public interface
 ******************************************************************************/

void HIDDEN eventlog_socket_control_signal_ghc_rts_ready(void) {
  DEBUG_TRACE("%s", "Sending signal that GHC RTS is ready.");
  pthread_mutex_lock(&g_ghc_rts_ready_mutex);
  if (!g_ghc_rts_ready) {
    g_ghc_rts_ready = true;
    pthread_cond_broadcast(&g_ghc_rts_ready_cond);
  }
  pthread_mutex_unlock(&g_ghc_rts_ready_mutex);
}

void HIDDEN eventlog_socket_control_start(
    pthread_t *control_thread, const volatile int *const control_fd_ptr,
    pthread_mutex_t *control_fd_mutex_ptr, pthread_cond_t *new_conn_cond_ptr) {
  DEBUG_TRACE("%s", "Starting control thread.");
  g_control_fd_ptr = control_fd_ptr;
  g_control_fd_mutex_ptr = control_fd_mutex_ptr;
  g_new_conn_cond_ptr = new_conn_cond_ptr;
  const int create_or_error =
      pthread_create(control_thread, NULL, control_loop, NULL);
  if (create_or_error != 0) {
    DEBUG_ERRNO("pthread_create() failed");
    return;
  }
  const int detach_or_error = pthread_detach(*control_thread);
  if (detach_or_error != 0) {
    DEBUG_ERRNO("pthread_detach() failed");
  }
}
