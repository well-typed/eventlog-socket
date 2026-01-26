#include <Rts.h>
#include <eventlog_socket.h>
#include <stdlib.h>

// Get the main closure.
extern StgClosure ZCMain_main_closure;

int main(int argc, char *argv[]) {
  RtsConfig rts_config = defaultRtsConfig;

  // Set the eventlog writer:
  rts_config.eventlog_writer = &SocketEventLogWriter;

  // Enable RTS options:
  rts_config.rts_opts_enabled = RtsOptsAll;

  // If GHC_EVENTLOG_UNIX_PATH is set...
  eventlog_socket_t eventlog_socket = {0};
  if (eventlog_socket_from_env(&eventlog_socket)) {
    eventlog_socket_init(&eventlog_socket, NULL);
  }

  // Delegate to the helper that runs hs_main and the application closure.
  eventlog_socket_wrap_hs_main(argc, argv, rts_config, &ZCMain_main_closure);
}
