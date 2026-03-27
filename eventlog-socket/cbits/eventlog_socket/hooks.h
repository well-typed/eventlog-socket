#ifndef EVENTLOG_SOCKET_HOOKS_H
#define EVENTLOG_SOCKET_HOOKS_H

#include "./macros.h"
#include "eventlog_socket.h"

/// @brief Register a new hook handler.
///
/// @see eventlog_socket_register_hook
HIDDEN EventlogSocketStatus es_hooks_register_hook(
    EventlogSocketHook hook, EventlogSocketHookHandler *hook_handler,
    const void *hook_data);

/// @brief Call all handlers for a given `EventlogSocketHook`.
HIDDEN EventlogSocketStatus es_hooks_handle_hook(EventlogSocketHook hook);

#endif /* EVENTLOG_SOCKET_HOOKS_H */
