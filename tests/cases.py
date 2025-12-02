"""Test case definitions for the Python harness."""
from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, List
from scripts import *


@dataclass
class EventlogAssertions:
    """Declarative expectations for ghc-events show output."""

    min_lines: int | None = None
    max_lines: int | None = None
    grep_includes: List[str] = field(default_factory=list)
    grep_excludes: List[str] = field(default_factory=list)

    def verify(self, path: Path, output: str) -> None:
        lines = output.splitlines()
        line_count = len(lines)
        if self.min_lines is not None and line_count < self.min_lines:
            raise AssertionError(
                f"{path}: expected at least {self.min_lines} lines, found {line_count}"
            )
        if self.max_lines is not None and line_count > self.max_lines:
            raise AssertionError(
                f"{path}: expected at most {self.max_lines} lines, found {line_count}"
            )
        for pattern in self.grep_includes:
            if not any(pattern in line for line in lines):
                raise AssertionError(f"{path}: missing pattern '{pattern}' in eventlog output")
        for pattern in self.grep_excludes:
            if any(pattern in line for line in lines):
                raise AssertionError(f"{path}: forbidden pattern '{pattern}' present in eventlog output")


@dataclass
class TestCase:
    description: str
    socket_type: str
    mode: str
    target: str
    args: List[str]
    control_script: ControlScript | None = None
    eventlog_assertions: EventlogAssertions | None = None

    def verify_eventlog(self, eventlog_path: Path, ghc_events_output: str | None) -> None:
        if self.eventlog_assertions is None:
            return
        if ghc_events_output is None:
            raise RuntimeError(
                f"{eventlog_path}: cannot verify eventlog without ghc-events output"
            )
        self.eventlog_assertions.verify(eventlog_path, ghc_events_output)


HEAP_PROF_SAMPLE_0_PATTERN = "heap prof sample 0"
HEAP_PROF_SAMPLE_1_PATTERN = "heap prof sample 1"
CUSTOM_COMMAND_PATTERN = "custom command handled"


def eventlog_assertions_no_start(
    *,
    min_lines: int | None = None,
    grep_includes: List[str] | None = None,
) -> EventlogAssertions:
    return EventlogAssertions(
        min_lines=min_lines,
        grep_includes=grep_includes or [],
        grep_excludes=[HEAP_PROF_SAMPLE_0_PATTERN],
    )


def start_heap_eventlog_assertions() -> EventlogAssertions:
    return EventlogAssertions(
        min_lines=1000,
        grep_includes=[HEAP_PROF_SAMPLE_0_PATTERN],
    )


def request_heap_eventlog_assertions() -> EventlogAssertions:
    return EventlogAssertions(
        min_lines=1000,
        grep_includes=[HEAP_PROF_SAMPLE_0_PATTERN],
        grep_excludes=[HEAP_PROF_SAMPLE_1_PATTERN],
    )


one_shot_num = "35"


@dataclass(frozen=True)
class ProgramScenario:
    target: str
    socket_type: str
    args: List[str]
    supports_reconnect: bool = True

    def args_for_mode(self, mode: str) -> List[str]:
        if mode == "normal":
            return self.args
        if mode == "reconnect":
            if not self.supports_reconnect:
                raise ValueError(f"{self.target} does not support reconnect mode")
            return ["--forever", *self.args]
        raise ValueError(f"unknown mode: {mode}")

    def modes(self) -> List[str]:
        if self.supports_reconnect:
            return ["normal", "reconnect"]
        return ["normal"]


@dataclass(frozen=True)
class ControlScenario:
    suffix: str
    control_script: ControlScript | None
    assertions_factory: Callable[[str], EventlogAssertions]


PROGRAM_SCENARIOS: List[ProgramScenario] = [
    ProgramScenario(
        target="fibber",
        socket_type="unix",
        args=[one_shot_num],
    ),
    ProgramScenario(
        target="fibber-tcp",
        socket_type="tcp",
        args=[one_shot_num],
    ),
    ProgramScenario(
        target="fibber-c-main",
        socket_type="unix",
        args=[one_shot_num],
    ),
    ProgramScenario(
        target="custom-command",
        socket_type="unix",
        args=[],
        supports_reconnect=False,
    ),
]


def _no_control_assertions(mode: str) -> EventlogAssertions:
    min_lines = 1000 if mode == "reconnect" else None
    return eventlog_assertions_no_start(min_lines=min_lines)


CONTROL_SCENARIOS: List[ControlScenario] = [
    ControlScenario("", None, _no_control_assertions),
    ControlScenario(
        ", start heap profiling",
        start_heap_profiling,
        lambda _mode: start_heap_eventlog_assertions(),
    ),
    ControlScenario(
        ", request heap sample",
        request_heap_profile,
        lambda _mode: request_heap_eventlog_assertions(),
    ),
    ControlScenario(
        ", junk control",
        script_junk_then_sample,
        lambda _mode: start_heap_eventlog_assertions(),
    ),
]


def _custom_command_assertions(_mode: str) -> EventlogAssertions:
    return EventlogAssertions(
        min_lines=None,
        grep_includes=[CUSTOM_COMMAND_PATTERN],
    )


CUSTOM_CONTROL_SCENARIOS: List[ControlScenario] = [
    ControlScenario(
        ", custom command",
        send_custom_command,
        _custom_command_assertions,
    ),
]


MODE_LABELS = {
    "normal": "finite",
    "reconnect": "forever, reconnect",
}


def default_cases() -> List[TestCase]:
    cases: List[TestCase] = []
    for program in PROGRAM_SCENARIOS:
        control_scenarios = CUSTOM_CONTROL_SCENARIOS if program.target == "custom-command" else CONTROL_SCENARIOS
        for mode in program.modes():
            args = program.args_for_mode(mode)
            mode_label = MODE_LABELS[mode]
            for scenario in control_scenarios:
                description = f"Test {program.target} ({mode_label}{scenario.suffix})"
                cases.append(
                    TestCase(
                        description,
                        program.socket_type,
                        mode,
                        program.target,
                        args,
                        control_script=scenario.control_script,
                        eventlog_assertions=scenario.assertions_factory(mode),
                    )
                )
    return cases
