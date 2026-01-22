/// @file eventlog_socket.h
#ifndef EVENGLOG_SOCKET_H
#define EVENGLOG_SOCKET_H

#include <Rts.h>
#include <rts/EventLogWriter.h>
#include <stdbool.h>
#include <stdint.h>

/******************************************************************************
 * Eventlog Writer
 ******************************************************************************/

extern const EventLogWriter SocketEventLogWriter;

void eventlog_socket_init_unix(const char *sock_path);
void eventlog_socket_init_tcp(const char *host, const char *port);
int eventlog_socket_init_from_env(void);
void eventlog_socket_wait(void);
void eventlog_socket_start_unix(const char *sock_path, bool wait);
void eventlog_socket_start_tcp(const char *host, const char *port, bool wait);
int eventlog_socket_hs_main(int argc, char *argv[], RtsConfig conf,
                            StgClosure *main_closure);

/******************************************************************************
 * Control Commands
 ******************************************************************************/

/// The version of the eventlog-socket control command protocol implemented by
/// this version of the library.
#define EVENTLOG_SOCKET_CONTROL_PROTOCOL_VERSION 0

/// A control command namespace.
///
/// See `eventlog_socket_control_register_namespace`.
typedef struct eventlog_socket_control_namespace
    eventlog_socket_control_namespace_t;

/// A control command ID.
///
/// Individual commands are identified by numbers (0-255).
typedef uint8_t eventlog_socket_control_command_id_t;

/// A control command handler.
///
/// This function is called when the corresponding control command message is
/// received on the eventlog socket. The `command_data` parameter will contain
/// the pointer provided to `eventlog_socket_control_register_command`.
typedef void eventlog_socket_control_command_handler_t(
    const eventlog_socket_control_namespace_t *const namespace,
    const eventlog_socket_control_command_id_t command_id,
    const void *command_data);

/// Register a new namespace.
///
/// @return If there is no existing namespace with the given name, this function
/// registers a new namespace and returns a stable pointer to it. Otherwise, it
/// returns NULL. The returned pointer should not be freed.
eventlog_socket_control_namespace_t *
eventlog_socket_control_register_namespace(uint8_t namespace_len,
                                           const char namespace[namespace_len]);

/// Register a new command.
///
/// @return If there is no existing command in the given namespace with the
/// given ID, this function registers a new command and returns true. Otherwise,
/// it returns false.
bool eventlog_socket_control_register_command(
    eventlog_socket_control_namespace_t *namespace,
    eventlog_socket_control_command_id_t command_id,
    eventlog_socket_control_command_handler_t command_handler,
    const void *command_data);

/// Signal that the GHC RTS is ready.
///
/// Since control command handlers may call function from the GHC RTS API, no
/// control commands are executed until the GHC RTS is ready. You do not need to
/// call this function if you're starting eventlog socket using the Haskell API
/// or via `eventlog_socket_start`.
///
/// @pre The GHC RTS is ready.
void eventlog_socket_control_signal_ghc_rts_ready(void);

#endif /* EVENGLOG_SOCKET_H */
