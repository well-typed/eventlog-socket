#ifndef CUSTOM_COMMAND_HANDLER_H
#define CUSTOM_COMMAND_HANDLER_H

#include <stdint.h>

#define CUSTOM_COMMAND_NAMESPACE 0x01
#define CUSTOM_COMMAND_ID_PING 0x01

void custom_command_register(void);

#endif
