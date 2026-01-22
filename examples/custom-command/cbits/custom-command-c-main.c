#include <stdlib.h>

#include <Rts.h>
#include <eventlog_socket.h>
#include <string.h>

#define CUSTOM_NAMESPACE "custom-command"
#define CUSTOM_COMMAND_ID_PING 0

extern void traceUserMsg(Capability *cap, char *msg);

static void emit_custom_user_msg(void) {
  static char message[] = "custom command handled";
  Capability *cap = rts_lock();
  if (cap == NULL) {
    return;
  }
  traceUserMsg(cap, message);
  rts_unlock(cap);
}

static void demo_command_handler(const eventlog_socket_control_command_t command,
                                 const void *user_data) {
  const char *label = (const char *)user_data;
  fprintf(stderr, "[custom-command] received namespace=%p id=0x%02x (%s)\n",
          (void *)command.namespace, command.command_id,
          label != NULL ? label : "no label");
  emit_custom_user_msg();
}

void custom_command_register(void) {
  static const char *label = "demo ping";

  // Register the custom namespace.
  const eventlog_socket_control_namespace_t *namespace =
      eventlog_socket_control_register_namespace(strlen(CUSTOM_NAMESPACE),
                                                 CUSTOM_NAMESPACE);
  if (namespace == NULL) {
    fprintf(stderr, "[custom-command] failed to register namespace=%s\n",
            CUSTOM_NAMESPACE);
    return;
  }

  // Register the ping command.
  const eventlog_socket_control_command_t ping_command = {
      .namespace = namespace, .command_id = CUSTOM_COMMAND_ID_PING};
  bool ok = eventlog_socket_control_register_command(
      ping_command, demo_command_handler, (void *)label);
  if (!ok) {
    fprintf(stderr,
            "[custom-command] failed to register custom command "
            "namespace=%p id=0x%02x\n",
            (void *)ping_command.namespace, ping_command.command_id);
    return;
  }
}

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
