#include <string.h>

#include "./error.h"
#include "./hooks.h"
#include "eventlog_socket.h"

/******************************************************************************
 * Hook Handler Registry
 ******************************************************************************/

/// @brief An entry in the hook registry.
typedef struct EventlogSocketHookHandlerEntry EventlogSocketHookHandlerEntry;

/// @brief An entry in the handler registry for one particular hook.
struct EventlogSocketHookHandlerEntry {
  /// @brief The user-provided hook handler.
  EventlogSocketHookHandler *const hook_handler;
  /// @brief The user-provided data for the hook handler.
  const void *hook_data;
  /// @brief The pointer to the next entry in the hook registry.
  EventlogSocketHookHandlerEntry *next;
};

/// @brief The registry for all hooks.
typedef struct {
  EventlogSocketHookHandlerEntry *on_connect_handlers;
  EventlogSocketHookHandlerEntry *on_disconnect_handlers;
} EventlogSocketHookHandlerRegistry;

/// @brief The global hook handler registry.
static EventlogSocketHookHandlerRegistry g_hook_handler_registry = {
    .on_connect_handlers = NULL,
    .on_disconnect_handlers = NULL,
};

/// @brief The mutex that guards the global hook handler registry.
static pthread_mutex_t g_hook_handler_registry_mutex =
    PTHREAD_MUTEX_INITIALIZER;

/* HIDDEN - see documentation in hooks.h */
HIDDEN EventlogSocketStatus es_hooks_register_hook(
    const EventlogSocketHook hook,
    EventlogSocketHookHandler *const hook_handler, const void *hook_data) {
  // Validate the arguments.
  if (hook_handler == NULL) {
    errno = EINVAL;
    return STATUS_FROM_ERRNO();
  }

  // Acquire the `g_hook_handler_registry_mutex`.
  RETURN_ON_ERROR(
      STATUS_FROM_PTHREAD(pthread_mutex_lock(&g_hook_handler_registry_mutex)));

  // Get a pointer to the *first* entry for the given hook.
  EventlogSocketHookHandlerEntry **entry = NULL;
  switch (hook) {
  case EVENTLOG_SOCKET_HOOK_POST_START_EVENT_LOGGING:
    entry = &g_hook_handler_registry.on_connect_handlers;
    break;
  case EVENTLOG_SOCKET_HOOK_PRE_END_EVENT_LOGGING:
    entry = &g_hook_handler_registry.on_disconnect_handlers;
    break;
  default:
    errno = EINVAL;
    return STATUS_FROM_ERRNO();
  }
  if (entry == NULL) {
    errno = EINVAL;
    return STATUS_FROM_ERRNO();
  }

  // Get a pointer to the *last* entry for the given hook.
  assert(entry != NULL);
  while (*entry != NULL) {
    // TODO: Check if the hook is already registered?
    entry = &(*entry)->next;
    assert(entry != NULL);
  }

  // Allocate memory and write the new hook handler and data.
  assert(*entry == NULL);
  *entry = malloc(sizeof(EventlogSocketHookHandlerEntry));
  if (*entry == NULL) {
    RETURN_ON_ERROR_CLEANUP(
        STATUS_FROM_ERRNO(), // `malloc` sets errno.
        pthread_mutex_unlock(&g_hook_handler_registry_mutex));
  }
  assert(*entry != NULL);
  const EventlogSocketHookHandlerEntry hook_handler_entry =
      (EventlogSocketHookHandlerEntry){
          .hook_handler = hook_handler,
          .hook_data = hook_data,
          .next = NULL,
      };
  memcpy(*entry, &hook_handler_entry, sizeof(EventlogSocketHookHandlerEntry));

  // Release the `g_hook_handler_registry_mutex`.
  RETURN_ON_ERROR(STATUS_FROM_PTHREAD(
      pthread_mutex_unlock(&g_hook_handler_registry_mutex)));

  // Return OK.
  return STATUS_FROM_CODE(EVENTLOG_SOCKET_OK);
}

/* HIDDEN - see documentation in hooks.h */
HIDDEN EventlogSocketStatus
es_hooks_handle_hook(const EventlogSocketHook hook) {

  // Acquire the `g_hook_handler_registry_mutex`.
  RETURN_ON_ERROR(
      STATUS_FROM_PTHREAD(pthread_mutex_lock(&g_hook_handler_registry_mutex)));

  // Get a pointer to the *first* entry for the given hook.
  EventlogSocketHookHandlerEntry **entry = NULL;
  switch (hook) {
  case EVENTLOG_SOCKET_HOOK_POST_START_EVENT_LOGGING:
    entry = &g_hook_handler_registry.on_connect_handlers;
    break;
  case EVENTLOG_SOCKET_HOOK_PRE_END_EVENT_LOGGING:
    entry = &g_hook_handler_registry.on_disconnect_handlers;
    break;
  default:
    errno = EINVAL;
    return STATUS_FROM_ERRNO();
  }
  if (entry == NULL) {
    errno = EINVAL;
    return STATUS_FROM_ERRNO();
  }

  // Loop through and call each hook handler.
  assert(entry != NULL);
  while (*entry != NULL) {
    EventlogSocketHookHandler *const hook_handler = (*entry)->hook_handler;
    if (hook_handler != NULL) {
      const void *hook_data = (*entry)->hook_data;
      hook_handler(hook_data);
    }
    entry = &(*entry)->next;
    assert(entry != NULL);
  }

  // Release the `g_hook_handler_registry_mutex`.
  RETURN_ON_ERROR(STATUS_FROM_PTHREAD(
      pthread_mutex_unlock(&g_hook_handler_registry_mutex)));

  // Return OK.
  return STATUS_FROM_CODE(EVENTLOG_SOCKET_OK);
}
