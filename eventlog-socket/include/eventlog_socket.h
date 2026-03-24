/// @file eventlog_socket.h
#ifndef EVENGLOG_SOCKET_H
#define EVENGLOG_SOCKET_H

/// @mainpage
/// This is the documentation for the @c eventlog-socket C API. For most uses,
/// the Haskell API is sufficient. However, if you want to start
/// @c eventlog-socket from a C main function, you need the C API.
///
/// If you instrument your application with @c eventlog-socket using the
/// Haskell API, then the RTS is started with the default file writer, which
/// means that the first few events are written to a file instead of the
/// eventlog socket. To avoid this, you can instrument your application using
/// the C API. See `eventlog_socket_wrap_hs_main`.
///
/// If you want to register custom commands via the C API, see
/// `eventlog_socket_control_register_namespace` and
/// `eventlog_socket_control_register_command`.
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

/// @brief The status codes for `EventlogSocketStatus`.
///
/// @since 0.1.1.1
typedef enum EventlogSocketStatusCode {
  /// @brief The operation completed successfully.
  EVENTLOG_SOCKET_OK = 0,
  /// @brief The GHC RTS does not support the eventlog.
  EVENTLOG_SOCKET_ERR_RTS_NOSUPPORT,
  /// @brief The GHC RTS failed to start the eventlog writer.
  EVENTLOG_SOCKET_ERR_RTS_FAIL,
  /// @brief Configuration via the environment could not find an address.
  EVENTLOG_SOCKET_ERR_ENV_NOADDR,
  /// @brief Configuration via the environment found a Unix domain socket path
  /// that is too long.
  EVENTLOG_SOCKET_ERR_ENV_TOOLONG,
  /// @brief Configuration via the environment found a TCP/IP port number but no
  /// host name.
  EVENTLOG_SOCKET_ERR_ENV_NOHOST,
  /// @brief Configuration via the environment found a TCP/IP host name but no
  /// port number.
  EVENTLOG_SOCKET_ERR_ENV_NOPORT,
  /// @brief The binary was compiled without support for control commands.
  EVENTLOG_SOCKET_ERR_CTL_NOSUPPORT,
  /// @brief The namespace or command ID is already registered.
  EVENTLOG_SOCKET_ERR_CTL_EXISTS,
  /// @brief An error occurred in `getaddrinfo`; the @c error_code member is set
  /// to indicate the error.
  EVENTLOG_SOCKET_ERR_GAI,
  /// @brief A system error occurred; the @c error_code member is set to
  /// indicate the error.
  EVENTLOG_SOCKET_ERR_SYS,
} EventlogSocketStatusCode;

/// @brief The return status for the functions in the @c eventlog-socket C API.
///
/// @since 0.1.1.1
typedef struct EventlogSocketStatus {
  /// @brief The eventlog socket error code.
  const EventlogSocketStatusCode ess_status_code;
  /// @brief An error code returned by `getaddrinfo` or the system call, if any.
  const int ess_error_code;
} EventlogSocketStatus;

/// @brief Return a string that describes the status.
///
/// @return Upon successful completion, a buffer containing a string that
/// describes the @p status is returned. This buffer is allocated with @c malloc
/// and must be freed with @c free.
///
/// @return On error, @c NULL is returned and @c errno is set to indicate the
/// error.
///
/// @since 0.1.1.1
char *eventlog_socket_strerror(EventlogSocketStatus status);

/******************************************************************************
 * Eventlog Writer
 ******************************************************************************/

/// @brief The address family of the eventlog socket.
///
/// Used as the tag for the tagged union `EventlogSocketAddr`.
///
/// @since 0.1.1.1
typedef enum EventlogSocketTag {
  /// @brief A Unix domain socket address.
  EVENTLOG_SOCKET_UNIX,
  /// @brief A TCP/IP address.
  EVENTLOG_SOCKET_INET,
} EventlogSocketTag;

/// @brief The address for a Unix domain socket.
///
/// @since 0.1.1.1
typedef struct EventlogSocketUnixAddr {
  /// The path to the Unix domain socket.
  char *esa_unix_path;
} EventlogSocketUnixAddr;

/// @brief The address for a TCP/IPv4 socket.
///
/// @since 0.1.1.1
typedef struct EventlogSocketInetAddr {
  /// The host name.
  char *esa_inet_host;
  /// The port number.
  char *esa_inet_port;
} EventlogSocketInetAddr;

/// @brief The options for an eventlog socket.
///
/// @since 0.1.1.1
typedef struct EventlogSocketOpts {
  /// @brief Whether or not to wait for a client to connect.
  bool eso_wait;
  /// @brief The size of the socket send buffer.
  ///
  /// See the documentation for `SO_SNDBUF` in `socket.h`.
  int eso_sndbuf;
  /// @brief The number of seconds to linger.
  int eso_linger;
} EventlogSocketOpts;

/// @brief The address for an eventlog socket.
///
/// @since 0.1.1.1
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
/// @since 0.1.1.1
void eventlog_socket_addr_free(EventlogSocketAddr *eventlog_socket);

/// @brief Initialise the `EventlogSocketOpts` object with the default options.
///
/// @since 0.1.1.1
void eventlog_socket_opts_init(EventlogSocketOpts *eventlog_socket_opts);

/// @brief Free any memory allocated by the members of the `EventlogSocketOpts`
/// value.
///
/// @since 0.1.1.1
void eventlog_socket_opts_free(EventlogSocketOpts *opts);

/// @brief
/// The name of the environment variable used by `eventlog_socket_from_env`
/// to determine the path to the Unix domain socket.
///
/// @since 0.1.1.1
#define EVENTLOG_SOCKET_ENV_UNIX_PATH "GHC_EVENTLOG_UNIX_PATH"

/// @brief
/// The name of the environment variable used by `eventlog_socket_from_env`
/// to determine the host name for a TCP/IPv4 socket.
///
/// @since 0.1.1.1
#define EVENTLOG_SOCKET_ENV_INET_HOST "GHC_EVENTLOG_INET_HOST"

/// @brief
/// The name of the environment variable used by `eventlog_socket_from_env`
/// to determine the port number for a TCP/IPv4 socket.
///
/// @since 0.1.1.1
#define EVENTLOG_SOCKET_ENV_INET_PORT "GHC_EVENTLOG_INET_PORT"

/// @brief
/// The name of the environment variable used by `eventlog_socket_from_env`
/// to determine whether or not to wait.
///
/// @since 0.1.1.1
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
///
/// @since 0.1.1.1
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
/// `EVENTLOG_SOCKET_ERR_ENV_NOADDR` is returned and the content of
/// @p eventlog_socket_addr_out and @p eventlog_socket_opts_out are unchanged.
///
/// @return If an invalid address is found, one of
/// `EVENTLOG_SOCKET_ERR_ENV_TOOLONG`, `EVENTLOG_SOCKET_ERR_ENV_NOHOST`, or
/// `EVENTLOG_SOCKET_ERR_ENV_NOPORT` is returned. A valid object is written to
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
/// @since 0.1.1.1
///
/// @par Examples
/// @parblock
/// The following function initialises eventlog socket from a C main function.
/// @code{.c}
/// #include <stdio.h>
/// #include <stdlib.h>
/// #include <Rts.h>
/// #include <eventlog_socket.h>
///
/// // Define the eventlog Unix domain socket path.
/// #define MY_EVENTLOG_SOCKET "/tmp/my_eventlog.socket"
///
/// // Get the closure for the Haskell main.
/// extern StgClosure ZCMain_main_closure;
///
/// int main(int argc, char* argv[]) {
///
///   // Create a GHC RTS configuration object.
///   RtsConfig rts_config = {0};
///   memcpy(&rts_config, &defaultRtsConfig, sizeof(RtsConfig));
///   rts_config.rts_opts_enabled = RtsOptsAll; // Enable all RTS options.
///   rts_config.rts_opts = "-l";               // Enable binary eventlog.
///
///   // Read the socket address and options from the environment.
///   EventlogSocketAddr addr = {0};
///   EventlogSocketOpts opts = {0};
///   EventlogSocketStatus status = eventlog_socket_from_env(&addr, &opts);
///
///   // Handle the return status.
///   switch (status) {
///   case EVENTLOG_SOCKET_FROM_ENV_OK:
///     // Evaluate the wrapped Haskell main closure.
///     return eventlog_socket_wrap_hs_main(argc, argv, rts_config,
///                                         &ZCMain_main_closure, &addr, &opts);
///   case EVENTLOG_SOCKET_ERR_ENV_NOADDR:
///     // Return with an exit failure, skip free.
///     exit(EXIT_FAILURE);
///   case EVENTLOG_SOCKET_ERR_ENV_TOOLONG:
///     fprintf(stderr, "Error: value of %s (%s) is too long\n",
///             EVENTLOG_SOCKET_ENV_UNIX_PATH,
///             eventlog_socket_addr.esa_unix_addr.esa_unix_path);
///     break;
///   case EVENTLOG_SOCKET_ERR_ENV_NOHOST:
///     fprintf(stderr, "Error: no value given for %s\n",
///             EVENTLOG_SOCKET_ENV_INET_HOST);
///     break;
///   case EVENTLOG_SOCKET_ERR_ENV_NOPORT:
///     fprintf(stderr, "Error: no value given for %s\n",
///             EVENTLOG_SOCKET_ENV_INET_PORT);
///     break;
///   default:
///     char *errmsg = eventlog_socket_strerror(status);
///     if (errmsg != NULL) {
///       fprintf(stderr, "Error: %s\n", errmsg);
///       free(errmsg);
///     }
///     break;
///   }
///
///   // Free the memory held by socket address and options.
///   eventlog_socket_addr_free(&eventlog_socket_addr);
///   eventlog_socket_opts_free(&eventlog_socket_opts);
///
///   // Return with an exit failure.
///   exit(EXIT_FAILURE);
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
/// @note You do not need to call this function if you're starting eventlog
/// socket using the Haskell API, via `eventlog_socket_start`, or via
/// `eventlog_socket_wrap_hs_main`.
///
/// @warning `eventlog_socket_init` ignores the @c eso_wait option.
///
/// @return See `EventlogSocketStatus`.
///
/// @since 0.1.1.1
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
/// This function attaches the eventlog-socket writer to the @p rts_config,
/// initializes eventlog-socket and the GHC RTS, and evaluates the Haskell
/// main closure.
///
/// If your needs are more fine-grained, you can use `eventlog_socket_init`,
/// `EventlogSocketWriter`, `eventlog_socket_signal_ghc_rts_ready`, and the
/// GHC RTS API directly. For details, see the code for this function.
///
/// @par Examples
/// @parblock
/// The following function initialises eventlog socket from a C main function.
/// @code{.c}
/// #include <Rts.h>
/// #include <eventlog_socket.h>
///
/// // Define the eventlog Unix domain socket path.
/// #define MY_EVENTLOG_SOCKET "/tmp/my_eventlog.socket"
///
/// // Get the closure for the Haskell main.
/// extern StgClosure ZCMain_main_closure;
///
/// int main(int argc, char* argv[]) {
///
///   // Create a GHC RTS configuration object.
///   RtsConfig rts_config = {0};
///   memcpy(&rts_config, &defaultRtsConfig, sizeof(RtsConfig));
///   rts_config.rts_opts_enabled = RtsOptsAll; // Enable all RTS options.
///   rts_config.rts_opts = "-l";               // Enable binary eventlog.
///
///   // Create the eventlog-socket address
///   const EventlogSocketAddr addr = {
///     .esa_tag = EVENTLOG_SOCKET_UNIX,
///     .esa_unix_addr = {
///       .esa_unix_path = MY_EVENTLOG_SOCKET,
///   }};
///
///   // Create the eventlog-socket options
///   EventlogSocketOpts opts = {0};
///   eventlog_socket_opts_init(&opts);
///   opts->eso_wait = true; // Ask eventlog-socket to wait for a connection.
///
///   // Evaluate the wrapped Haskell main closure.
///   return eventlog_socket_wrap_hs_main(argc, argv, rts_config,
///                                       &ZCMain_main_closure, &addr, &opts);
/// }
/// @endcode
/// @endparblock
void eventlog_socket_wrap_hs_main(
    int argc, char *argv[], RtsConfig rts_config, StgClosure *main_closure,
    const EventlogSocketAddr *eventlog_socket_addr,
    const EventlogSocketOpts *eventlog_socket_opts);

/// The global `EventLogWriter` object for @c eventlog-socket.
///
/// @note You do not need to use this object if you're starting eventlog
/// socket using the Haskell API, via `eventlog_socket_start`, or via
/// `eventlog_socket_wrap_hs_main`.
extern const EventLogWriter EventLogSocketWriter;

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
///
/// @since 0.1.1.1
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
///
/// @since 0.1.1.1
EventlogSocketStatus eventlog_socket_wait(void);

/// @brief Read the current status of the worker thread.
///
/// @since 0.1.1.1
EventlogSocketStatus eventlog_socket_worker_status(void);

/// @brief Read the current status of the control thread.
///
/// @since 0.1.1.1
EventlogSocketStatus eventlog_socket_control_status(void);

/******************************************************************************
 * Control Commands
 ******************************************************************************/

/// @brief The version of the eventlog-socket control command protocol
/// implemented by this version of the library.
///
/// @since 0.1.1.1
#define EVENTLOG_SOCKET_CONTROL_PROTOCOL_VERSION 0

/// @brief A control command namespace.
///
/// See `eventlog_socket_control_register_namespace`.
///
/// @since 0.1.1.1
typedef struct EventlogSocketControlNamespace EventlogSocketControlNamespace;

/// @brief A control command ID.
///
/// Individual commands are identified by numbers (0-255).
///
/// @since 0.1.1.1
typedef uint8_t EventlogSocketControlCommandId;

/// @brief A control command handler.
///
/// This function is called when the corresponding control command message is
/// received on the eventlog socket. The `command_data` parameter will contain
/// the pointer provided to `eventlog_socket_control_register_command`.
///
/// @since 0.1.1.1
typedef void EventlogSocketControlCommandHandler(
    const EventlogSocketControlNamespace *const namespace,
    const EventlogSocketControlCommandId command_id, const void *command_data);

/// @brief Get the name of a registered namespace.
///
/// @return Upon successful completion, this function returns an allocated
/// string with the namespace name.
///
/// @return If @p namespace is @c NULL, this function returns @c NULL.
///
/// @return If the binary was compiled without control support, this function
/// returns @c NULL.
///
/// @since 0.1.1.1
const char *
eventlog_socket_control_strnamespace(EventlogSocketControlNamespace *namespace);

/// @brief Register a new namespace.
///
/// @return Upon successful completion, this function registers a new namespace,
/// writes a pointer to it to @p namespace_out, and returns a status with code
/// @c EVENTLOG_SOCKET_OK.
///
/// @return If the given namespace is already registered, this function returns
/// a status with code @c EVENTLOG_SOCKET_ERR_CTL_EXISTS and the object pointer
/// to by @p namespace_out is unchanged.
///
/// @return If @p namespace_out is @c NULL, this function returns a status with
/// code @c EVENTLOG_SOCKET_ERR_SYS and error code @c EINVAL.
///
/// @return If the binary was compiled without control support, this function
/// returns a status with code @c EVENTLOG_SOCKET_ERR_CTL_NOSUPPORT.
///
/// @since 0.1.1.1
EventlogSocketStatus eventlog_socket_control_register_namespace(
    uint8_t namespace_len, const char namespace[namespace_len + 1],
    EventlogSocketControlNamespace **namespace_out);

/// @brief Register a new command.
///
/// @return Upon successful completion, this function registers the command and
/// returns a status with code @c EVENTLOG_SOCKET_OK.
///
/// @return If the given command is already registered, this function returns
/// a status with code @c EVENTLOG_SOCKET_ERR_CTL_EXISTS.
///
/// @return If @p namespace is @c NULL, this function returns a status with
/// code @c EVENTLOG_SOCKET_ERR_SYS and error code @c EINVAL.
///
/// @return If @p namespace is the builtin namespace, this function returns a
/// status with code @c EVENTLOG_SOCKET_ERR_SYS and error code @c EINVAL.
///
/// @return If a system error occurs, this function returns a status with code
/// @c EVENTLOG_SOCKET_ERR_SYS and the error code set to indicate the error.
///
/// @return If the binary was compiled without control support, this function
/// returns a status with code @c EVENTLOG_SOCKET_ERR_CTL_NOSUPPORT.
///
/// @note The @p command_handler should not use the functions from this header.
///
/// @since 0.1.1.1
EventlogSocketStatus eventlog_socket_control_register_command(
    EventlogSocketControlNamespace *namespace,
    EventlogSocketControlCommandId command_id,
    EventlogSocketControlCommandHandler *command_handler,
    const void *command_data);

#endif /* EVENGLOG_SOCKET_H */
