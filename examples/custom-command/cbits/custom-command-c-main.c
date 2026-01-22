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
static void
handle_ping(const eventlog_socket_control_namespace_t *const namespace,
            const eventlog_socket_control_command_id_t command_id,
            const void *user_data) {
  const char *label = user_data == NULL ? "-" : user_data;
  DEBUG_INFO(
      "[custom-command] handle_ping (namespace=%p, command=0x%02x, label=%s)",
      (void *)namespace, command_id, label);
  write_user_message("handled ping");
}

/// Handler for the pong command.
static void
handle_pong(const eventlog_socket_control_namespace_t *const namespace,
            const eventlog_socket_control_command_id_t command_id,
            const void *user_data) {
  const char *label = user_data == NULL ? "-" : user_data;
  DEBUG_INFO(
      "[custom-command] handle_pong (namespace=%p, command=0x%02x, label=%s)",
      (void *)namespace, command_id, label);
  write_user_message("handled pong");
}

/// Register the "custom-command" namespace and the ping and pong custom
/// commands.
void custom_command_initialize(void) {
  // Register the "custom-command" namespace.
  const eventlog_socket_control_namespace_t *namespace =
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
  RtsConfig conf = defaultRtsConfig;
  conf.eventlog_writer = &SocketEventLogWriter;
  conf.rts_opts_enabled = RtsOptsAll;
  conf.rts_opts = "-l";

  custom_command_initialize();

  eventlog_socket_init_from_env();

  eventlog_socket_wait();

  extern StgClosure ZCMain_main_closure;
  eventlog_socket_hs_main(argc, argv, conf, &ZCMain_main_closure);
}
