"""Declarative control scripts for ghc-eventlog-socket tests."""

from typing import Callable
import time

class ControlContext:
    """Simple wrapper providing helper methods to send control commands."""

    def __init__(self, sender: Callable[[bytes], None]):
        self._sender = sender

    def send(self, data: bytes) -> None:
        self._sender(data)


def control_send_command(
    ctx: ControlContext, namespace: bytes, command_id: int
) -> None:
    ctx.send(bytes([
        # MAGIC
        0xF0, 0x9E, 0x97, 0x8C,
        # PROTOCOL_VERSION
        0x00,
        # NAMESPACE_LEN
        len(namespace) & 0xFF,
        # NAMESPACE
        *namespace,
        # COMMAND_ID
        command_id,
    ]))


def start_heap_profiling(ctx: ControlContext) -> None:
    control_send_command(ctx, b"eventlog-socket", 0x00)


def stop_heap_profiling(ctx: ControlContext) -> None:
    control_send_command(ctx, b"eventlog-socket", 0x01)


def request_heap_profile(ctx: ControlContext) -> None:
    control_send_command(ctx, b"eventlog-socket", 0x02)


def send_custom_command(ctx: ControlContext) -> None:
    control_send_command(ctx, b"custom-command", 0x00)


def sleep(ctx: ControlContext, seconds: float) -> None:
    time.sleep(seconds)


ControlScript = Callable[[ControlContext], None]


def script_junk_then_sample(ctx: ControlContext) -> None:
    # Send garbage bytes to ensure the control receiver sees protocol errors
    # while the writer continues streaming.
    ctx.send(b"JUNK!!!!")
    start_heap_profiling(ctx)
