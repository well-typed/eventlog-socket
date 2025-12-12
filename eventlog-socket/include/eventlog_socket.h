#ifndef EVENGLOG_SOCKET_H
#define EVENGLOG_SOCKET_H

#include <Rts.h>
#include <rts/EventLogWriter.h>

extern const EventLogWriter SocketEventLogWriter;

void eventlog_socket_init_unix(const char *sock_path);
void eventlog_socket_init_tcp(const char *host, const char *port);
void eventlog_socket_ready(void);

void eventlog_socket_wait(void);

void eventlog_socket_start_unix(const char *sock_path, bool wait);
void eventlog_socket_start_tcp(const char *host, const char *port, bool wait);

int eventlog_socket_hs_main(int argc, char *argv[], RtsConfig conf, StgClosure *main_closure);

#endif
