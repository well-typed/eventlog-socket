"""Declarative control scripts for ghc-eventlog-socket tests."""
from typing import Callable, Dict
import struct
import time

CONTROL_MAGIC = b"GCTL"
CONTROL_NAMESPACE_CORE = 0
CUSTOM_COMMAND_NAMESPACE = 0x44454D4F
CUSTOM_COMMAND_ID = 0x01

class ControlContext:
    """Simple wrapper providing helper methods to send control commands."""

    def __init__(self, sender: Callable[[bytes], None]):
        self._sender = sender

    def send(self, data: bytes) -> None:
        self._sender(data)


def control_send_command(ctx: ControlContext, cmd_id: int, namespace: int = CONTROL_NAMESPACE_CORE) -> None:
    payload = CONTROL_MAGIC + struct.pack(">I", namespace & 0xFFFFFFFF) + bytes([cmd_id & 0xFF])
    ctx.send(payload)


def start_heap_profiling(ctx: ControlContext) -> None:
    control_send_command(ctx, 0x01)


def stop_heap_profiling(ctx: ControlContext) -> None:
    control_send_command(ctx, 0x02)


def request_heap_profile(ctx: ControlContext) -> None:
    control_send_command(ctx, 0x03)


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
