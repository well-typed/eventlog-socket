#ifndef EVENTLOG_SOCKET_INIT_STATE_H
#define EVENTLOG_SOCKET_INIT_STATE_H

#include <stdlib.h>

/// @brief Bit flag for `g_init_state` that tracks whether or not the function
/// @c eventlog_socket_init has been called.
#define EVENTLOG_SOCKET_SIG_INITIALIZED 1

/// @brief Bit flag for `g_init_state` that tracks whether or not the function
/// @c EventlogSocketWriter->initEventLogWriter has been called.
#define EVENTLOG_SOCKET_SIG_ATTACHED 2

/// @brief Bit flag for `g_init_state` that tracks whether or not the function
/// @c eventlog_socket_signal_ghc_rts_ready has been called.
#define EVENTLOG_SOCKET_SIG_RTS_READY 4

/// @brief Bit flag for `g_init_state` that tracks whether or not the first
/// client has connected to the eventlog socket. This flag is tracks the first
/// connection and does not reset when the client disconnects.
#define EVENTLOG_SOCKET_SIG_HAD_FIRST_CONNECTION 8

/// @brief The variable that holds the init state flags.
typedef uint8_t EventlogSocketInitState;

#endif /* EVENTLOG_SOCKET_INIT_STATE_H */
