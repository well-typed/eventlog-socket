/// @file control.c
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

typedef struct eventlog_socket_control_namespace_entry
    eventlog_socket_control_namespace_entry_t;

struct eventlog_socket_control_namespace_entry {
  const char *namespace;
  const size_t namespace_len;
  const eventlog_socket_control_namespace_id_t namespace_id;
  eventlog_socket_control_namespace_entry_t *next;
};

eventlog_socket_control_namespace_entry_t
    g_eventlog_socket_control_namespace_list = {
        .namespace = BUILTIN_NAMESPACE,
        .namespace_len = strlen(BUILTIN_NAMESPACE),
        .next = NULL,
};

bool eventlog_socket_control_register_namespace(
    const size_t namespace_len, const char namespace[namespace_len + 1],
    eventlog_socket_control_namespace_id_t *namespace_id_out) {

  // Initialise the namespace_entry pointer.
  eventlog_socket_control_namespace_entry_t *namespace_entry =
      &g_eventlog_socket_control_namespace_list;

  // Let's start counting namespace ids.
  eventlog_socket_control_namespace_id_t namespace_id = 0;

  // Is the requested namespace already registered?
  do {
    // Does the requested namespace match the current namespace_entry?
    if (namespace_entry->namespace_len == namespace_len) {
      const int cmp =
          strncmp(namespace_entry->namespace, namespace, namespace_len);
      if (cmp == 0) {
        // Return the associated namespace_id.
        return namespace_id;
      }
    }
    // Continue with the next namespace_entry.
    if (namespace_entry->next != NULL) {
      namespace_entry = namespace_entry->next;
      ++namespace_id;
    } else {
      break;
    }
  } while (true);

  // Register the requested namespace.
  assert(namespace_entry != NULL);
  assert(namespace_entry->next == NULL);
  char *namespace_cpy = malloc(namespace_len);
  strncpy(namespace_cpy, namespace, namespace_len);
  namespace_entry->next = &(eventlog_socket_control_namespace_entry_t){
      .namespace = namespace_cpy,
      .namespace_len = namespace_len,
      .next = NULL,
  };
  ++namespace_id;
  DEBUG_TRACE("Registered namespace '%s' under ID %d", namespace_cpy,
              namespace_id);

  // Write out the new namespace_id.
  *namespace_id_out = namespace_id;

  // Return success.
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
  DEBUG_TRACE("control: startHeapProfiling");
  startHeapProfTimer();
}

// Stop heap profiling
static void
control_stop_heap_profiling(eventlog_socket_control_command_t command,
                            void *user_data) {
  (void)command;
  (void)user_data;
  DEBUG_TRACE("control: stopHeapProfiling");
  stopHeapProfTimer();
}

// Request heap profil
static void
control_request_heap_profile(eventlog_socket_control_command_t command,
                             void *user_data) {
  (void)command;
  (void)user_data;
  DEBUG_TRACE("control: requestHeapProfile");
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
    DEBUG_ERROR("control: failed to allocate handler entry");
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

typedef enum eventlog_socket_control_status {
  CONTROL_RECV_OK,
  CONTROL_RECV_PROTOCOL_ERROR,
  CONTROL_RECV_DISCONNECTED,
} eventlog_socket_control_status_t;

static volatile bool g_ghc_rts_ready = false;
static pthread_mutex_t g_ghc_rts_ready_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_ghc_rts_ready_cond = PTHREAD_COND_INITIALIZER;

void HIDDEN eventlog_socket_control_signal_rts_ready(void) {
  DEBUG_TRACE("Sending signal that GHC RTS is ready.");
  pthread_mutex_lock(&g_ghc_rts_ready_mutex);
  if (!g_ghc_rts_ready) {
    g_ghc_rts_ready = true;
    pthread_cond_broadcast(&g_ghc_rts_ready_cond);
  }
  pthread_mutex_unlock(&g_ghc_rts_ready_mutex);
}

static void eventlog_socket_control_wait_rts_ready(void) {
  DEBUG_TRACE("Waiting for signal that GHC RTS is ready.");
  pthread_mutex_lock(&g_ghc_rts_ready_mutex);
  while (!g_ghc_rts_ready) {
    pthread_cond_wait(&g_ghc_rts_ready_cond, &g_ghc_rts_ready_mutex);
  }
  pthread_mutex_unlock(&g_ghc_rts_ready_mutex);
}

static int g_control_fd = -1;
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
control_receive_command(int fd,
                        eventlog_socket_control_command_t *command_out) {
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

static void handle_control_command(eventlog_socket_control_command_t command) {
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
  } else {
    DEBUG_ERROR("control: unhandled command namespace=0x%08x id=0x%02x",
                command.namespace_id, command.command_id);
  }
}

static void control_reset(const int new_control_fd) {
  DEBUG_TRACE("Resetting control server state.");
  g_control_fd = new_control_fd;
}

/// @pre must have lock on @link g_control_fd_mutex_ptr
static void control_wait(void) {
  DEBUG_TRACE("Waiting to be notified of new connection.");
  pthread_cond_wait(g_new_conn_cond_ptr, g_control_fd_mutex_ptr);
}

static void *control_receiver(void *arg) {
  (void)arg;

  assert(g_control_fd_ptr != NULL);
  assert(g_control_fd_mutex_ptr != NULL);
  assert(g_new_conn_cond_ptr != NULL);

  // Wait for the GHC RTS to become ready.
  eventlog_socket_control_wait_rts_ready();

  while (true) {
    DEBUG_TRACE("Starting new control iteration.");

    // Acquire the lock on the connection file description.
    DEBUG_TRACE("Acquire lock on connection.");
    pthread_mutex_lock(g_control_fd_mutex_ptr);

    // Read current connection file description.
    const int new_control_fd = *g_control_fd_ptr;
    DEBUG_TRACE("Old connection fd: %d", g_control_fd);
    DEBUG_TRACE("New connection fd: %d", new_control_fd);

    // If there WAS NO connection and there IS NO connection, then...
    if (g_control_fd == -1 && new_control_fd == -1) {
      // ...wait to be notified of a new connection...
      control_wait();
      // ...release the lock...
      pthread_mutex_unlock(g_control_fd_mutex_ptr);
      // ...and re-enter the loop.
      continue;
    }

    // If there WAS NO connection and there IS A connection, then...
    else if (g_control_fd == -1 && new_control_fd != -1) {
      // ...DON'T wait to be notified of a new connection...
      // ...we may we have already missed the signal...
      // ...reset the control server state...
      control_reset(new_control_fd);
      // ...continue to try to handle a command.
    }

    // If there WAS A connection and there IS NO connection, then...
    else if (g_control_fd != -1 && new_control_fd == -1) {
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
    }

    // These conditions should be covering, so throw an error otherwise.
    else {
      assert(false);
    }

    // Release the lock on the connection file description.
    pthread_mutex_unlock(g_control_fd_mutex_ptr);

    // Check that g_control_fd is up-to-date:
    assert(g_control_fd == new_control_fd);

    // Read a command:
    eventlog_socket_control_command_t command = {0};
    eventlog_socket_control_status_t status =
        control_receive_command(g_control_fd, &command);
    if (status == CONTROL_RECV_DISCONNECTED) {
      continue;
    } else if (status == CONTROL_RECV_PROTOCOL_ERROR) {
      // Ignore protocol errors: they indicate garbage control traffic but
      // shouldn't drop the data writer. Keep listening for valid commands.
      continue;
    }

    handle_control_command(command);
  }
  return NULL;
}

void HIDDEN eventlog_socket_control_start(
    pthread_t *control_thread, const volatile int *const control_fd_ptr,
    pthread_mutex_t *control_fd_mutex_ptr, pthread_cond_t *new_conn_cond_ptr) {
  DEBUG_TRACE("Starting control thread.");
  g_control_fd_ptr = control_fd_ptr;
  g_control_fd_mutex_ptr = control_fd_mutex_ptr;
  g_new_conn_cond_ptr = new_conn_cond_ptr;
  const int create_or_error =
      pthread_create(control_thread, NULL, control_receiver, NULL);
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
