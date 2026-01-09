#ifndef EVENTLOG_SOCKET_CONTROL_SERVER_H
#define EVENTLOG_SOCKET_CONTROL_SERVER_H

#include "eventlog_socket.h"
/* eventlog_control_command_id_t */
/* eventlog_control_namespace_id_t */
/* eventlog_control_command_handler_t */

#define CONTROL_BUILTIN_NAMESPACE_ID 0

typedef enum eventlog_control_builtin_namespace_id {
  CONTROL_NAMESPACE_ID_BUILTIN = 0,
} eventlog_control_builtin_namespace_id_t;

typedef enum eventlog_control_builtin_command_id {
  CONTROL_COMMAND_ID_START_HEAP_PROFILING = 0,
  CONTROL_COMMAND_ID_STOP_HEAP_PROFILING = 1,
  CONTROL_COMMAND_ID_REQUEST_HEAP_PROFILE = 2,
} eventlog_control_builtin_command_id_t;

const eventlog_control_command_t CONTROL_COMMAND_START_HEAP_PROFILING = {
    .namespace_id = CONTROL_NAMESPACE_ID_BUILTIN,
    .command_id = CONTROL_COMMAND_ID_START_HEAP_PROFILING,
};
const eventlog_control_command_t CONTROL_COMMAND_STOP_HEAP_PROFILING = {
    .namespace_id = CONTROL_NAMESPACE_ID_BUILTIN,
    .command_id = CONTROL_COMMAND_ID_STOP_HEAP_PROFILING,
};
const eventlog_control_command_t CONTROL_COMMAND_REQUEST_HEAP_PROFILE = {
    .namespace_id = CONTROL_NAMESPACE_ID_BUILTIN,
    .command_id = CONTROL_COMMAND_ID_REQUEST_HEAP_PROFILE};

typedef enum eventlog_control_status {
  CONTROL_RECV_OK,
  CONTROL_RECV_PROTOCOL_ERROR,
  CONTROL_RECV_DISCONNECTED,
} eventlog_control_status_t;

typedef struct eventlog_control_handler_item eventlog_control_handler_item_t;

struct eventlog_control_handler_item {
  eventlog_control_namespace_id_t namespace_id;
  eventlog_control_command_id_t command_id;
  eventlog_control_command_handler_t *handler;
  void *user_data;
  eventlog_control_handler_item_t *next;
};

#endif /* EVENTLOG_SOCKET_CONTROL_SERVER_H */
