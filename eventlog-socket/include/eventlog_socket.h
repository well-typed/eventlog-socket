#ifndef EVENGLOG_SOCKET_H
#define EVENGLOG_SOCKET_H

#include <stdbool.h>
#include <stdint.h>
#include <Rts.h>
#include <rts/EventLogWriter.h>

extern const EventLogWriter SocketEventLogWriter;

typedef uint8_t control_namespace_t;

typedef void (*eventlog_control_command_handler)(control_namespace_t namespace_id, uint8_t cmd_id, void *user_data);

bool eventlog_socket_register_control_command(control_namespace_t namespace_id,
                                              uint8_t cmd_id,
                                              eventlog_control_command_handler handler,
                                              void *user_data);

void eventlog_socket_init_unix(const char *sock_path);
void eventlog_socket_init_tcp(const char *host, const char *port);
void eventlog_socket_ready(void);

void eventlog_socket_wait(void);

void eventlog_socket_start_unix(const char *sock_path, bool wait);
void eventlog_socket_start_tcp(const char *host, const char *port, bool wait);

int eventlog_socket_hs_main(int argc, char *argv[], RtsConfig conf, StgClosure *main_closure);

#endif
