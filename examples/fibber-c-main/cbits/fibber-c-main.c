#include <stdlib.h>
#include <Rts.h>
#include <eventlog_socket.h>

// Get the main closure.
extern StgClosure ZCMain_main_closure;

int main (int argc, char *argv[])
{
    RtsConfig conf = defaultRtsConfig;

    // Set the eventlog writer:
    conf.eventlog_writer = &SocketEventLogWriter;

    // Enable RTS options:
    conf.rts_opts_enabled = RtsOptsAll;

    // If GHC_EVENTLOG_UNIX_SOCKET is set...
    const char* sock_path = getenv("GHC_EVENTLOG_UNIX_SOCKET");
    if (sock_path != NULL) {
        // Start the eventlog writer.
        eventlog_socket_init_unix(sock_path);

        // Wait for the monitoring process to connect.
        eventlog_socket_wait();
    }

    // Delegate to the helper that runs hs_main and the application closure.
    eventlog_socket_hs_main(argc, argv, conf, &ZCMain_main_closure);
}
