#ifndef EVENGLOG_SOCKET_H
#define EVENGLOG_SOCKET_H
#include <rts/EventLogWriter.h>

static const EventLogWriter SocketEventLogWriter;

void eventlog_socket_init(const char *sock_path);

void eventlog_socket_wait(void);

void eventlog_socket_start(const char *sock_path, bool wait);

#endif
