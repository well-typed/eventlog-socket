/// @file      hsapi.h
/// @brief     Helper functions for the @c GHC.Eventlog.Socket module.
/// @details   This module declares wrappers for some of the functions
///            exported by @c eventlog_socket.h for use in the Haskell
///            module @c GHC.Eventlog.Socket.
/// @author    Wen Kokke
/// @version   0.1.2.1
/// @date      2025-2026
/// @copyright BSD-3-Clause License.
///

#include "./macros.h"
#include <eventlog_socket.h>

HIDDEN void es_hsapi_start(EventlogSocketStatus *eventlog_socket_status,
                           EventlogSocketAddr *eventlog_socket_addr,
                           EventlogSocketOpts *eventlog_socket_opts);

HIDDEN void es_hsapi_from_env(EventlogSocketStatus *eventlog_socket_status,
                              EventlogSocketAddr *eventlog_socket_addr,
                              EventlogSocketOpts *eventlog_socket_opts);

HIDDEN char *es_hsapi_strerror(EventlogSocketStatus *eventlog_socket_status);

HIDDEN void
es_hsapi_register_hook(EventlogSocketStatus *eventlog_socket_status,
                       EventlogSocketHook eventlog_socket_hook,
                       EventlogSocketHookHandler eventlog_socket_hook_handler,
                       const void *eventlog_socket_hook_data);

HIDDEN void
es_hsapi_worker_status(EventlogSocketStatus *eventlog_socket_status_out);

HIDDEN void
es_hsapi_control_status(EventlogSocketStatus *eventlog_socket_status_out);

HIDDEN void es_hsapi_control_register_namespace(
    EventlogSocketStatus *eventlog_socket_status,
    uint8_t eventlog_socket_namespace_len,
    char eventlog_socket_namespace[eventlog_socket_namespace_len],
    EventlogSocketControlNamespace **eventlog_socket_namespace_out);

HIDDEN void es_hsapi_control_register_command(
    EventlogSocketStatus *eventlog_socket_status,
    EventlogSocketControlNamespace *eventlog_socket_namespace,
    EventlogSocketControlCommandId eventlog_socket_command_id,
    EventlogSocketControlCommandHandler eventlog_socket_command_handler,
    const void *eventlog_socket_command_data);
