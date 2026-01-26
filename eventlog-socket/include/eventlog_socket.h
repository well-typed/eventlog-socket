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

typedef enum {
  EVENTLOG_SOCKET_UNIX,
  EVENTLOG_SOCKET_INET,
} eventlog_socket_tag_t;

typedef struct eventlog_socket_unix_addr {
  /// The path to the Unix domain socket.
  char *unix_path;
} eventlog_socket_unix_addr_t;

typedef struct eventlog_socket_inet_addr {
  /// The host name.
  char *inet_host;
  /// The port number.
  char *inet_port;
} eventlog_socket_inet_addr_t;

typedef struct eventlog_socket_opts {
  bool wait;
  int so_sndbuf;
} eventlog_socket_opts_t;

typedef struct eventlog_socket {
  eventlog_socket_tag_t tag;
  union {
    /// The address for an `EVENTLOG_SOCKET_UNIX` socket.
    eventlog_socket_unix_addr_t unix_addr;
    /// The address for an `EVENTLOG_SOCKET_INET` socket.
    eventlog_socket_inet_addr_t inet_addr;
  };
} eventlog_socket_t;

// note: does not free `eventlog_socket_t` itself
void eventlog_socket_free(eventlog_socket_t *eventlog_socket);

// writes default options
void eventlog_socket_opts_init(eventlog_socket_opts_t *eventlog_socket_opts);

// note: does not free `eventlog_socket_opts_free` itself
void eventlog_socket_opts_free(eventlog_socket_opts_t *opts);

extern const EventLogWriter SocketEventLogWriter;

typedef enum {
  EVENTLOG_SOCKET_FROM_ENV_OK,
  EVENTLOG_SOCKET_FROM_ENV_NOTFOUND,
  EVENTLOG_SOCKET_FROM_ENV_TOOLONG,
  EVENTLOG_SOCKET_FROM_ENV_INVAL,
} eventlog_socket_from_env_status_t;

/// @par MT-Unsafe
eventlog_socket_from_env_status_t
eventlog_socket_from_env(eventlog_socket_t *eventlog_socket_out,
                         eventlog_socket_opts_t *eventlog_socket_opts_out);

// Use this when you install SocketEventLogWriter via RtsConfig before hs_main.
// It spawns the worker immediately but defers handling of control messages
// until eventlog_socket_ready() is invoked after RTS initialization.
void eventlog_socket_init(const eventlog_socket_t *eventlog_socket,
                          const eventlog_socket_opts_t *opts);

void eventlog_socket_init_unix(char *unix_path);

void eventlog_socket_init_inet(char *inet_host, char *inet_port);

/// @pre The GHC RTS is ready.
/// @pre The function `eventlog_socket_init` has been called.
void eventlog_socket_attach(void);

// Use this from an already-running RTS: it reconfigures eventlogging to use
// SocketEventLogWriter and restarts the log when a client connects.
void eventlog_socket_start(const eventlog_socket_t *eventlog_socket,
                           const eventlog_socket_opts_t *opts);

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

#endif /* EVENGLOG_SOCKET_H */
