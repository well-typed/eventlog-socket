/// @file eventlog_socket.h
#ifndef EVENGLOG_SOCKET_H
#define EVENGLOG_SOCKET_H

#include <Rts.h>
#include <rts/EventLogWriter.h>
#include <stdbool.h>
#include <stdint.h>

#define EVENTLOG_SOCKET_CONTROL_PROTOCOL_VERSION 0

extern const EventLogWriter SocketEventLogWriter;

typedef struct eventlog_socket_control_namespace
    eventlog_socket_control_namespace_t;

typedef uint8_t eventlog_socket_control_command_id_t;

typedef void eventlog_socket_control_command_handler_t(
    const eventlog_socket_control_namespace_t *const namespace,
    const eventlog_socket_control_command_id_t command_id,
    const void *user_data);

const eventlog_socket_control_namespace_t *
eventlog_socket_control_register_namespace(uint8_t namespace_len,
                                           const char namespace[namespace_len]);

bool eventlog_socket_control_register_command(
    const eventlog_socket_control_namespace_t *namespace,
    eventlog_socket_control_command_id_t command_id,
    eventlog_socket_control_command_handler_t *handler, const void *user_data);

void eventlog_socket_init_unix(const char *sock_path);
void eventlog_socket_init_tcp(const char *host, const char *port);
int eventlog_socket_init_from_env(void);
void eventlog_socket_signal_rts_ready(void);

void eventlog_socket_wait(void);

void eventlog_socket_start_unix(const char *sock_path, bool wait);
void eventlog_socket_start_tcp(const char *host, const char *port, bool wait);

int eventlog_socket_hs_main(int argc, char *argv[], RtsConfig conf,
                            StgClosure *main_closure);

#endif /* EVENGLOG_SOCKET_H */
