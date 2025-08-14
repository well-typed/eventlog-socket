#include <stdlib.h>
#include <ghcversion.h>
#include <rts/PosixSource.h>
#include <Rts.h>
#include <HsFFI.h>
#include <eventlog_socket.h>

int main (int argc, char *argv[])
{
    RtsConfig conf = defaultRtsConfig;

    // Set the eventlog writer:
    conf.eventlog_writer = &SocketEventLogWriter;

    // Enable RTS options:
    conf.rts_opts_enabled = RtsOptsAll;

    // Start the eventlog writer:
    const char* sock_path = getenv("GHC_EVENTLOG_SOCKET");
    eventlog_socket_init(sock_path);

    extern StgClosure ZCMain_main_closure;
    return hs_main(argc, argv, &ZCMain_main_closure, conf);
}
