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

/// @brief The address family of the eventlog socket.
///
/// Used as the tag for the tagged union `EventlogSocketAddr`.
typedef enum EventlogSocketTag {
  EVENTLOG_SOCKET_UNIX,
  EVENTLOG_SOCKET_INET,
} EventlogSocketTag;

/// @brief The address for a Unix domain socket.
typedef struct EventlogSocketUnixAddr {
  /// The path to the Unix domain socket.
  char *esa_unix_path;
} EventlogSocketUnixAddr;

/// @brief The address for a TCP/IPv4 socket.
typedef struct EventlogSocketInetAddr {
  /// The host name.
  char *esa_inet_host;
  /// The port number.
  char *esa_inet_port;
} EventlogSocketInetAddr;

/// @brief The options for an eventlog socket.
typedef struct EventlogSocketOpts {
  /// @brief Whether or not to wait for a client to connect.
  bool eso_wait;
  /// @brief The size of the send buffer.
  ///
  /// See the documentation for `SO_SNDBUF` in `socket.h`.
  int eso_sndbuf;
} EventlogSocketOpts;

/// @brief The address for an eventlog socket.
typedef struct EventlogSocketAddr {
  /// @brief The address family.
  ///
  /// @invariant If `esa_tag` is `EVENTLOG_SOCKET_UNIX`, then `esa_unix_addr` is
  /// set.
  /// @invariant If `esa_tag` is `EVENTLOG_SOCKET_INET`, then `esa_inet_addr` is
  /// set.
  EventlogSocketTag esa_tag;
  union {
    /// @brief The address for a Unix domain socket.
    EventlogSocketUnixAddr esa_unix_addr;
    /// @brief The address for a TCP/IPv4 socket.
    EventlogSocketInetAddr esa_inet_addr;
  };
} EventlogSocketAddr;

/// @brief Free any memory allocated by the members of the `EventlogSocketAddr`
/// value.
///
/// @warning This function does not free the memory allocated for the
/// `EventlogSocketAddr` value itself.
void eventlog_socket_addr_free(EventlogSocketAddr *eventlog_socket);

/// @brief Initialise the `EventlogSocketOpts` object with the default options.
void eventlog_socket_opts_init(EventlogSocketOpts *eventlog_socket_opts);

/// @brief Free any memory allocated by the members of the `EventlogSocketOpts`
/// value.
///
/// @warning This function does not free the memory allocated for the
/// `EventlogSocketOpts` value itself.
void eventlog_socket_opts_free(EventlogSocketOpts *opts);

/// @brief The return status for `eventlog_socket_from_env`.
///
/// @see eventlog_socket_from_env
typedef enum EventlogSocketFromEnvStatus {
  /// @brief Successfully initialised the socket address and options.
  EVENTLOG_SOCKET_FROM_ENV_OK,
  /// @brief Did not find any socket address.
  EVENTLOG_SOCKET_FROM_ENV_NONE,
  /// @brief Received invalid arguments.
  EVENTLOG_SOCKET_FROM_ENV_INVAL,
  /// @brief The found Unix domain socket path was too long.
  EVENTLOG_SOCKET_FROM_ENV_UNIX_PATH_TOO_LONG,
  /// @brief No TCP/IP port number was found, but no host name was found.
  EVENTLOG_SOCKET_FROM_ENV_INET_HOST_MISSING,
  /// @brief A TCP/IP host name was found, but no port number was found.
  EVENTLOG_SOCKET_FROM_ENV_INET_PORT_MISSING,
} EventlogSocketFromEnvStatus;

/// @brief
/// The name of the environment variable used by `eventlog_socket_from_env`
/// to determine the path to the Unix domain socket.
#define EVENTLOG_SOCKET_ENV_UNIX_PATH "GHC_EVENTLOG_UNIX_PATH"

/// @brief
/// The name of the environment variable used by `eventlog_socket_from_env`
/// to determine the host name for a TCP/IPv4 socket.
#define EVENTLOG_SOCKET_ENV_INET_HOST "GHC_EVENTLOG_INET_HOST"

/// @brief
/// The name of the environment variable used by `eventlog_socket_from_env`
/// to determine the port number for a TCP/IPv4 socket.
#define EVENTLOG_SOCKET_ENV_INET_PORT "GHC_EVENTLOG_INET_PORT"

/// @brief
/// The name of the environment variable used by `eventlog_socket_from_env`
/// to determine whether or not to wait.
#define EVENTLOG_SOCKET_ENV_WAIT "GHC_EVENTLOG_WAIT"

/// @brief Start the eventlog socket writer.
///
/// Use this when you install the eventlog socket writer *after* the GHC RTS has
/// started. This is equivalent to `eventlog_socket_init` followed by
/// evaluating the main closure with `eventlog_socket_wrap_hs_main`.
/// This is primarily intended to be called from Haskell.
/// To install the eventlog socket writer *before* the GHC RTS has started, use
/// `eventlog_socket_init` and `eventlog_socket_wrap_hs_main`.
///
/// @pre The GHC RTS is initialized.
/// @pre The argument `eventlog_socket_addr` is nonnull.
void eventlog_socket_start(const EventlogSocketAddr *eventlog_socket_addr,
                           const EventlogSocketOpts *eventlog_socket_opts);

/// @brief
/// Read the eventlog socket address and options from the environment.
///
/// @par MT-Unsafe
///
/// @return
///   Upon successful completion, the value `EVENTLOG_SOCKET_FROM_ENV_OK` is
///   returned. A valid object is written to `eventlog_socket_addr_out`, which
///   must be freed using `eventlog_socket_addr_free`. If
///   `eventlog_socket_opts_out` was nonnull, then a valid object is written to
///   `eventlog_socket_opts_out`, which must be freed using
///   `eventlog_socket_opts_free`.
///
/// @return
///   If no socket address is found, the value `EVENTLOG_SOCKET_FROM_ENV_NONE`
///   is returned and the content of `eventlog_socket_addr_out` and
///   `eventlog_socket_opts_out` are unchanged.
///
/// @return
///   If `eventlog_socket_addr_out` was null, then
///   `EVENTLOG_SOCKET_FROM_ENV_INVAL` is returned and the content of
///   `eventlog_socket_addr_out` and `eventlog_socket_opts_out` are unchanged.
///
/// @return
///   If any other value is returned, then a valid object is written to
///   `eventlog_socket_addr_out` as if `EVENTLOG_SOCKET_FROM_ENV_OK` was
///   returned, which must be freed using `eventlog_socket_addr_free. If
///   `eventlog_socket_opts_out` was nonnull, then a valid object is written to
///   `eventlog_socket_opts_out`, which must be freed using
///   `eventlog_socket_opts_free`. These objects is for use in error messages
///   only, and should not be passed to `eventlog_socket_init` or
///   `eventlog_socket_start`. For details, see `EventlogSocketFromEnvStatus`.
///
/// @see EventlogSocketFromEnvStatus
EventlogSocketFromEnvStatus
eventlog_socket_from_env(EventlogSocketAddr *eventlog_socket_addr_out,
                         EventlogSocketOpts *eventlog_socket_opts_out);

/// @brief Initialise the eventlog socket.
///
/// Use this when you install the eventlog socket writer *before* the GHC RTS
/// has started. This is primarily intended to be called from a C main function.
/// To install the eventlog socket writer *after* the GHC RTS has started, use
/// `eventlog_socket_start`.
///
/// @pre The argument `eventlog_socket_addr` is nonnull.
void eventlog_socket_init(const EventlogSocketAddr *eventlog_socket_addr,
                          const EventlogSocketOpts *eventlog_socket_opts);

/// @brief Wait for a client to connect to the eventlog socket.
///
/// @pre Either `eventlog_socket_init` or `eventlog_socket_start` was called,
/// with `EventlogSocketOpts.eso_wait` set to `false`.
void eventlog_socket_wait(void);

/// @brief Evaluate the Haskell main closure using the eventlog socket writer.
///
/// Use this when you install the eventlog socket writer *before* the GHC RTS
/// has started. This is primarily intended to be called from a C main function.
/// To install the eventlog socket writer *after* the GHC RTS has started, use
/// `eventlog_socket_start`.
///
/// @return The exit code of the Haskell main function.
///
/// @pre The eventlog socket was initialised using `eventlog_socket_init`.
int eventlog_socket_wrap_hs_main(int argc, char *argv[], RtsConfig rts_config,
                                 StgClosure *main_closure);

/******************************************************************************
 * Control Commands
 ******************************************************************************/

/// @brief The version of the eventlog-socket control command protocol
/// implemented by this version of the library.
#define EVENTLOG_SOCKET_CONTROL_PROTOCOL_VERSION 0

/// @brief A control command namespace.
///
/// See `eventlog_socket_control_register_namespace`.
typedef struct EventlogSocketControlNamespace EventlogSocketControlNamespace;

/// @brief A control command ID.
///
/// Individual commands are identified by numbers (0-255).
typedef uint8_t EventlogSocketControlCommandId;

/// @brief A control command handler.
///
/// This function is called when the corresponding control command message is
/// received on the eventlog socket. The `command_data` parameter will contain
/// the pointer provided to `eventlog_socket_control_register_command`.
typedef void EventlogSocketControlCommandHandler(
    const EventlogSocketControlNamespace *const namespace,
    const EventlogSocketControlCommandId command_id, const void *command_data);

/// @brief Register a new namespace.
///
/// @return If there is no existing namespace with the given name, this function
/// registers a new namespace and returns a stable pointer to it. Otherwise, it
/// returns NULL. The returned pointer should not be freed.
EventlogSocketControlNamespace *
eventlog_socket_control_register_namespace(uint8_t namespace_len,
                                           const char namespace[namespace_len]);

/// @brief Register a new command.
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
