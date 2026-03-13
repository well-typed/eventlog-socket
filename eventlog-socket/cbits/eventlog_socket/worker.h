#ifndef EVENTLOG_SOCKET_WORKER_H
#define EVENTLOG_SOCKET_WORKER_H

#include "./init_state.h"
#include "./macros.h"
#include "./write_buffer.h"
#include "eventlog_socket.h"

/// @brief The state that is shared with the worker thread.
typedef struct {
  /// @brief The function writes the thread handle to this location. It should
  /// be nonnull.
  pthread_t *const worker_thread_ptr;
  /// @brief The worker thread reads the eventlog socket file descriptor from
  /// this pointer. If EVENTLOG_SOCKET_FEATURE_CONTROL is defined, it should be
  /// nonnull.
  volatile int *const client_fd_ptr;
  // TODO: documentation.
  WriteBuffer *write_buffer_ptr;
  /// @brief The worker thread uses this mutex to guard its reads of
  /// `client_fd_ptr`. All other accesses of the memory location pointed to by
  /// `client_fd_ptr` should also be guarded using this mutex. It should be
  /// nonnull. This file descriptor is *not* managed by the worker thread.
  pthread_mutex_t *const mutex_ptr;
  /// @brief The initialization state. Should be used with the bit flag macros
  /// from @c init_state.h. Should be guarded by @c mutex_ptr. See @c
  /// g_init_state in @c eventlog_socket.c.
  volatile EventlogSocketInitState *init_state_ptr;
  /// @brief The worker thread uses this condition together with `mutex_ptr` to
  /// wait for changes in `client_fd_ptr`. It should be nonnull.
  pthread_cond_t *const restrict new_connection_cond_ptr;
  /// @brief The worker thread uses this condition together with `mutex_ptr` to
  /// wait for the GHC RTS to be initialised. It should be nonnull.
  pthread_cond_t *const restrict ghc_rts_ready_cond_ptr;
} WorkerState;

HIDDEN EventlogSocketStatus worker_init(WorkerState worker_state);

HIDDEN EventlogSocketStatus
worker_start(const EventlogSocketAddr *eventlog_socket_addr,
             const EventlogSocketOpts *eventlog_socket_opts);

HIDDEN void worker_wake(void);

#endif /* EVENTLOG_SOCKET_WORKER_H */
