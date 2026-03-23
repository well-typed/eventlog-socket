#ifndef EVENTLOG_SOCKET_CONTROL_H
#define EVENTLOG_SOCKET_CONTROL_H

#include <pthread.h>
#include <stdbool.h>

#include "./init_state.h"
#include "./macros.h"
#include "eventlog_socket.h"

/// @brief The state that is shared with the control thread.
typedef struct {
  /// @brief The function writes the thread handle to this location. It should
  /// be nonnull.
  pthread_t *const control_thread_ptr;
  /// @brief The control thread reads the eventlog socket file descriptor from
  /// this pointer. It should be nonnull.
  const volatile int *const client_fd_ptr;
  /// @brief The control thread uses this mutex to guard its reads of
  /// `client_fd_ptr`. All other accesses of the memory location pointed to by
  /// `client_fd_ptr` should also be guarded using this mutex. It should be
  /// nonnull. This file descriptor is *not* managed by the control thread.
  pthread_mutex_t *const mutex_ptr;
  /// @brief The initialization state. Should be used with the bit flag macros
  /// from @c init_state.h. Should be guarded by @c mutex_ptr. See @c
  /// g_init_state in @c eventlog_socket.c.
  const volatile EventlogSocketInitState *init_state_ptr;
  /// @brief The control thread uses this condition together with `mutex_ptr` to
  /// wait for changes in `client_fd_ptr`. It should be nonnull.
  pthread_cond_t *const restrict new_connection_cond_ptr;
  /// @brief The control thread uses this condition together with `mutex_ptr` to
  /// wait for the GHC RTS to be initialised. It should be nonnull.
  pthread_cond_t *const restrict ghc_rts_ready_cond_ptr;
} ControlState;

/// @brief Start the control thread.
///
/// @param control_thread_state The state that is shared with the control
/// thread.
///
/// @return Upon successful completion, 0 is returned.
///
/// @return On error, On error, -1 is returned, errno is set to indicate the
/// error.
///
/// @par Errors
/// @parblock
/// `EAGAIN`, `EINVAL`, `EPERM`, or `ESRCH`.
/// @endparblock
HIDDEN EventlogSocketStatus es_control_start(ControlState control_thread_state);

/// @see eventlog_socket_control_strnamespace
HIDDEN const char *
es_control_strnamespace(EventlogSocketControlNamespace *namespace);

/// @see eventlog_socket_control_register_namespace
HIDDEN EventlogSocketStatus es_control_register_namespace(
    uint8_t namespace_len, const char namespace[namespace_len],
    EventlogSocketControlNamespace **namespace_out);

/// @see eventlog_socket_control_register_command
HIDDEN EventlogSocketStatus
es_control_register_command(EventlogSocketControlNamespace *namespace,
                            EventlogSocketControlCommandId command_id,
                            EventlogSocketControlCommandHandler command_handler,
                            const void *command_data);

/// @brief Read the current status of the control thread.
HIDDEN EventlogSocketStatus es_control_status(void);

#endif /* EVENTLOG_SOCKET_CONTROL_H */
