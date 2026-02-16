#include <stdlib.h>

#include <Rts.h>
#include <eventlog_socket.h>
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

#define CUSTOM_COMMAND_NAMESPACE "custom-command"
#define CUSTOM_COMMAND_PING 0
#define CUSTOM_COMMAND_PONG 1

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
  EventlogSocketControlNamespace *namespace =
      eventlog_socket_control_register_namespace(
          strlen(CUSTOM_COMMAND_NAMESPACE), CUSTOM_COMMAND_NAMESPACE);
  if (namespace == NULL) {
    DEBUG_ERROR("failed to register namespace '%s'", CUSTOM_COMMAND_NAMESPACE);
  }

  // Register the "ping" command.
  if (!eventlog_socket_control_register_command(namespace, CUSTOM_COMMAND_PING,
                                                handle_ping, (void *)NULL)) {
    DEBUG_ERROR("failed to register custom command 'ping' with namespace %p "
                "and id %02x",
                (void *)namespace, CUSTOM_COMMAND_PING);
    abort();
  }

  // Register the "pong" command.
  static const char *pong_label = "this is a label";
  if (!eventlog_socket_control_register_command(namespace, CUSTOM_COMMAND_PONG,
                                                handle_pong, pong_label)) {

    DEBUG_ERROR("failed to register custom command 'pong' with namespace %p "
                "and id %02x",
                (void *)namespace, CUSTOM_COMMAND_PONG);
    abort();
  }
}

int main(int argc, char *argv[]) {
  RtsConfig rts_config = defaultRtsConfig;
  rts_config.rts_opts_enabled = RtsOptsAll;
  rts_config.rts_opts = "-l";

  custom_command_init();

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

  extern StgClosure ZCMain_main_closure;
  eventlog_socket_wrap_hs_main(argc, argv, rts_config, &ZCMain_main_closure);
}
