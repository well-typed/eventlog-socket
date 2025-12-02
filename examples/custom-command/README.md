# Custom command example

This example keeps a simple workload busy while installing a custom control command that
prints to `stderr` whenever the monitoring process sends a message.

```
$ export CUSTOM_COMMAND_SOCKET=/tmp/custom-command.sock
$ cabal run custom-command
```

In another shell, connect to the socket and send the command with namespace
`0x44454d4f` (`"DEMO"`) and command id `0x01`:

```sh
python - <<'PY'
import socket, struct
sock = socket.socket(socket.AF_UNIX)
sock.connect("/tmp/custom-command.sock")
sock.sendall(b"GCTL" + struct.pack(">I", 0x44454d4F) + b"\x01")
sock.close()
PY
```

`custom-command` prints a message confirming that it handled the command via the new
`eventlog_socket_register_control_command` API.
