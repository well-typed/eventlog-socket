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
 * Error Types
 ******************************************************************************/

typedef enum EventlogSocketStatusCode {
  /// @brief The operation completed successfully.
  EVENTLOG_SOCKET_OK = 0,
  /// @brief The GHC RTS does not support the eventlog.
  EVENTLOG_SOCKET_ERROR_RTS_NOSUPPORT,
  /// @brief The GHC RTS failed to start the eventlog writer.
  EVENTLOG_SOCKET_ERROR_RTS_FAIL,
  /// @brief Configuration via the environment could not find an address.
  EVENTLOG_SOCKET_ERROR_CNF_NOADDR,
  /// @brief Configuration via the environment found a Unix domain socket path
  /// that is too long.
  EVENTLOG_SOCKET_ERROR_CNF_TOOLONG,
  /// @brief Configuration via the environment found a TCP/IP port number but no
  /// host name.
  EVENTLOG_SOCKET_ERROR_CNF_NOHOST,
  /// @brief Configuration via the environment found a TCP/IP host name but no
  /// port number.
  EVENTLOG_SOCKET_ERROR_CNF_NOPORT,
  /// @brief The control command is already registered.
  EVENTLOG_SOCKET_ERROR_CMD_EXISTS,
  /// @brief An error occurred in `getaddrinfo`; the @c error_code member is set
  /// to indicate the error.
  EVENTLOG_SOCKET_ERROR_GAI,
  /// @brief A system error occurred; the @c error_code member is set to
  /// indicate the error.
  EVENTLOG_SOCKET_ERROR_SYSTEM,
} EventlogSocketStatusCode;

typedef struct EventlogSocketStatus {
  /// @brief The eventlog socket error code.
  const EventlogSocketStatusCode ess_status_code;
  /// @brief An error code returned by `getaddrinfo` or the system call, if any.
  const int ess_error_code;
} EventlogSocketStatus;

/// @brief Return a string that describese the status.
///
/// @return Upon successful completion, a buffer containing a string that
/// describes the @p status is returned. This buffer is allocated with @c malloc
/// and must be freed with @c free.
///
/// @return On error, @c NULL is returned and @c errno is set to indicate the
/// error.
char *eventlog_socket_strerror(EventlogSocketStatus status);

/******************************************************************************
 * Eventlog Writer
 ******************************************************************************/

/// @brief The address family of the eventlog socket.
///
/// Used as the tag for the tagged union `EventlogSocketAddr`.
typedef enum EventlogSocketTag {
  /// @brief A Unix domain socket address.
  EVENTLOG_SOCKET_UNIX,
  /// @brief A TCP/IP address.
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
void eventlog_socket_addr_free(EventlogSocketAddr *eventlog_socket);

/// @brief Initialise the `EventlogSocketOpts` object with the default options.
void eventlog_socket_opts_init(EventlogSocketOpts *eventlog_socket_opts);

/// @brief Free any memory allocated by the members of the `EventlogSocketOpts`
/// value.
void eventlog_socket_opts_free(EventlogSocketOpts *opts);

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
///
/// @pre The argument `eventlog_socket_addr` is nonnull.
EventlogSocketStatus
eventlog_socket_start(const EventlogSocketAddr *eventlog_socket_addr,
                      const EventlogSocketOpts *eventlog_socket_opts);

/// @brief Read the eventlog socket address and options from the environment.
///
/// @par MT-Unsafe
///
/// @return Upon successful completion, the status code `EVENTLOG_SOCKET_OK` is
/// returned  A valid object is written to @p eventlog_socket_addr_out, which
/// must be freed using `eventlog_socket_addr_free`. If @p
/// eventlog_socket_opts_out was nonnull, then a valid object is written to @p
/// eventlog_socket_opts_out, which must be freed using
/// `eventlog_socket_opts_free`.
///
/// @return If no socket address is found, the status code
/// `EVENTLOG_SOCKET_ERROR_CNF_NOADDR` is returned and the content of
/// @p eventlog_socket_addr_out and @p eventlog_socket_opts_out are unchanged.
///
/// @return If an invalid address is found, one of
/// `EVENTLOG_SOCKET_ERROR_CNF_TOOLONG`, `EVENTLOG_SOCKET_ERROR_CNF_NOHOST`, or
/// `EVENTLOG_SOCKET_ERROR_CNF_NOPORT` is returned. A valid object is written to
/// @p eventlog_socket_addr_out, which must be freed using
/// `eventlog_socket_addr_free`. If @p eventlog_socket_opts_out was nonnull,
/// then a valid object is written to @p eventlog_socket_opts_out, which must be
/// freed using `eventlog_socket_opts_free`. These objects is for use in error
/// messages only, and should not be passed to `eventlog_socket_init` or
/// `eventlog_socket_start`.
///
/// @return If any other status is returned, the contents of
/// @p eventlog_socket_addr_out and @p eventlog_socket_opts_out are unchanged.
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
///   case EVENTLOG_SOCKET_FROM_ENV_SYSTEM:
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
EventlogSocketStatus
eventlog_socket_from_env(EventlogSocketAddr *eventlog_socket_addr_out,
                         EventlogSocketOpts *eventlog_socket_opts_out);

/// @brief Initialise the eventlog socket.
///
/// Use this when you install the eventlog socket writer *before* the GHC RTS
/// has started. This is primarily intended to be called from a C main function.
/// To install the eventlog socket writer *after* the GHC RTS has started, use
/// `eventlog_socket_start`.
///
/// @return See `EventlogSocketStatus`.
///
/// @par Errors
/// @parblock
/// This function may return the following system errors:
/// `EACCES`, `EADDRINUSE`, `EADDRNOTAVAIL`, `EAFNOSUPPORT`, `EAGAIN`,
/// `EALREADY`, `EBADF`, `EDESTADDRREQ`, `EDOM`, `EFAULT`, `EINPROGRESS`,
/// `EINVAL`, `EIO`, `EISCONN`, `EISDIR`, `ELOOP`, `EMFILE`, `ENAMETOOLONG`,
/// `ENFILE`, `ENOBUFS`, `ENOENT`, `ENOMEM`, `ENOPKG`, `ENOPROTOOPT`, `ENOTDIR`,
/// `ENOTSOCK`, `EOPNOTSUPP`, `EPERM`, `EPROTONOSUPPORT`, or `EROFS`.
///
/// This function may return the following `getaddrinfo` errors:
/// `EAI_ADDRFAMILY`, `EAI_AGAIN`, `EAI_BADFLAGS`, `EAI_FAIL`, `EAI_FAMILY`,
/// `EAI_MEMORY`, `EAI_NODATA`, `EAI_NONAME`, `EAI_SERVICE`, `EAI_SOCKTYPE`, or
/// `EAI_SYSTEM`.
/// @endparblock
///
/// @par Examples
/// @parblock
/// The following function initialises eventlog socket from a C main function.
/// @code{.c}
/// #include <Rts.h>
/// #include <eventlog_socket.h>
/// #include <netdb.h>
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
///     .esa_tag = EVENTLOG_SOCKET_UNIX,
///     .esa_unix_addr = {
///       .esa_unix_path = MY_EVENTLOG_SOCKET,
///     }};
///   const int success_or_eaino =
///     eventlog_socket_init(&eventlog_socket_addr, NULL);
///   if (success_or_eaino != 0) {
///     fprintf(stderr, "eventlog_socket_init() failed: %s",
///             gai_strerror(success_or_eaino));
///   }
///
///   // Evaluate the close for the Haskell main.
///   return eventlog_socket_wrap_hs_main(argc, argv, rts_config,
///                                       &ZCMain_main_closure);
/// }
/// @endcode
/// @endparblock
EventlogSocketStatus
eventlog_socket_init(const EventlogSocketAddr *eventlog_socket_addr,
                     const EventlogSocketOpts *eventlog_socket_opts);

/// @brief Evaluate the Haskell main closure using the eventlog socket writer.
///
/// Use this when you install the eventlog socket writer *before* the GHC RTS
/// has started. This is primarily intended to be called from a C main function.
/// To install the eventlog socket writer *after* the GHC RTS has started, use
/// `eventlog_socket_start`.
///
/// This function attaches the `SocketEventLogWriter` to the @p rts_config,
/// initializes the GHC RTS and calls `eventlog_socket_signal_ghc_rts_ready`,
/// and finally evaluates the Haskell main closure.
///
/// If your needs are more fine-grained, you can use `SocketEventLogWriter`,
/// `eventlog_socket_signal_ghc_rts_ready`, and the GHC RTS API directly.
///
/// @pre The eventlog socket was successfully initialised using
/// `eventlog_socket_init`.
///
/// @par Examples
/// @parblock
/// See `eventlog_socket_init`.
/// @endparblock
void eventlog_socket_wrap_hs_main(int argc, char *argv[], RtsConfig rts_config,
                                  StgClosure *main_closure);

/// @brief Attaches the global `EventLogWriter` object to an @c RtsConfig.
///
/// @note You do not need to call this function if you're starting eventlog
/// socket using the Haskell API, via `eventlog_socket_start`, or via
/// `eventlog_socket_wrap_hs_main`.
RtsConfig eventlog_socket_attach_rts_config(RtsConfig rts_config);

/// @brief Signal that the GHC RTS is ready.
///
/// Since control command handlers may call function from the GHC RTS API, no
/// control commands are executed until the GHC RTS is ready.
///
/// @pre The GHC RTS is initialized.
///
/// @note You do not need to call this function if you're starting eventlog
/// socket using the Haskell API, via `eventlog_socket_start`, or via
/// `eventlog_socket_wrap_hs_main`.
///
/// @return Upon successful completion, 0 is returned.
///
/// @return On error, On error, -1 is returned, errno is set to indicate the
/// error.
///
/// @par Errors
/// @parblock
/// `EAGAIN`, `EBUSY`, `EDEADLK`, `EINVAL`, `ENOTRECOVERABLE`, `EOWNERDEAD`, or
/// `EPERM`.
/// @endparblock
EventlogSocketStatus eventlog_socket_signal_ghc_rts_ready(void);

/// @brief Wait for a client to connect to the eventlog socket.
///
/// @pre The eventlog socket was successfully initialised using
/// `eventlog_socket_init` or `eventlog_socket_start`.
///
/// @note If the eventlog socket is initialised with
/// `EventlogSocketOpts.eso_wait` set to `true`, then `eventlog_socket_wait` is
/// called automatically.
///
/// @return Upon successful completion, 0 is returned.
///
/// @return On error, On error, -1 is returned, errno is set to indicate the
/// error.
///
/// @par Errors
/// @parblock
/// `EAGAIN`, `EBUSY`, `EDEADLK`, `EINVAL`, `ENOTRECOVERABLE`, `EOWNERDEAD`, or
/// `EPERM`.
/// @endparblock
EventlogSocketStatus eventlog_socket_wait(void);

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
/// @return Upon successful completion, `EVENTLOG_SOCKET_REGISTER_COMMAND_OK` is
/// returned.
///
/// @return If there is an existing command in the given namespace with the
/// given ID, `EVENTLOG_SOCKET_REGISTER_COMMAND_EXISTS` is returned.
///
/// @return If a system error occurs, `EVENTLOG_SOCKET_REGISTER_COMMAND_SYSTEM`
/// is returned and errno is set to indicate the error.
EventlogSocketStatus eventlog_socket_control_register_command(
    EventlogSocketControlNamespace *namespace,
    EventlogSocketControlCommandId command_id,
    EventlogSocketControlCommandHandler command_handler,
    const void *command_data);

#endif /* EVENGLOG_SOCKET_H */
