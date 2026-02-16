#include <Rts.h>
#include <eventlog_socket.h>
#include <stdio.h>
#include <stdlib.h>

// Debug macros
#define DEBUG_ERROR(fmt, ...)                                                  \
  do {                                                                         \
    fprintf(stderr, "ERROR[%s|%d|%s]: " fmt "\n", __FILE__, __LINE__,          \
            __func__, __VA_ARGS__);                                            \
  } while (0)

// Get the main closure.
extern StgClosure ZCMain_main_closure;

int main(int argc, char *argv[]) {
  RtsConfig rts_config = defaultRtsConfig;

  // Enable RTS options:
  rts_config.rts_opts_enabled = RtsOptsAll;

  // Start with configuration from the environment...
  EventlogSocketAddr eventlog_socket_addr = {0};
  EventlogSocketOpts eventlog_socket_opts = {0};
  const EventlogSocketFromEnvStatus status =
      eventlog_socket_from_env(&eventlog_socket_addr, &eventlog_socket_opts);
  switch (status) {
  case EVENTLOG_SOCKET_FROM_ENV_OK:
    eventlog_socket_init(&eventlog_socket_addr, &eventlog_socket_opts);
    break;
  case EVENTLOG_SOCKET_FROM_ENV_INVAL:
    break;
  case EVENTLOG_SOCKET_FROM_ENV_UNIX_PATH_TOO_LONG:
    assert(eventlog_socket_addr.esa_tag == EVENTLOG_SOCKET_UNIX);
    DEBUG_ERROR("value of %s (%s) is too long", EVENTLOG_SOCKET_ENV_UNIX_PATH,
                eventlog_socket_addr.esa_unix_addr.esa_unix_path);
    break;
  case EVENTLOG_SOCKET_FROM_ENV_INET_HOST_MISSING:
    DEBUG_ERROR("no value given for %s", EVENTLOG_SOCKET_ENV_INET_HOST);
    break;
  case EVENTLOG_SOCKET_FROM_ENV_INET_PORT_MISSING:
    DEBUG_ERROR("no value given for %s", EVENTLOG_SOCKET_ENV_INET_PORT);
    break;
  }
  if (status != EVENTLOG_SOCKET_FROM_ENV_INVAL) {
    eventlog_socket_addr_free(&eventlog_socket_addr);
    eventlog_socket_opts_free(&eventlog_socket_opts);
  }

  // Delegate to the helper that runs hs_main and the application closure.
  eventlog_socket_wrap_hs_main(argc, argv, rts_config, &ZCMain_main_closure);
}
