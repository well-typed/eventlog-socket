#include <Rts.h>
#include <eventlog_socket.h>
#include <stdlib.h>

// Get the main closure.
extern StgClosure ZCMain_main_closure;

int main(int argc, char *argv[]) {
  RtsConfig conf = defaultRtsConfig;

  // Set the eventlog writer:
  conf.eventlog_writer = &SocketEventLogWriter;

  // Enable RTS options:
  conf.rts_opts_enabled = RtsOptsAll;

  // If GHC_EVENTLOG_UNIX_SOCKET is set...
  eventlog_socket_init_from_env();

  // Delegate to the helper that runs hs_main and the application closure.
  eventlog_socket_hs_main(argc, argv, conf, &ZCMain_main_closure);
}
