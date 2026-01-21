"""Declarative control scripts for ghc-eventlog-socket tests."""

from typing import Callable
import time

CONTROL_MAGIC = bytes([0xF0, 0x9E, 0x97, 0x8C])
CONTROL_PROTOCOL_VERSION = bytes([0x00])
CONTROL_NAMESPACE_CORE = b"eventlog-socket"

CUSTOM_COMMAND_NAMESPACE = b"custom-command"
CUSTOM_COMMAND_ID = 0x00


class ControlContext:
    """Simple wrapper providing helper methods to send control commands."""

    def __init__(self, sender: Callable[[bytes], None]):
        self._sender = sender

    def send(self, data: bytes) -> None:
        self._sender(data)


def control_send_command(
    ctx: ControlContext, command_id: int, namespace: bytes = CONTROL_NAMESPACE_CORE
) -> None:
    payload = (
        CONTROL_MAGIC
        + +CONTROL_PROTOCOL_VERSION
        + bytes([len(namespace) & 0xFF])
        + namespace
        + bytes([command_id & 0xFF])
    )
    ctx.send(payload)


def start_heap_profiling(ctx: ControlContext) -> None:
    control_send_command(ctx, 0x00)


def stop_heap_profiling(ctx: ControlContext) -> None:
    control_send_command(ctx, 0x01)


def request_heap_profile(ctx: ControlContext) -> None:
    control_send_command(ctx, 0x02)


def send_custom_command(ctx: ControlContext) -> None:
    control_send_command(ctx, CUSTOM_COMMAND_ID, namespace=CUSTOM_COMMAND_NAMESPACE)


def sleep(ctx: ControlContext, seconds: float) -> None:
    time.sleep(seconds)


ControlScript = Callable[[ControlContext], None]


def script_junk_then_sample(ctx: ControlContext) -> None:
    # Send garbage bytes to ensure the control receiver sees protocol errors
    # while the writer continues streaming.
    ctx.send(b"JUNK!!!!")
    start_heap_profiling(ctx)
