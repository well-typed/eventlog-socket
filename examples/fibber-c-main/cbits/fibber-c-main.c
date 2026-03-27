#include <Rts.h>
#include <eventlog_socket.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEBUG_ERROR(fmt, ...)                                                  \
  do {                                                                         \
    fprintf(stderr, "ERROR[%s|%d|%s]: " fmt "\n", __FILE__, __LINE__,          \
            __func__, __VA_ARGS__);                                            \
  } while (0)

// Get the main closure.
extern StgClosure ZCMain_main_closure;

/// Internal GHC function that writes a user marker to the eventlog.
extern void traceUserMarker(Capability *cap, char *msg);

/// Write a user marker to the eventlog.
static void write_user_marker(char *const marker) {
  Capability *cap = rts_lock();
  if (cap == NULL) {
    DEBUG_ERROR("%s", "could not acquire GHC RTS capability lock");
    return;
  }
  traceUserMarker(cap, marker);
  rts_unlock(cap);
}

// Hook for startEventLogging.
void handler_HookPostStartEventLogging(const void *arg) {
  (void)arg;
  write_user_marker("HookPostStartEventLogging fired.");
}

// Hook for endEventLogging.
void handler_HookPreEndEventLogging(const void *arg) {
  (void)arg;
  write_user_marker("HookPreEndEventLogging fired.");
}

int main(int argc, char *argv[]) {

  // Register start/end hooks.
  eventlog_socket_register_hook(EVENTLOG_SOCKET_HOOK_POST_START_EVENT_LOGGING,
                                &handler_HookPostStartEventLogging, NULL);
  eventlog_socket_register_hook(EVENTLOG_SOCKET_HOOK_PRE_END_EVENT_LOGGING,
                                &handler_HookPreEndEventLogging, NULL);

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

    // Delegate to the helper that runs hs_main and the application closure.
    eventlog_socket_wrap_hs_main(argc, argv, rts_config, &ZCMain_main_closure,
                                 &eventlog_socket_addr, &eventlog_socket_opts);
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
}
