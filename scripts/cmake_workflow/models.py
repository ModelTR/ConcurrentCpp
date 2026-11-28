"""Data model shared by the generic verifier."""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from collections.abc import Callable, Sequence
    from pathlib import Path

    from .console import Style

type JsonScalar = str | int | float | bool | None
type JsonValue = JsonScalar | list[JsonValue] | dict[str, JsonValue]
type JsonObject = dict[str, JsonValue]


@dataclass(frozen=True)
class Issue:
    """A static validation finding."""

    severity: str
    scope: str
    message: str


class PresetError(Exception):
    """Raised when preset files cannot be loaded or resolved."""


@dataclass
class Plan:
    """Everything needed to verify one configure preset."""

    name: str
    effective: JsonObject
    cache: dict[str, str]
    origin_file: Path
    family: str
    arch: str | None
    build_presets: list[str]
    test_presets: list[str]
    build_effective: dict[str, JsonObject]
    test_effective: dict[str, JsonObject]
    build_dir: Path
    install_dir: Path
    log_dir: Path
    env: dict[str, str]
    notes: list[str]
    skip_reason: str | None
    workflow: str | None = None
    issues: list[Issue] = field(default_factory=list[Issue])

    @property
    def has_static_error(self) -> bool:
        """Return whether any static check failed for this preset."""
        return any(issue.severity == "error" for issue in self.issues)


@dataclass(frozen=True)
class StageResult:
    """The result of one configure, build or test command."""

    kind: str
    label: str
    status: str
    duration: float = 0.0
    detail: str = ""
    log: Path | None = None
    tail: str = ""


@dataclass
class PlanResult:
    """Aggregate result for one configure preset."""

    plan: Plan
    stages: list[StageResult] = field(default_factory=list[StageResult])

    @property
    def status(self) -> str:
        """Return ``pass``, ``fail`` or ``skip`` for this plan."""
        if self.plan.skip_reason:
            return "skip"
        if self.plan.has_static_error:
            return "fail"
        if any(stage.status == "fail" for stage in self.stages):
            return "fail"
        return "pass"


@dataclass(frozen=True)
class StageSpec:
    """One command to execute for a preset stage."""

    kind: str
    label: str
    command: tuple[str, ...]
    log_name: str
    validator: Callable[[], str | None] | None = None


@dataclass(frozen=True)
class RunSettings:
    """Execution settings shared by all stages of one preset."""

    cwd: Path
    env: dict[str, str]
    log_dir: Path
    timeout: float
    verbose: bool
    style: Style


@dataclass(frozen=True)
class ProcessOutcome:
    """Raw result of a subprocess invocation."""

    status: str
    detail: str
    duration: float
    tail: str


@dataclass(frozen=True)
class EnvironmentResult:
    """Prepared toolchain environment and human-readable notes."""

    env: dict[str, str] | None
    notes: list[str]
    skip_reason: str | None


@dataclass(frozen=True)
class Toolchain:
    """Resolved CMake and CTest executables."""

    cmake: str
    ctest: str


@dataclass(frozen=True)
class RunOptions:
    """Normalized command-line options used by the verification run."""

    source_dir: Path
    presets_file: Path
    binary_root: Path
    includes: tuple[str, ...]
    excludes: tuple[str, ...]
    stages: tuple[str, ...]
    jobs: int | None
    concurrency: int
    timeout: float
    clean: bool
    clean_after: bool
    fail_fast: bool
    verbose: bool
    cmake_bin: str
    ctest_bin: str
    report: Path | None
    list_only: bool
    check_only: bool
    dry_run: bool
    no_cpm_cache: bool


@dataclass(frozen=True)
class ReportData:
    """Inputs for the JSON verification report."""

    cmake_version: str
    stages: tuple[str, ...]
    source_dir: Path
    binary_root: Path
    results: Sequence[PlanResult]
    issues: Sequence[Issue]


@dataclass(frozen=True)
class HeaderData:
    """Values printed in the run header."""

    selected: int
    visible: int
    cmake_version: str
    cpm_cache: str | None
