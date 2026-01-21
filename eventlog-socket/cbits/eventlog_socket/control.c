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

#define CONTROL_READ_COMMAND_CHUNK_SIZE 32
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

typedef struct eventlog_socket_control_namespace_entry
    eventlog_socket_control_namespace_entry_t;

struct eventlog_socket_control_namespace_entry {
  size_t namespace_len;
  char *namespace;
  eventlog_socket_control_namespace_entry_t *next;
};

eventlog_socket_control_namespace_entry_t g_control_namespace_list = {
    .namespace = BUILTIN_NAMESPACE,
    .namespace_len = strlen(BUILTIN_NAMESPACE),
    .next = NULL,
};

pthread_mutex_t g_control_namespace_list_mutex = PTHREAD_MUTEX_INITIALIZER;

bool control_namespace_match(
    const eventlog_socket_control_namespace_entry_t *namespace_entry,
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

bool control_namespace_resolve(
    const size_t namespace_len, const char namespace[namespace_len],
    eventlog_socket_control_namespace_id_t *namespace_id_out) {

  // Acquire a lock on g_control_namespace_list.
  pthread_mutex_lock(&g_control_namespace_list_mutex);

  // Initialise the namespace_entry pointer.
  eventlog_socket_control_namespace_entry_t *namespace_entry =
      &g_control_namespace_list;

  // Let's start counting namespace ids.
  eventlog_socket_control_namespace_id_t namespace_id = 0;

  while (namespace_entry != NULL) {
    // Is this the namespace you are looking for?
    if (control_namespace_match(namespace_entry, namespace_len, namespace)) {
      // Write out the namespace_id.
      *namespace_id_out = namespace_id;
      // Release the lock on g_control_namespace_list.
      pthread_mutex_unlock(&g_control_namespace_list_mutex);
      return true;
    }
    // Otherwise, continue with the next namespace_entry.
    namespace_entry = namespace_entry->next;
    ++namespace_id;
  }
  // Release the lock on g_control_namespace_list.
  pthread_mutex_unlock(&g_control_namespace_list_mutex);
  return false;
}

bool eventlog_socket_control_register_namespace(
    const uint8_t namespace_len, const char namespace[namespace_len],
    eventlog_socket_control_namespace_id_t *namespace_id_out) {

  // Acquire the lock on g_control_namespace_list.
  pthread_mutex_lock(&g_control_namespace_list_mutex);

  // Initialise the namespace_entry pointer.
  eventlog_socket_control_namespace_entry_t *namespace_entry =
      &g_control_namespace_list;

  // Let's start counting namespace ids.
  eventlog_socket_control_namespace_id_t namespace_id = 0;

  // Is the requested namespace already registered?
  do {
    // Is this the namespace you are trying to register?
    if (control_namespace_match(namespace_entry, namespace_len, namespace)) {
      // If so, return false.
      pthread_mutex_unlock(&g_control_namespace_list_mutex);
      return false;
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
  eventlog_socket_control_namespace_entry_t *next =
      malloc(sizeof(eventlog_socket_control_namespace_entry_t));
  next->namespace_len = namespace_len;
  next->namespace = malloc(namespace_len + 1);
  next->namespace[namespace_len] = '\0';
  strncpy(next->namespace, namespace, namespace_len);
  namespace_entry->next = next;
  DEBUG_TRACE("Registered namespace '%s' under ID %d", namespace, namespace_id);

  // Write out the new namespace_id.
  *namespace_id_out = namespace_id;

  // Return success.
  pthread_mutex_unlock(&g_control_namespace_list_mutex);
  return true;
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

typedef struct eventlog_socket_control_command_item
    eventlog_socket_control_command_item_t;

struct eventlog_socket_control_command_item {
  eventlog_socket_control_namespace_id_t namespace_id;
  eventlog_socket_control_command_id_t command_id;
  eventlog_socket_control_command_handler_t *handler;
  void *user_data;
  eventlog_socket_control_command_item_t *next;
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
static eventlog_socket_control_command_item_t *g_control_handlers =
    &(eventlog_socket_control_command_item_t){
        .namespace_id = BUILTIN_NAMESPACE_ID,
        .command_id = BUILTIN_COMMAND_ID_START_HEAP_PROFILING,
        .handler = control_start_heap_profiling,
        .user_data = NULL,
        .next = &(eventlog_socket_control_command_item_t){
            .namespace_id = BUILTIN_NAMESPACE_ID,
            .command_id = BUILTIN_COMMAND_ID_STOP_HEAP_PROFILING,
            .handler = control_stop_heap_profiling,
            .user_data = NULL,
            .next = &(eventlog_socket_control_command_item_t){
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
  eventlog_socket_control_command_item_t *entry = g_control_handlers;
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

  entry = malloc(sizeof(eventlog_socket_control_command_item_t));
  if (entry == NULL) {
    DEBUG_ERROR("%s", "control: failed to allocate handler entry");
    goto out;
  }
  entry->namespace_id = command.namespace_id;
  entry->command_id = command.command_id;
  entry->handler = handler;
  entry->user_data = user_data;
  entry->next = g_control_handlers;
  g_control_handlers = entry;
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
  CONTROL_READ_MAGIC,
  CONTROL_READ_PROTOCOL_VERSION,
  CONTROL_READ_NAMESPACE_LEN,
  CONTROL_READ_NAMESPACE,
  CONTROL_READ_COMMAND_ID,
} control_read_state_tag_t;

const char *control_read_state_tag_show(control_read_state_tag_t tag) {
  switch (tag) {
  case CONTROL_READ_MAGIC:
    return "CONTROL_READ_MAGIC";
  case CONTROL_READ_PROTOCOL_VERSION:
    return "CONTROL_READ_PROTOCOL_VERSION";
  case CONTROL_READ_NAMESPACE_LEN:
    return "CONTROL_READ_NAMESPACE_LEN";
  case CONTROL_READ_NAMESPACE:
    return "CONTROL_READ_NAMESPACE";
  case CONTROL_READ_COMMAND_ID:
    return "CONTROL_READ_COMMAND_ID";
  }
}

typedef struct {
  control_read_state_tag_t tag;
  union {
    // if .tag == CONTROL_READ_MAGIC
    // the state stores:
    // * the position of the next header byte
    /// @invariant header_pos < CONTROL_MAGIC_LEN
    uint8_t header_pos;

    // if .tag == CONTROL_READ_PROTOCOL_VERSION
    // the state stores nothing.

    // if .tag == CONTROL_READ_NAMESPACE_LEN
    // the state stores nothing.

    // if .tag == CONTROL_READ_NAMESPACE
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

    // if .tag == CONTROL_READ_COMMAND
    // the state stores:
    // * the current namespace id
    eventlog_socket_control_namespace_id_t command_namespace_id;
  };
} control_read_state_t;

control_read_state_t g_control_read_state = {
    .tag = CONTROL_READ_MAGIC,
    .header_pos = 0,
};

typedef enum {
  CONTROL_FD_CLOSED,
  CONTROL_FD_ERROR,
  CONTROL_FD_RETRY,
  CONTROL_FD_READY,
  CONTROL_FD_INTR,
} control_fd_status_t;

static bool control_handle_command(eventlog_socket_control_command_t command) {
  DEBUG_TRACE("Handle command in namespace %02x with id %02x",
              command.namespace_id, command.command_id);
  eventlog_socket_control_command_handler_t *handler = NULL;
  void *user_data = NULL;

  pthread_mutex_lock(&g_control_handlers_mutex);
  eventlog_socket_control_command_item_t *entry = g_control_handlers;
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

/// @pre g_control_read_state.tag != tag
static void control_read_enter_state(const control_read_state_tag_t tag,
                                     const uint8_t *data) {
  DEBUG_TRACE("%s -> %s", control_read_state_tag_show(g_control_read_state.tag),
              control_read_state_tag_show(tag));

  // this should only be called when restarting or moving to a different
  // state...
  assert(tag == CONTROL_READ_MAGIC || g_control_read_state.tag != tag);

  // if the parser is leaving CONTROL_READ_NAMESPACE, then...
  if (g_control_read_state.tag == CONTROL_READ_NAMESPACE) {
    // ...free the namespace buffer...
    free(g_control_read_state.namespace_buffer);
  }

  // update the control state tag...
  g_control_read_state.tag = tag;

  // initialise the control state appropriately...
  switch (tag) {
  case CONTROL_READ_MAGIC: {
    // if restarting, handle current_byte...
    if (tag == CONTROL_READ_MAGIC && data != NULL &&
        *data == control_magic[0]) {
      // ...start at the second header byte...
      g_control_read_state.header_pos = 1;
    } else {
      // ...start at the first header byte...
      g_control_read_state.header_pos = 0;
    }
    break;
  }
  case CONTROL_READ_PROTOCOL_VERSION: {
    assert(data == NULL);
    break;
  }
  case CONTROL_READ_NAMESPACE_LEN: {
    assert(data == NULL);
    break;
  }
  case CONTROL_READ_NAMESPACE: {
    assert(data != NULL);
    const size_t namespace_len = *data;
    g_control_read_state.namespace_buffer_len = namespace_len;
    g_control_read_state.namespace_buffer_pos = 0;
    // allocate space for the namespace, with one additional byte to ensure
    // that the string is always null-terminated in memory.
    g_control_read_state.namespace_buffer = malloc(namespace_len + 1);
    g_control_read_state.namespace_buffer[namespace_len] = '\0';
    break;
  }
  case CONTROL_READ_COMMAND_ID: {
    assert(data != NULL);
    g_control_read_state.command_namespace_id = *data;
    break;
  }
  }
}

static void control_handle_command_chunk(const size_t chunk_size,
                                         const uint8_t chunk[chunk_size]) {
  DEBUG_TRACE("Received chunk of size %zd.", chunk_size);
  // iterate over the bytes in the chunk...
  for (size_t chunk_index = 0; chunk_index < chunk_size; ++chunk_index) {
    // get the next byte from the chunk...
    const uint8_t current_byte = chunk[chunk_index];
    switch (g_control_read_state.tag) {
    // the parser is currently reading the header...
    case CONTROL_READ_MAGIC: {
      // invariant: header_pos should be a valid index into control_magic
      assert(0 <= g_control_read_state.header_pos);
      assert(g_control_read_state.header_pos < CONTROL_MAGIC_LEN);
      const uint8_t expected_byte =
          control_magic[g_control_read_state.header_pos];
      // if the next byte is the expected byte...
      if (current_byte == expected_byte) {
        DEBUG_TRACE("Matched control_magic byte %d",
                    g_control_read_state.header_pos);
        // ...move on the the next state...
        ++g_control_read_state.header_pos;
        // if header_pos moves out of control_magic...
        if (g_control_read_state.header_pos >= CONTROL_MAGIC_LEN) {
          // ...continue reading the namespace length...
          control_read_enter_state(CONTROL_READ_PROTOCOL_VERSION, NULL);
        }
        // ...continue processing with the _next_ byte...
        continue;
      }
      // if the next byte is not the expected byte...
      else {
        // ...there has been a protocol error...
        // ...restart with the _current_ byte...
        control_read_enter_state(CONTROL_READ_MAGIC, &current_byte);
        // ...continue processing with the _next_ byte...
        continue;
      }
    }
    // the parser is currently reading the protocol version...
    case CONTROL_READ_PROTOCOL_VERSION: {
      DEBUG_TRACE("Matched protocol version byte %d", current_byte);
      // if the message version matches the protocol version...
      if (current_byte == EVENTLOG_SOCKET_CONTROL_PROTOCOL_VERSION) {
        // ...then we should be able to parse the message...
        // ...continue processing with the _next_ byte...
        control_read_enter_state(CONTROL_READ_NAMESPACE_LEN, NULL);
        continue;
      } else {
        // ...otherwise, let's not try and parse this message...
        // ...restart with the _current_ byte...
        control_read_enter_state(CONTROL_READ_MAGIC, &current_byte);
      }
    }
    // the parser is currently reading the namespace length...
    case CONTROL_READ_NAMESPACE_LEN: {
      // if current_byte == 0, then...
      if (current_byte == 0) {
        // ...there has been a protocol error...
        // todo: enforce this in the register function
        // todo: write an error to the eventlog
        DEBUG_ERROR("%s", "Received namespace length 0");
        // ...restart with the _current_ byte...
        control_read_enter_state(CONTROL_READ_MAGIC, &current_byte);
        continue;
      } else {
        DEBUG_TRACE("Matched namespace_len byte %d", current_byte);
        // otherwise, accept the namespace length and move to the next state...
        control_read_enter_state(CONTROL_READ_NAMESPACE, &current_byte);
        continue;
      }
    }
    case CONTROL_READ_NAMESPACE: {
      // calculate the number of bytes still required for the namespace.
      // note: subtraction is safe due to the invariant on namespace_buffer_pos.
      assert(g_control_read_state.namespace_buffer_len > 0);
      assert(g_control_read_state.namespace_buffer_pos <
             g_control_read_state.namespace_buffer_len);
      const uint8_t required_bytes_for_namespace =
          g_control_read_state.namespace_buffer_len -
          g_control_read_state.namespace_buffer_pos;
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
      void *cpy_dest = g_control_read_state.namespace_buffer +
                       g_control_read_state.namespace_buffer_pos;
      const void *cpy_src = chunk + chunk_index;
      memcpy(cpy_dest, cpy_src, available_bytes);
      // move namespace_buffer_pos.
      g_control_read_state.namespace_buffer_pos += available_bytes;

      // if the namespace is incomplete, then...
      if (g_control_read_state.namespace_buffer_pos <
          g_control_read_state.namespace_buffer_len) {
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
                  g_control_read_state.namespace_buffer);

      // ...try to resolve the namespace...
      uint8_t namespace_id = UCHAR_MAX;
      const bool namespace_id_found = control_namespace_resolve(
          g_control_read_state.namespace_buffer_len,
          g_control_read_state.namespace_buffer, &namespace_id);
      // if the namespace was successfully resolved, then...
      if (namespace_id_found) {
        DEBUG_TRACE("resolved namespace '%s' to %d",
                    g_control_read_state.namespace_buffer, namespace_id);
        // move chunk_index by the number of copied bytes less one,
        // because the chunk_index will be updated when we reenter the for loop.
        // note: the subtraction is safe because available_bytes > 0
        chunk_index += available_bytes - 1;
        // ...move to the next state...
        control_read_enter_state(CONTROL_READ_COMMAND_ID, &namespace_id);
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
                    g_control_read_state.namespace_buffer);
        // ...restart with the _current_ byte...
        control_read_enter_state(CONTROL_READ_MAGIC, &current_byte);
        // ...continue processing with the _next_ byte...
        continue;
      }
    }
    case CONTROL_READ_COMMAND_ID: {
      DEBUG_TRACE("matched command_id byte '%d'", current_byte);
      // Create a command.
      const eventlog_socket_control_command_t command = {
          .namespace_id = g_control_read_state.command_namespace_id,
          .command_id = current_byte,
      };
      // Handle the command.
      control_handle_command(command);
      // ...restart _without_ the current byte...
      control_read_enter_state(CONTROL_READ_MAGIC, NULL);
      // ...continue processing with the _next_ byte...
      continue;
    }
    }
  }
}

static control_fd_status_t control_wait_for_input(void) {
  DEBUG_TRACE("Waiting for input on %d.", g_control_fd);

  // poll for available data:
  // note: POLLHUP and POLLRDHUP are output only and are ignored input.
  struct pollfd pfds[1] = {{
      .fd = g_control_fd,
      .events = POLLIN,
      .revents = 0,
  }};
  const int ready = poll(pfds, 1, POLL_LISTEN_TIMEOUT);

  // if ready is -1, an error occurred...
  if (ready == -1) {
    DEBUG_ERRNO("poll() failed");
    return CONTROL_FD_ERROR;
  }
  // if ready is 0, the call to poll timed out...
  if (ready == 0) {
    DEBUG_TRACE("%s", "poll() timed out");
    return CONTROL_FD_RETRY;
  }
  // otherwise ready is 1, and the file descriptor is ready...
  assert(ready == 1); // poll invariant: ready <= |pfds|
  const int revents = pfds[0].revents;
  // if the POLLIN bit is set, the file descriptor is ready with input...
  if (revents & POLLIN) {
    return CONTROL_FD_READY;
  }
  // if either of the POLLERR, POLLHUP, or POLLNVAL bits are set,
  // the file descriptor is closed...
  // note: in the case of POLLHUP there may still be buffered input,
  //       so this condition should be checked _after_ POLLIN.
  if ((revents & POLLNVAL) || (revents & POLLHUP) || (revents & POLLERR)) {
    return CONTROL_FD_CLOSED;
  }
  // note: the above conditions should cover all possible outputs.
  //       if this isn't the case, the following assertion should trigger.
  assert(false);
  //       if assertions are turned off, treat this as a timeout.
  return CONTROL_FD_RETRY;
}

static control_fd_status_t
control_read_command_chunk(const size_t chunk_size, uint8_t chunk[chunk_size]) {
  DEBUG_TRACE("Receiving input from %d.", g_control_fd);

  // read a chunk from the file descriptor...
  const ssize_t chunk_size_or_error =
      recv(g_control_fd, chunk, CONTROL_READ_COMMAND_CHUNK_SIZE, 0);
  // if num_bytes_or_error == -1, an error occurred...
  if (chunk_size_or_error == -1) {
    // if errno is EINTR, the receive was interrupted...
    if (errno == EINTR) {
      return CONTROL_FD_INTR;
    }
    // if errno is EGAIN or EWOULDBLOCK, recv timed out...
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      DEBUG_TRACE("%s", "recv() timed out or was interrupted.");
      // note: the socket should have SO_RCVTIMEO set.
      return CONTROL_FD_RETRY;
    }
    // if errno is anything else, there is some error...
    DEBUG_ERRNO("recv() failed");
    return CONTROL_FD_ERROR;
  }
  // if num_bytes_or_error == 0, the connection was closed...
  if (chunk_size_or_error == 0) {
    DEBUG_TRACE("%s", "recv() failed: the connection was closed.");
    return CONTROL_FD_CLOSED;
  }
  // otherwise, handle the received chunk...
  DEBUG_TRACE("recv() read %zd bytes", chunk_size_or_error);
  return CONTROL_FD_READY;
}

// HERE BE DRAGONS

typedef enum eventlog_socket_control_status {
  CONTROL_RECV_OK,
  CONTROL_RECV_PROTOCOL_ERROR,
  CONTROL_RECV_DISCONNECTED,
} eventlog_socket_control_status_t;

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
    DEBUG_ERROR("control poll() failed: %s", strerror(errno));
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

static eventlog_socket_control_status_t control_read_exact(int fd, uint8_t *buf,
                                                           size_t len) {
  size_t have = 0;
  while (have < len) {
    ssize_t got = recv(fd, buf + have, len - have, 0);
    DEBUG_TRACE("control_read_exact %zd/%zu", got, len);
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
      DEBUG_ERROR("control recv() failed: %s", strerror(errno));
      return CONTROL_RECV_DISCONNECTED;
    }
    have += (size_t)got;
  }
  return CONTROL_RECV_OK;
}

static eventlog_socket_control_status_t
control_read_command(int fd, eventlog_socket_control_command_t *command_out) {
  DEBUG_TRACE("Listen for command on fd %d", fd);
  uint8_t header[CONTROL_MAGIC_LEN];
  eventlog_socket_control_status_t status =
      control_read_exact(fd, header, CONTROL_MAGIC_LEN);
  if (status != CONTROL_RECV_OK) {
    return status;
  }
  if (memcmp(header, control_magic, CONTROL_MAGIC_LEN) != 0) {
    DEBUG_TRACE("invalid control magic: %02x %02x %02x %02x", header[0],
                header[1], header[2], header[3]);
    return CONTROL_RECV_PROTOCOL_ERROR;
  }
  eventlog_socket_control_namespace_id_t namespace_id;
  status = control_read_exact(fd, &namespace_id, 1);
  if (status != CONTROL_RECV_OK) {
    return status;
  }
  eventlog_socket_control_command_id_t command_id = 0;
  status = control_read_exact(fd, &command_id, 1);
  if (status != CONTROL_RECV_OK) {
    return status;
  }

  *command_out = (eventlog_socket_control_command_t){
      .namespace_id = namespace_id, .command_id = command_id};
  DEBUG_TRACE("control command namespace=0x%08x id=0x%02x", namespace_id,
              command_id);
  return CONTROL_RECV_OK;
}

static void control_reset(const int new_control_fd) {
  DEBUG_TRACE("%s", "Resetting control server state.");
  g_control_fd = new_control_fd;
}

/// @pre must have lock on @link g_control_fd_mutex_ptr
static void control_wait(void) {
  DEBUG_TRACE("%s", "Waiting to be notified of new connection.");
  pthread_cond_wait(g_new_conn_cond_ptr, g_control_fd_mutex_ptr);
}

static void *control_loop(void *arg) {
  (void)arg;

  assert(g_control_fd_ptr != NULL);
  assert(g_control_fd_mutex_ptr != NULL);
  assert(g_new_conn_cond_ptr != NULL);

  // Wait for the GHC RTS to become ready.
  control_wait_ghc_rts_ready();

  while (true) {
    DEBUG_TRACE("%s", "Starting new control iteration.");

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
      control_wait();
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
      control_reset(new_control_fd);
      // ...continue to try to handle a command.
    }

    // If there WAS A connection but there IS NO connection, then...
    else if (g_control_fd != -1 && new_control_fd == -1) {
      DEBUG_TRACE("%s", "There WAS A connection but there IS NO connection.");
      // ...reset the control server state...
      control_reset(new_control_fd);
      // ...wait to be notified of a new connection...
      control_wait();
      // ...release the lock...
      pthread_mutex_unlock(g_control_fd_mutex_ptr);
      // ...and re-enter the loop.
      continue;
    }

    // If there WAS A connection and there IS A DIFFERENT connection, then...
    else if (g_control_fd != -1 && new_control_fd != -1 &&
             g_control_fd != new_control_fd) {
      DEBUG_TRACE(
          "%s", "There WAS A connection and there IS A DIFFERENT connection.");
      // ...DON'T wait to be notified of a new connection...
      // ...we may we have already missed the signal...
      // ...reset the control server state...
      control_reset(new_control_fd);
      // ...continue to try to handle a command.
    }

    // If there WAS A connection and there IS THE SAME connection, then...
    else if (g_control_fd != -1 && new_control_fd != -1 &&
             g_control_fd == new_control_fd) {
      // ...continue to try to handle a command.
      DEBUG_TRACE("%s",
                  "There WAS A connection and there IS THE SAME connection.");
    }

    // These conditions should be covering, so throw an error otherwise.
    else {
      assert(false);
    }

    // Release the lock on the connection file description.
    pthread_mutex_unlock(g_control_fd_mutex_ptr);

    // Check that g_control_fd is up-to-date:
    assert(g_control_fd == new_control_fd);

    // Wait for input:
    const control_fd_status_t wait_fd_status = control_wait_for_input();
    switch (wait_fd_status) {
    case CONTROL_FD_READY:
      break;
    case CONTROL_FD_CLOSED:
    case CONTROL_FD_ERROR:
    case CONTROL_FD_RETRY:
      continue;
    case CONTROL_FD_INTR:
      return NULL;
    }

    // Read a command:
    uint8_t chunk[CONTROL_READ_COMMAND_CHUNK_SIZE] = {0};
    const control_fd_status_t read_fd_status =
        control_read_command_chunk(CONTROL_READ_COMMAND_CHUNK_SIZE, chunk);
    switch (read_fd_status) {
    case CONTROL_FD_READY:
      control_handle_command_chunk(CONTROL_READ_COMMAND_CHUNK_SIZE, chunk);
      break;
    case CONTROL_FD_CLOSED:
    case CONTROL_FD_ERROR:
    case CONTROL_FD_RETRY:
      continue;
    case CONTROL_FD_INTR:
      return NULL;
    }
  }
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
    DEBUG_ERROR("failed to start control receiver: %s",
                strerror(create_or_error));
    return;
  }
  const int detach_or_error = pthread_detach(*control_thread);
  if (detach_or_error != 0) {
    DEBUG_ERROR("failed to detach control receiver: %s",
                strerror(detach_or_error));
  }
}
