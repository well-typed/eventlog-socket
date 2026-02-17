/// @file eventlog_socket.h
#ifndef EVENGLOG_SOCKET_H
#define EVENGLOG_SOCKET_H

/// @mainpage
/// This is the documentation for the @c eventlog-socket C API.
/// For most uses, the Haskell API is sufficient.
/// However, if you want to instrument your application from a C main or install
/// custom command handlers you need the C API.
///
/// 1. You want to start @c eventlog-socket from a C main function.
///
///    If you instrument your application with @c eventlog-socket using the
///    Haskell API, then the RTS is started with the default file writer, which
///    means that the first few events are written to a file instead of the
///    eventlog socket.
///
///    To avoid this, you can instrument your application using the C API. See
///    `eventlog_socket_init` and `eventlog_socket_wrap_hs_main`.
///
/// 2. You want to register custom commands for use with the @c eventlog-socket
///    control protocol.
///
///    Presently, this has to be done using the C API.
///
///    For details, `eventlog_socket_control_register_namespace` and
///    `eventlog_socket_control_register_command`.
///
///
/// The main entry point for the C API is eventlog_socket.h. This file is
/// installed as part of the @c eventlog-socket package and should be available
/// from the C bits of your Haskell package.

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
  /// @brief The size of the socket send buffer.
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

/// @brief Initialise the eventlog socket.
///
/// Use this when you install the eventlog socket writer *before* the GHC RTS
/// has started. This is primarily intended to be called from a C main function.
/// To install the eventlog socket writer *after* the GHC RTS has started, use
/// `eventlog_socket_start`.
///
/// @pre The argument `eventlog_socket_addr` is nonnull.
///
/// @par Examples
/// @parblock
/// The following function initialises eventlog socket from a C main function.
/// @code{.c}
/// #include <Rts.h>
/// #include <eventlog_socket.h>
/// #include <stdlib.h>
///
/// // Define the eventlog Unix domain socket path.
/// #define MY_EVENTLOG_SOCKET "/tmp/my_eventlog.socket"
///
/// // Get the closure for the Haskell main.
/// extern StgClosure ZCMain_main_closure;
///
/// int main(int argc, char* argv[]) {
///
///  // Create a GHC RTS configuration object.
///  RtsConfig rts_config = {0};
///  memcpy(&rts_config, &defaultRtsConfig, sizeof(RtsConfig));
///  rts_config.rts_opts_enabled = RtsOptsAll; // Enable all RTS options.
///  rts_config.rts_opts = "-l";               // Enable binary eventlog.
///
///   // Initialise eventlog socket using the default options.
///   EventlogSocketAddr eventlog_socket_addr = {
///       .esa_tag = EVENTLOG_SOCKET_UNIX,
///       .esa_unix_addr = {
///           .esa_unix_path = MY_EVENTLOG_SOCKET,
///       }};
///   eventlog_socket_init(eventlog_socket_addr, NULL);
///
///   // Evaluate the close for the Haskell main.
///   return eventlog_socket_wrap_hs_main(argc, argv, rts_config,
///                                       &ZCMain_main_closure);
/// }
/// @endcode
/// @endparblock
void eventlog_socket_init(const EventlogSocketAddr *eventlog_socket_addr,
                          const EventlogSocketOpts *eventlog_socket_opts);

/// @brief Read the eventlog socket address and options from the environment.
///
/// @par MT-Unsafe
///
/// @return Upon successful completion, the value `EVENTLOG_SOCKET_FROM_ENV_OK`
/// is returned. A valid object is written to @p eventlog_socket_addr_out, which
/// must be freed using `eventlog_socket_addr_free`. If
/// @p eventlog_socket_opts_out was nonnull, then a valid object is written to
/// @p eventlog_socket_opts_out, which must be freed using
/// `eventlog_socket_opts_free`.
///
/// @return If no socket address is found, the value
/// `EVENTLOG_SOCKET_FROM_ENV_NONE` is returned and the content of
/// @p eventlog_socket_addr_out and @p eventlog_socket_opts_out are unchanged.
///
/// @return If @p eventlog_socket_addr_out was null, then
/// `EVENTLOG_SOCKET_FROM_ENV_INVAL` is returned and the content of
/// @p eventlog_socket_addr_out and @p eventlog_socket_opts_out are unchanged.
///
/// @return If any other value is returned, then a valid object is written to
/// @p eventlog_socket_addr_out as if `EVENTLOG_SOCKET_FROM_ENV_OK` was
/// returned, which must be freed using `eventlog_socket_addr_free`. If
/// @p eventlog_socket_opts_out was nonnull, then a valid object is written to
/// @p eventlog_socket_opts_out, which must be freed using
/// `eventlog_socket_opts_free`. These objects is for use in error messages
/// only, and should not be passed to `eventlog_socket_init` or
/// `eventlog_socket_start`. For details, see `EventlogSocketFromEnvStatus`.
///
/// @par Examples
/// @parblock
/// The following function initialises eventlog socket using a socket address
/// and options from the environment.
/// @code{.c}
/// EventlogSocketFromEnvStatus init_from_env(void) {
///
///   // Read the socket address and options from the environment.
///   EventlogSocketAddr eventlog_socket_addr = {0};
///   EventlogSocketOpts eventlog_socket_opts = {0};
///   const EventlogSocketFromEnvStatus status =
///       eventlog_socket_from_env(&eventlog_socket_addr,
///       &eventlog_socket_opts);
///
///   // Handle the return status.
///   switch (status) {
///   case EVENTLOG_SOCKET_FROM_ENV_OK:
///     // Initialise eventlog socket.
///     eventlog_socket_init(&eventlog_socket_addr, &eventlog_socket_opts);
///     break;
///   case EVENTLOG_SOCKET_FROM_ENV_NONE:
///     return status; // Skip free.
///   case EVENTLOG_SOCKET_FROM_ENV_INVAL:
///     return status; // Skip free.
///   case EVENTLOG_SOCKET_FROM_ENV_UNIX_PATH_TOO_LONG:
///     fprintf(stderr, "Error: value of %s (%s) is too long\n",
///             EVENTLOG_SOCKET_ENV_UNIX_PATH,
///             eventlog_socket_addr.esa_unix_addr.esa_unix_path);
///     break;
///   case EVENTLOG_SOCKET_FROM_ENV_INET_HOST_MISSING:
///     fprintf(stderr, "Error: no value given for %s\n",
///             EVENTLOG_SOCKET_ENV_INET_HOST);
///     break;
///   case EVENTLOG_SOCKET_FROM_ENV_INET_PORT_MISSING:
///     fprintf(stderr, "Error: no value given for %s\n",
///             EVENTLOG_SOCKET_ENV_INET_PORT);
///     break;
///   }
///
///   // Free the memory held by socket address and options.
///   eventlog_socket_addr_free(&eventlog_socket_addr);
///   eventlog_socket_opts_free(&eventlog_socket_opts);
///
///   return status;
/// }
/// @endcode
/// @endparblock
EventlogSocketFromEnvStatus
eventlog_socket_from_env(EventlogSocketAddr *eventlog_socket_addr_out,
                         EventlogSocketOpts *eventlog_socket_opts_out);

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
///
/// @par Examples
/// @parblock
/// See `eventlog_socket_init`.
/// @endparblock
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
