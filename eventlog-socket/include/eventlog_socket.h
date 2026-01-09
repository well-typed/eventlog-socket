#ifndef EVENGLOG_SOCKET_H
#define EVENGLOG_SOCKET_H

#include <Rts.h>
#include <rts/EventLogWriter.h>
#include <stdbool.h>
#include <stdint.h>

extern const EventLogWriter SocketEventLogWriter;

typedef uint8_t eventlog_socket_control_namespace_id_t;

typedef uint8_t eventlog_socket_control_command_id_t;

typedef struct {
  eventlog_socket_control_namespace_id_t namespace_id;
  eventlog_socket_control_command_id_t command_id;
} eventlog_socket_control_command_t;

typedef void eventlog_socket_control_command_handler_t(
    const eventlog_socket_control_command_t command, void *user_data);

bool eventlog_socket_control_register_command(
    eventlog_socket_control_command_t command,
    eventlog_socket_control_command_handler_t *handler, void *user_data);

void eventlog_socket_init_unix(const char *sock_path);
void eventlog_socket_init_tcp(const char *host, const char *port);
void eventlog_socket_ready(void);

void eventlog_socket_wait(void);

void eventlog_socket_start_unix(const char *sock_path, bool wait);
void eventlog_socket_start_tcp(const char *host, const char *port, bool wait);

int eventlog_socket_hs_main(int argc, char *argv[], RtsConfig conf,
                            StgClosure *main_closure);

#endif /* EVENGLOG_SOCKET_H */
