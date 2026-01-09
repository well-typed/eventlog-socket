#include <stdbool.h>
#include <stdio.h>

#include <Rts.h>
#include <eventlog_socket.h>

#include "custom-command-handler.h"

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

static void demo_command_handler(eventlog_socket_control_command_t command,
                                 void *user_data) {
  const char *label = (const char *)user_data;
  fprintf(stderr, "[custom-command] received namespace=0x%08x id=0x%02x (%s)\n",
          command.command_id, command.namespace_id,
          label != NULL ? label : "no label");
  emit_custom_user_msg();
}

void custom_command_register(void) {
  static const char *label = "demo ping";
  eventlog_socket_control_command_t ping_command = {
      .namespace_id = CUSTOM_COMMAND_NAMESPACE,
      .command_id = CUSTOM_COMMAND_ID_PING};
  bool ok = eventlog_socket_control_register_command(
      ping_command, demo_command_handler, (void *)label);
  if (!ok) {
    fprintf(stderr,
            "[custom-command] failed to register namespace=0x%08x id=0x%02x\n",
            CUSTOM_COMMAND_NAMESPACE, CUSTOM_COMMAND_ID_PING);
  }
}
