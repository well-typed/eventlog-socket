/// @file      hsapi.c
/// @brief     Helper functions for the @c GHC.Eventlog.Socket module.
/// @details   This module defines wrappers for some of the functions
///            exported by @c eventlog_socket.h for use in the Haskell
///            module @c GHC.Eventlog.Socket.
/// @author    Wen Kokke
/// @version   0.1.3.0
/// @date      2025-2026
/// @copyright BSD-3-Clause License.
///

#include "./hsapi.h"
#include <string.h>

HIDDEN void es_hsapi_start(EventlogSocketStatus *eventlog_socket_status,
                           EventlogSocketAddr *eventlog_socket_addr,
                           EventlogSocketOpts *eventlog_socket_opts) {
  const EventlogSocketStatus status =
      eventlog_socket_start(eventlog_socket_addr, eventlog_socket_opts);
  memcpy(eventlog_socket_status, &status, sizeof(EventlogSocketStatus));
}

HIDDEN void es_hsapi_from_env(EventlogSocketStatus *eventlog_socket_status,
                              EventlogSocketAddr *eventlog_socket_addr,
                              EventlogSocketOpts *eventlog_socket_opts) {
  const EventlogSocketStatus status =
      eventlog_socket_from_env(eventlog_socket_addr, eventlog_socket_opts);
  memcpy(eventlog_socket_status, &status, sizeof(EventlogSocketStatus));
}

HIDDEN char *es_hsapi_strerror(EventlogSocketStatus *eventlog_socket_status) {
  return eventlog_socket_strerror(*eventlog_socket_status);
}

HIDDEN void
es_hsapi_register_hook(EventlogSocketStatus *eventlog_socket_status,
                       EventlogSocketHook eventlog_socket_hook,
                       EventlogSocketHookHandler eventlog_socket_hook_handler,
                       const void *eventlog_socket_hook_data) {
  const EventlogSocketStatus status = eventlog_socket_register_hook(
      eventlog_socket_hook, eventlog_socket_hook_handler,
      eventlog_socket_hook_data);
  memcpy(eventlog_socket_status, &status, sizeof(EventlogSocketStatus));
}

HIDDEN void
es_hsapi_worker_status(EventlogSocketStatus *eventlog_socket_status_out) {
  const EventlogSocketStatus eventlog_socket_status =
      eventlog_socket_worker_status();
  memcpy(eventlog_socket_status_out, &eventlog_socket_status,
         sizeof(EventlogSocketStatus));
}

HIDDEN void
es_hsapi_control_status(EventlogSocketStatus *eventlog_socket_status_out) {
  const EventlogSocketStatus eventlog_socket_status =
      eventlog_socket_control_status();
  memcpy(eventlog_socket_status_out, &eventlog_socket_status,
         sizeof(EventlogSocketStatus));
}

HIDDEN void es_hsapi_control_register_namespace(
    EventlogSocketStatus *eventlog_socket_status,
    uint8_t eventlog_socket_namespace_len,
    char eventlog_socket_namespace[eventlog_socket_namespace_len],
    EventlogSocketControlNamespace **eventlog_socket_namespace_out) {
  const EventlogSocketStatus status =
      eventlog_socket_control_register_namespace(eventlog_socket_namespace_len,
                                                 eventlog_socket_namespace,
                                                 eventlog_socket_namespace_out);
  memcpy(eventlog_socket_status, &status, sizeof(EventlogSocketStatus));
}

HIDDEN void es_hsapi_control_register_command(
    EventlogSocketStatus *eventlog_socket_status,
    EventlogSocketControlNamespace *eventlog_socket_namespace,
    EventlogSocketControlCommandId eventlog_socket_command_id,
    EventlogSocketControlCommandHandler eventlog_socket_command_handler,
    const void *eventlog_socket_command_data) {
  const EventlogSocketStatus status = eventlog_socket_control_register_command(
      eventlog_socket_namespace, eventlog_socket_command_id,
      eventlog_socket_command_handler, eventlog_socket_command_data);
  memcpy(eventlog_socket_status, &status, sizeof(EventlogSocketStatus));
}
