#include <stdlib.h>
#include <Rts.h>
#include <eventlog_socket.h>

int main (int argc, char *argv[])
{
    RtsConfig conf = defaultRtsConfig;

    // Set the eventlog writer:
    conf.eventlog_writer = &SocketEventLogWriter;

    // Enable RTS options:
    conf.rts_opts_enabled = RtsOptsAll;
    conf.rts_opts = "-l";

    // Start the eventlog writer:
    const char* sock_path = getenv("GHC_EVENTLOG_SOCKET");
    eventlog_socket_init_unix(sock_path);
    // Wait for the monitoring process to connect.
    eventlog_socket_wait();

    extern StgClosure ZCMain_main_closure;
    return hs_main(argc, argv, &ZCMain_main_closure, conf);
}
