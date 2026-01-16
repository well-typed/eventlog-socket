#ifndef EVENTLOG_SOCKET_CONTROL_H
#define EVENTLOG_SOCKET_CONTROL_H

#include "./macros.h"

#include <pthread.h>
#include <stdbool.h>

void HIDDEN eventlog_socket_control_create(
    const volatile int *control_fd_ptr, pthread_mutex_t *control_fd_mutex_ptr);

void HIDDEN eventlog_socket_control_signal_rts_ready(void);

#endif /* EVENTLOG_SOCKET_CONTROL_H */
