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

#define CONTROL_MAGIC "GCTL"
#define CONTROL_MAGIC_LEN 4

#define CONTROL_BUILTIN_NAMESPACE_ID 0
#define CONTROL_NAMESPACE_ID_BUILTIN 0
#define CONTROL_COMMAND_ID_START_HEAP_PROFILING 0
#define CONTROL_COMMAND_ID_STOP_HEAP_PROFILING 1
#define CONTROL_COMMAND_ID_REQUEST_HEAP_PROFILE 2

typedef enum eventlog_control_status {
  CONTROL_RECV_OK,
  CONTROL_RECV_PROTOCOL_ERROR,
  CONTROL_RECV_DISCONNECTED,
} eventlog_control_status_t;

typedef struct eventlog_control_handler_item eventlog_control_handler_item_t;

struct eventlog_control_handler_item {
  eventlog_socket_control_namespace_id_t namespace_id;
  eventlog_socket_control_command_id_t command_id;
  eventlog_socket_control_command_handler_t *handler;
  void *user_data;
  eventlog_control_handler_item_t *next;
};

// Whether we have started the control thread
static volatile bool g_control_ready = false;

// Whether the controller thread is ready to start
static bool g_control_ready_armed = false;

static pthread_mutex_t g_control_ready_mutex = PTHREAD_MUTEX_INITIALIZER;

void __attribute__((visibility("hidden"))) eventlog_socket_control_arm(void) {
  pthread_mutex_lock(&g_control_ready_mutex);
  g_control_ready = false;
  g_control_ready_armed = true;
  pthread_mutex_unlock(&g_control_ready_mutex);
}

bool __attribute__((visibility("hidden")))
eventlog_socket_control_is_armed(void) {
  bool armed = false;
  pthread_mutex_lock(&g_control_ready_mutex);
  if (g_control_ready_armed) {
    g_control_ready_armed = false;
    armed = true;
  }
  pthread_mutex_unlock(&g_control_ready_mutex);
  return armed;
}

// static int (*g_read_control_fd_fn)(void) = NULL;
static const volatile int *g_control_fd_ptr = NULL;

static pthread_mutex_t *g_control_fd_mutex_ptr = NULL;

// Signal to the control thread to start.
static pthread_cond_t g_control_ready_cond = PTHREAD_COND_INITIALIZER;

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
static eventlog_control_handler_item_t *g_control_handlers =
    &(eventlog_control_handler_item_t){
        .namespace_id = CONTROL_BUILTIN_NAMESPACE_ID,
        .command_id = CONTROL_COMMAND_ID_START_HEAP_PROFILING,
        .handler = control_start_heap_profiling,
        .user_data = NULL,
        .next = &(eventlog_control_handler_item_t){
            .namespace_id = CONTROL_BUILTIN_NAMESPACE_ID,
            .command_id = CONTROL_COMMAND_ID_STOP_HEAP_PROFILING,
            .handler = control_stop_heap_profiling,
            .user_data = NULL,
            .next = &(eventlog_control_handler_item_t){
                .namespace_id = CONTROL_BUILTIN_NAMESPACE_ID,
                .command_id = CONTROL_COMMAND_ID_REQUEST_HEAP_PROFILE,
                .handler = control_request_heap_profile,
                .user_data = NULL,
                .next = NULL,
            }}};

static pthread_mutex_t g_control_handlers_mutex = PTHREAD_MUTEX_INITIALIZER;

void __attribute__((visibility("hidden"))) eventlog_socket_control_start(void) {
  pthread_mutex_lock(&g_control_ready_mutex);
  if (!g_control_ready) {
    g_control_ready = true;
    pthread_cond_broadcast(&g_control_ready_cond);
  }
  pthread_mutex_unlock(&g_control_ready_mutex);
}

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

static eventlog_control_status_t control_read_exact(int fd, uint8_t *buf,
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

bool eventlog_socket_control_register_command(
    eventlog_socket_control_command_t command,
    eventlog_socket_control_command_handler_t handler, void *user_data) {
  if (handler == NULL) {
    return false;
  }

  bool success = false;
  pthread_mutex_lock(&g_control_handlers_mutex);
  eventlog_control_handler_item_t *entry = g_control_handlers;
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

  entry = malloc(sizeof(eventlog_control_handler_item_t));
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

static eventlog_control_status_t
control_receive_command(int fd,
                        eventlog_socket_control_command_t *command_out) {
  uint8_t header[CONTROL_MAGIC_LEN];
  eventlog_control_status_t status =
      control_read_exact(fd, header, CONTROL_MAGIC_LEN);
  if (status != CONTROL_RECV_OK) {
    return status;
  }
  if (memcmp(header, CONTROL_MAGIC, CONTROL_MAGIC_LEN) != 0) {
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
  eventlog_socket_control_command_handler_t *handler = NULL;
  void *user_data = NULL;

  pthread_mutex_lock(&g_control_handlers_mutex);
  eventlog_control_handler_item_t *entry = g_control_handlers;
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

static void *control_receiver(void *arg) {
  (void)arg;

  pthread_mutex_lock(&g_control_ready_mutex);
  while (!g_control_ready) {
    pthread_cond_wait(&g_control_ready_cond, &g_control_ready_mutex);
  }
  pthread_mutex_unlock(&g_control_ready_mutex);

  while (true) {
    // Grab consistent snapshot of client_fd; connection teardown may happen
    // at any time so we must hold the mutex while comparing.
    assert(g_control_fd_ptr != NULL);
    assert(g_control_fd_mutex_ptr != NULL);
    pthread_mutex_lock(g_control_fd_mutex_ptr);
    const int control_fd = *g_control_fd_ptr;
    pthread_mutex_unlock(g_control_fd_mutex_ptr);
    if (control_fd == -1) {
      break;
    }

    eventlog_socket_control_command_t command = {0};
    eventlog_control_status_t status =
        control_receive_command(control_fd, &command);
    if (status == CONTROL_RECV_DISCONNECTED) {
      break;
    } else if (status == CONTROL_RECV_PROTOCOL_ERROR) {
      // Ignore protocol errors: they indicate garbage control traffic but
      // shouldn't drop the data writer. Keep listening for valid commands.
      continue;
    }

    handle_control_command(command);
  }

  return NULL;
}

void __attribute__((visibility("hidden")))
eventlog_socket_control_create(const volatile int *const control_fd_ptr,
                               pthread_mutex_t *control_fd_mutex_ptr) {
  g_control_fd_ptr = control_fd_ptr;
  g_control_fd_mutex_ptr = control_fd_mutex_ptr;
  pthread_t tid;
  int ret = pthread_create(&tid, NULL, control_receiver, NULL);
  if (ret != 0) {
    DEBUG_ERROR("failed to start control receiver: %s", strerror(ret));
    return;
  }
  ret = pthread_detach(tid);
  if (ret != 0) {
    DEBUG_ERROR("failed to detach control receiver: %s", strerror(ret));
  }
}
