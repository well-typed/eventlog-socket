#include <Rts.h>
#include <eventlog_socket.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// @brief If the status contains an error code, print a string describing the
/// error to stderr, and exit.
#define EXIT_ON_ERROR(eventlog_socket_status)                                  \
  do {                                                                         \
    const EventlogSocketStatus status = (eventlog_socket_status);              \
    const EventlogSocketStatusCode status_code = status.ess_status_code;       \
    if (status_code != EVENTLOG_SOCKET_OK) {                                   \
      char *strerr = eventlog_socket_strerror(status);                         \
      if (strerr != NULL) {                                                    \
        fprintf(stderr, "ERROR[%s|%d|%s]: %s\n", __FILE__, __LINE__, __func__, \
                strerr);                                                       \
        free(strerr);                                                          \
      }                                                                        \
      exit((int)status_code); /* NOLINT */                                     \
    }                                                                          \
  } while (0)

// Get the main closure.
extern StgClosure ZCMain_main_closure;

int main(int argc, char *argv[]) {

  // Create a GHC RTS configuration object.
  RtsConfig rts_config = {0};
  memcpy(&rts_config, &defaultRtsConfig, sizeof(RtsConfig));
  rts_config.rts_opts_enabled = RtsOptsAll; // Enable all RTS options.
  rts_config.rts_opts = "-l";               // Enable binary eventlog.

  // Read the socket address and options from the environment.
  EventlogSocketAddr eventlog_socket_addr = {0};
  EventlogSocketOpts eventlog_socket_opts = {0};
  const EventlogSocketStatus status =
      eventlog_socket_from_env(&eventlog_socket_addr, &eventlog_socket_opts);

  // Handle the return status.
  switch (status.ess_status_code) {
  case EVENTLOG_SOCKET_OK:
    EXIT_ON_ERROR(
        eventlog_socket_init(&eventlog_socket_addr, &eventlog_socket_opts));
    /*FALLTHROUGH*/
  case EVENTLOG_SOCKET_ERR_ENV_TOOLONG:
  case EVENTLOG_SOCKET_ERR_ENV_NOHOST:
  case EVENTLOG_SOCKET_ERR_ENV_NOPORT:
    // Free the memory held by socket address and options.
    eventlog_socket_addr_free(&eventlog_socket_addr);
    eventlog_socket_opts_free(&eventlog_socket_opts);
    break;
  default:
    break;
  }
  // Delegate to the helper that runs hs_main and the application closure.
  eventlog_socket_wrap_hs_main(argc, argv, rts_config, &ZCMain_main_closure);
}
