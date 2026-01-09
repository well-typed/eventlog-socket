#ifndef EVENTLOG_SOCKET_CONTROL_H
#define EVENTLOG_SOCKET_CONTROL_H

#include <pthread.h>
#include <stdbool.h>

void __attribute__((visibility("hidden"))) eventlog_socket_control_start(void);

void __attribute__((visibility("hidden")))
eventlog_socket_control_create(const volatile int *control_fd_ptr,
                               pthread_mutex_t *control_fd_mutex_ptr);

void __attribute__((visibility("hidden"))) eventlog_socket_control_arm(void);

bool __attribute__((visibility("hidden")))
eventlog_socket_control_is_armed(void);

#endif /* EVENTLOG_SOCKET_CONTROL_H */
