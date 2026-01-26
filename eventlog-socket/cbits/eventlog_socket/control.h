#ifndef EVENTLOG_SOCKET_CONTROL_H
#define EVENTLOG_SOCKET_CONTROL_H

#include "./macros.h"

#include <pthread.h>
#include <stdbool.h>

/// @brief Start the control thread.
///
/// @param control_thread
///   The function writes the thread handle to this location.
/// @param control_fd_ptr
///   The control thread reads the eventlog socket file descriptor from this
///   pointer. It should be nonnull.
/// @param control_fd_mutex_ptr
///   The control thread uses this mutex to guard its reads of `control_fd_ptr`.
///   All other accesses of the memory location pointed to by `control_fd_ptr`
///   should also be guarded using this mutex. Should be nonnull.
/// @param new_conn_cond_ptr
///   The control thread uses this condition together with
///   `control_fd_mutex_ptr` to wait for changes in `control_fd_ptr`.
void HIDDEN eventlog_socket_control_start(pthread_t *control_thread,
                                          const volatile int *control_fd_ptr,
                                          pthread_mutex_t *control_fd_mutex_ptr,
                                          pthread_cond_t *new_conn_cond_ptr);

/// Signal that the GHC RTS is ready.
///
/// Since control command handlers may call function from the GHC RTS API, no
/// control commands are executed until the GHC RTS is ready. You do not need to
/// call this function if you're starting eventlog socket using the Haskell API
/// or via `eventlog_socket_start`.
///
/// @pre The GHC RTS is ready.
void HIDDEN control_signal_ghc_rts_ready(void);

#endif /* EVENTLOG_SOCKET_CONTROL_H */
