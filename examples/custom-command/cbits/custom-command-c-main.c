#include <stdlib.h>

#include <Rts.h>
#include <eventlog_socket.h>
#include <string.h>

#include "custom-command-handler.h"

int main(int argc, char *argv[]) {
  RtsConfig conf = defaultRtsConfig;
  conf.eventlog_writer = &SocketEventLogWriter;
  conf.rts_opts_enabled = RtsOptsAll;
  conf.rts_opts = "-l";

  custom_command_register();

  eventlog_socket_init_from_env();

  eventlog_socket_wait();

  extern StgClosure ZCMain_main_closure;
  eventlog_socket_hs_main(argc, argv, conf, &ZCMain_main_closure);
}
