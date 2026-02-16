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

typedef enum EventlogSocketTag {
  EVENTLOG_SOCKET_UNIX,
  EVENTLOG_SOCKET_INET,
} EventlogSocketTag;

typedef struct EventlogSocketUnixAddr {
  /// The path to the Unix domain socket.
  char *esa_unix_path;
} EventlogSocketUnixAddr;

typedef struct EventlogSocketInetAddr {
  /// The host name.
  char *esa_inet_host;
  /// The port number.
  char *esa_inet_port;
} EventlogSocketInetAddr;

typedef struct EventlogSocketOpts {
  bool eso_wait;
  int eso_sndbuf;
} EventlogSocketOpts;

typedef struct EventlogSocketAddr {
  EventlogSocketTag esa_tag;
  union {
    /// The address for an `EVENTLOG_SOCKET_UNIX` socket.
    EventlogSocketUnixAddr esa_unix_addr;
    /// The address for an `EVENTLOG_SOCKET_INET` socket.
    EventlogSocketInetAddr esa_inet_addr;
  };
} EventlogSocketAddr;

// note: does not free `EventlogSocketAddr` itself
void eventlog_socket_addr_free(EventlogSocketAddr *eventlog_socket);

// writes default options
void eventlog_socket_opts_init(EventlogSocketOpts *eventlog_socket_opts);

// note: does not free `eventlog_socket_opts_free` itself
void eventlog_socket_opts_free(EventlogSocketOpts *opts);

extern const EventLogWriter SocketEventLogWriter;

typedef enum EventlogSocketFromEnvStatus {
  EVENTLOG_SOCKET_FROM_ENV_OK,
  EVENTLOG_SOCKET_FROM_ENV_NOTFOUND,
  EVENTLOG_SOCKET_FROM_ENV_TOOLONG,
  EVENTLOG_SOCKET_FROM_ENV_INVAL,
} EventlogSocketFromEnvStatus;

/// @par MT-Unsafe
EventlogSocketFromEnvStatus
eventlog_socket_addr_from_env(EventlogSocketAddr *eventlog_socket_out,
                              EventlogSocketOpts *eventlog_socket_opts_out);

// Use this when you install SocketEventLogWriter via RtsConfig before hs_main.
// It spawns the worker immediately but defers handling of control messages
// until eventlog_socket_ready() is invoked after RTS initialization.
void eventlog_socket_init(const EventlogSocketAddr *eventlog_socket,
                          const EventlogSocketOpts *opts);

void eventlog_socket_init_unix(char *unix_path);

void eventlog_socket_init_inet(char *inet_host, char *inet_port);

/// @pre The GHC RTS is ready.
/// @pre The function `eventlog_socket_init` has been called.
void eventlog_socket_attach(void);

// Use this from an already-running RTS: it reconfigures eventlogging to use
// SocketEventLogWriter and restarts the log when a client connects.
void eventlog_socket_start(const EventlogSocketAddr *eventlog_socket,
                           const EventlogSocketOpts *opts);

void eventlog_socket_start_unix(char *unix_path);

void eventlog_socket_start_inet(char *inet_host, char *inet_port);

void eventlog_socket_wait(void);

int eventlog_socket_wrap_hs_main(int argc, char *argv[], RtsConfig rts_config,
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
typedef struct EventlogSocketControlNamespace EventlogSocketControlNamespace;

/// A control command ID.
///
/// Individual commands are identified by numbers (0-255).
typedef uint8_t EventlogSocketControlCommandId;

/// A control command handler.
///
/// This function is called when the corresponding control command message is
/// received on the eventlog socket. The `command_data` parameter will contain
/// the pointer provided to `eventlog_socket_control_register_command`.
typedef void EventlogSocketControlCommandHandler(
    const EventlogSocketControlNamespace *const namespace,
    const EventlogSocketControlCommandId command_id, const void *command_data);

/// Register a new namespace.
///
/// @return If there is no existing namespace with the given name, this function
/// registers a new namespace and returns a stable pointer to it. Otherwise, it
/// returns NULL. The returned pointer should not be freed.
EventlogSocketControlNamespace *
eventlog_socket_control_register_namespace(uint8_t namespace_len,
                                           const char namespace[namespace_len]);

/// Register a new command.
///
/// @return If there is no existing command in the given namespace with the
/// given ID, this function registers a new command and returns true. Otherwise,
/// it returns false.
bool eventlog_socket_control_register_command(
    EventlogSocketControlNamespace *namespace,
    EventlogSocketControlCommandId command_id,
    EventlogSocketControlCommandHandler command_handler,
    const void *command_data);

#endif /* EVENGLOG_SOCKET_H */
