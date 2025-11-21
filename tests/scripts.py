"""Declarative control scripts for ghc-eventlog-socket tests."""
from __future__ import annotations

from typing import Callable
import time

CONTROL_MAGIC = b"GCTL"

class ControlContext:
    """Simple wrapper providing helper methods to send control commands."""

    def __init__(self, sender: Callable[[bytes], None]):
        self._sender = sender

    def send(self, data: bytes) -> None:
        self._sender(data)


def control_send_command(ctx: ControlContext, cmd_id: int) -> None:
    ctx.send(CONTROL_MAGIC + bytes([cmd_id & 0xFF]))


def start_heap_profiling(ctx: ControlContext) -> None:
    control_send_command(ctx, 0x01)


def stop_heap_profiling(ctx: ControlContext) -> None:
    control_send_command(ctx, 0x02)


def request_heap_profile(ctx: ControlContext) -> None:
    control_send_command(ctx, 0x03)


def sleep(ctx: ControlContext, seconds: float) -> None:
    time.sleep(seconds)

ControlScript = Callable[[ControlContext], None]



def script_junk_then_sample(ctx: ControlContext) -> None:
    # Send garbage bytes to ensure the control receiver sees protocol errors
    # while the writer continues streaming.
    ctx.send(b"JUNK!!!!")
    start_heap_profiling(ctx)


