#include <Rts.h>
#include <eventlog_socket.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>

// Debug macros
#define DEBUG_INFO(fmt, ...)                                                   \
  do {                                                                         \
    fprintf(stderr, "INFO[%s|%d|%s]: " fmt "\n", __FILE__, __LINE__, __func__, \
            __VA_ARGS__);                                                      \
  } while (0)
#define DEBUG_ERROR(fmt, ...)                                                  \
  do {                                                                         \
    fprintf(stderr, "ERROR[%s|%d|%s]: " fmt "\n", __FILE__, __LINE__,          \
            __func__, __VA_ARGS__);                                            \
  } while (0)
#define DEBUG_ERRNO(msg)                                                       \
  do {                                                                         \
    perror("ERROR: " msg);                                                     \
  } while (0)

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
        exit((int)status_code); /* NOLINT */                                   \
      }                                                                        \
    }                                                                          \
  } while (0)

#define CUSTOM_COMMAND_NAMESPACE "custom-command"
#define CUSTOM_COMMAND_PING 1
#define CUSTOM_COMMAND_PONG 2

/// Internal GHC function that writes a user message to the eventlog.
extern void traceUserMsg(Capability *cap, char *msg);

/// Write a user message to the eventlog.
static void write_user_message(char *message) {
  Capability *cap = rts_lock();
  if (cap == NULL) {
    DEBUG_ERROR("%s", "could not acquire GHC RTS capability lock");
    return;
  }
  traceUserMsg(cap, message);
  rts_unlock(cap);
}

/// Handler for the ping command.
static void handle_ping(const EventlogSocketControlNamespace *const namespace,
                        const EventlogSocketControlCommandId command_id,
                        const void *user_data) {
  const char *label = user_data == NULL ? "-" : user_data;
  DEBUG_INFO(
      "[custom-command] handle_ping (namespace=%p, command=0x%02x, label=%s)",
      (void *)namespace, command_id, label);
  write_user_message("handled ping");
}

/// Handler for the pong command.
static void handle_pong(const EventlogSocketControlNamespace *const namespace,
                        const EventlogSocketControlCommandId command_id,
                        const void *user_data) {
  const char *label = user_data == NULL ? "-" : user_data;
  DEBUG_INFO(
      "[custom-command] handle_pong (namespace=%p, command=0x%02x, label=%s)",
      (void *)namespace, command_id, label);
  write_user_message("handled pong");
}

/// Register the "custom-command" namespace and the ping and pong custom
/// commands.
void custom_command_init(void) {
  // Register the "custom-command" namespace.
  EventlogSocketControlNamespace *namespace = NULL;
  EXIT_ON_ERROR(eventlog_socket_control_register_namespace(
      strlen(CUSTOM_COMMAND_NAMESPACE), CUSTOM_COMMAND_NAMESPACE, &namespace));

  // Register the "ping" command.
  EXIT_ON_ERROR(eventlog_socket_control_register_command(
      namespace, CUSTOM_COMMAND_PING, handle_ping, (void *)NULL));

  // Register the "pong" command.
  static const char *pong_label = "this is a label";
  EXIT_ON_ERROR(eventlog_socket_control_register_command(
      namespace, CUSTOM_COMMAND_PONG, handle_pong, pong_label));
}

// Get the main closure.
extern StgClosure ZCMain_main_closure;

int main(int argc, char *argv[]) {

  // Create a GHC RTS configuration object.
  RtsConfig rts_config = {0};
  memcpy(&rts_config, &defaultRtsConfig, sizeof(RtsConfig));
  rts_config.rts_opts_enabled = RtsOptsAll; // Enable all RTS options.
  rts_config.rts_opts = "-l";               // Enable binary eventlog.

  custom_command_init();

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
    // Delegate to the helper that runs hs_main and the application closure.
    break;
  }
  eventlog_socket_wrap_hs_main(argc, argv, rts_config, &ZCMain_main_closure);
}
