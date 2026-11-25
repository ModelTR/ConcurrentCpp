#!/usr/bin/env python3
"""Verify every CMake configure preset with its build and test presets.

All presets in this project share ``${sourceDir}/build`` as their binary
directory, so they cannot be configured one after another in place.  This
script loads the preset files, flattens preset inheritance into a derived
presets file whose ``binaryDir``/``installDir`` point at an isolated directory
per preset, and then runs the *real* presets through ``--presets-file``:

    cmake --preset <configure> --presets-file <derived>
    cmake --build --preset <build> --presets-file <derived>
    ctest --preset <test> --presets-file <derived>

It also performs static validation: preset inheritance, configure/build/test
mappings, compiler and compiler-launcher availability, and a set of common
preset mistakes (missing build type, a C++ compiler in ``CMAKE_C_COMPILER``,
tests that silently pass when none are discovered, ...).

Examples:
    python scripts/verify_cmake_presets.py --list
    python scripts/verify_cmake_presets.py --check-only
    python scripts/verify_cmake_presets.py --presets "MSVC x64-debug"
    python scripts/verify_cmake_presets.py --presets "*release*" --stages configure,build
    python scripts/verify_cmake_presets.py --dry-run

Exit codes:
    0  every selected preset was verified (warnings and skips are allowed)
    1  at least one preset or static check failed
    2  the run could not be performed (bad arguments, no CMake, nothing verified)
"""

from __future__ import annotations

import argparse
import contextlib
import copy
import fnmatch
import json
import os
import platform
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from collections import OrderedDict, defaultdict, deque
from dataclasses import dataclass, field, replace
from datetime import UTC, datetime
from functools import partial
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from collections.abc import Callable, Sequence
    from typing import IO, ClassVar, TextIO, TypeGuard

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_PRESETS_FILE = REPO_ROOT / "CMakePresets.json"
DEFAULT_BINARY_ROOT = REPO_ROOT / "build" / "preset-verify"
FAMILIES = ("configurePresets", "buildPresets", "testPresets")
STAGE_ORDER = ("configure", "build", "test")
MIN_PRESET_VERSION = 2
CONFIGURE_FAMILY = "configurePresets"
BUILD_FAMILY = "buildPresets"
TEST_FAMILY = "testPresets"
MACRO_RE = re.compile(r"\$\{(sourceDir|sourceParentDir|fileDir|hostSystemName)\}")
COMPILER_VARIABLES = ("CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER")
LAUNCHER_VARIABLES = ("CMAKE_C_COMPILER_LAUNCHER", "CMAKE_CXX_COMPILER_LAUNCHER")

type JsonScalar = str | int | float | bool | None
type JsonValue = JsonScalar | list[JsonValue] | dict[str, JsonValue]
type JsonObject = dict[str, JsonValue]


# --------------------------------------------------------------------------
# JSON helpers
# --------------------------------------------------------------------------


def _pairs_hook(pairs: list[tuple[str, object]]) -> dict[str, object]:
    """Reject duplicate keys while loading preset JSON."""
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            message = f"duplicate JSON key {key!r}"
            raise ValueError(message)
        result[key] = value
    return result


def _is_object(value: object) -> TypeGuard[JsonObject]:
    """Return whether *value* is a JSON object."""
    return isinstance(value, dict)


def _is_list(value: object) -> TypeGuard[list[JsonValue]]:
    """Return whether *value* is a JSON array."""
    return isinstance(value, list)


def _object_field(obj: JsonObject, key: str) -> JsonObject:
    """Return a nested object field or an empty object."""
    value = obj.get(key)
    return value if _is_object(value) else {}


def _string_field(obj: JsonObject, key: str) -> str | None:
    """Return a string field or ``None``."""
    value = obj.get(key)
    return value if isinstance(value, str) else None


def _bool_field(obj: JsonObject, key: str) -> bool:
    """Return a boolean field, defaulting to ``False``."""
    value = obj.get(key)
    return value if isinstance(value, bool) else False


def _string_list(value: object) -> list[str]:
    """Normalize a string or array-of-strings into a list of strings."""
    if isinstance(value, str):
        return [value]
    if not _is_list(value):
        return []
    return [item for item in value if isinstance(item, str)]


def _display_value(value: JsonValue) -> str:
    """Render a JSON scalar or structure for diagnostics and cache values."""
    if isinstance(value, bool):
        return "ON" if value else "OFF"
    if isinstance(value, (int, float)):
        return str(value)
    if isinstance(value, str):
        return value
    return json.dumps(value)


def _macro_replacements(source_dir: Path, file_dir: Path) -> dict[str, str]:
    """Build the macro substitution table supported by preset files."""
    return {
        "sourceDir": source_dir.as_posix(),
        "sourceParentDir": source_dir.parent.as_posix(),
        "fileDir": file_dir.as_posix(),
        "hostSystemName": platform.system(),
    }


def _expand_text(text: str, source_dir: Path, file_dir: Path) -> str:
    replacements = _macro_replacements(source_dir, file_dir)
    return MACRO_RE.sub(lambda match: replacements.get(match.group(1), match.group(0)), text)


def _expand_macros(value: JsonValue, source_dir: Path, file_dir: Path) -> JsonValue:
    """Recursively expand path and host macros in a JSON value."""
    if isinstance(value, str):
        return _expand_text(value, source_dir, file_dir)
    if _is_list(value):
        return [_expand_macros(item, source_dir, file_dir) for item in value]
    if _is_object(value):
        return {key: _expand_macros(item, source_dir, file_dir) for key, item in value.items()}
    return value


# --------------------------------------------------------------------------
# Console helpers
# --------------------------------------------------------------------------


class Style:
    """Render terminal text with optional ANSI colours."""

    _CODES: ClassVar[dict[str, str]] = {
        "red": "31",
        "green": "32",
        "yellow": "33",
        "cyan": "36",
        "bold": "1",
        "dim": "2",
    }

    def __init__(self, mode: str) -> None:
        """Create a style for ``auto``, ``always`` or ``never`` colour output."""
        if mode == "always":
            self.enabled = True
        elif mode == "never":
            self.enabled = False
        else:
            self.enabled = sys.stdout.isatty() and os.environ.get("NO_COLOR") is None

    def __call__(self, text: str, *styles: str) -> str:
        """Render *text* with the requested ANSI styles."""
        if not self.enabled:
            return text
        prefix = "".join(f"\033[{self._CODES[name]}m" for name in styles)
        return f"{prefix}{text}\033[0m"


def status_text(status: str, style: Style) -> str:
    """Return a padded, coloured status label."""
    labels = {
        "pass": ("PASS", "green"),
        "fail": ("FAIL", "red"),
        "skip": ("SKIP", "yellow"),
        "blocked": ("BLOCKED", "yellow"),
        "not-run": ("NOT RUN", "dim"),
    }
    label, colour = labels.get(status, (status.upper(), "dim"))
    return style(f"{label:<7}", colour)


# --------------------------------------------------------------------------
# Data model
# --------------------------------------------------------------------------


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
    issues: list[Issue] = field(default_factory=list)

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
    stages: list[StageResult] = field(default_factory=list)

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

    presets_file: Path
    binary_root: Path
    includes: tuple[str, ...]
    excludes: tuple[str, ...]
    stages: tuple[str, ...]
    jobs: int | None
    timeout: float
    clean: bool
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
class AnalysisContext:
    """Immutable inputs shared while building plans."""

    catalog: PresetCatalog
    source_dir: Path
    binary_root: Path
    effective: dict[tuple[str, str], JsonObject]
    build_map: dict[str, list[str]]
    test_map: dict[str, list[str]]


@dataclass(frozen=True)
class ReportData:
    """Inputs for the JSON verification report."""

    cmake_version: str
    stages: tuple[str, ...]
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


@dataclass(frozen=True)
class Session:
    """A fully analyzed verification run."""

    options: RunOptions
    catalog: PresetCatalog
    plans: list[Plan]
    issues: list[Issue]
    cmake_version: str
    style: Style


# --------------------------------------------------------------------------
# Preset loading / resolution
# --------------------------------------------------------------------------


def _merge_presets(base: JsonObject, override: JsonObject) -> JsonObject:
    """Approximate CMake's preset inheritance merge (maps merge, arrays append)."""
    merged: JsonObject = copy.deepcopy(base)
    for key, value in override.items():
        if key == "inherits":
            continue
        current = merged.get(key)
        if _is_object(value) and _is_object(current):
            merged[key] = _merge_presets(current, value)
        elif _is_list(value) and _is_list(current):
            merged[key] = [*copy.deepcopy(current), *copy.deepcopy(value)]
        else:
            merged[key] = copy.deepcopy(value)
    return merged


class PresetCatalog:
    """Loaded preset families with inheritance resolution."""

    def __init__(self, source_dir: Path) -> None:
        """Create an empty catalog rooted at *source_dir*."""
        self.source_dir = source_dir
        self.families: dict[str, OrderedDict[str, JsonObject]] = {
            family: OrderedDict() for family in FAMILIES
        }
        self.origins: dict[tuple[str, str], Path] = {}
        self.version: int | None = None
        self.cmake_minimum_required: JsonObject | None = None
        self.issues: list[Issue] = []
        self._resolved: dict[tuple[str, str], JsonObject] = {}
        self._resolving: set[tuple[str, str]] = set()
        self._loaded_files: set[Path] = set()
        self._loading_files: set[Path] = set()

    def load(self, preset_file: Path) -> None:
        """Load *preset_file*, its includes, and an adjacent user presets file."""
        preset_file = preset_file.resolve()
        if not preset_file.is_file():
            message = f"preset file not found: {preset_file}"
            raise PresetError(message)

        main_data = self._read_json(preset_file)
        version = main_data.get("version")
        if isinstance(version, int):
            self.version = version
        minimum_value = main_data.get("cmakeMinimumRequired")
        self.cmake_minimum_required = (
            copy.deepcopy(minimum_value) if _is_object(minimum_value) else None
        )

        self._load_file_with_includes(preset_file, main_data)

        user_file = preset_file.parent / "CMakeUserPresets.json"
        if user_file.is_file() and user_file.resolve() != preset_file:
            try:
                user_data = self._read_json(user_file)
            except PresetError as exc:
                self.issues.append(Issue("warning", str(user_file), str(exc)))
            else:
                self._load_file_with_includes(user_file, user_data)

    def _load_file_with_includes(self, path: Path, data: JsonObject) -> None:
        if path in self._loaded_files or path in self._loading_files:
            return
        self._loading_files.add(path)
        try:
            for include in _string_list(data.get("include", [])):
                candidate = self._resolve_include(include, path)
                if candidate in self._loaded_files:
                    continue
                try:
                    included_data = self._read_json(candidate)
                except PresetError as exc:
                    message = f"{path}: {exc}"
                    raise PresetError(message) from exc
                self._load_file_with_includes(candidate, included_data)
            self._load_one(path, data)
        finally:
            self._loading_files.discard(path)

    def _resolve_include(self, include: str, path: Path) -> Path:
        candidate = Path(_expand_text(include, self.source_dir, path.parent))
        if not candidate.is_absolute():
            candidate = (path.parent / candidate).resolve()
        if not candidate.is_file():
            message = f"{path}: included preset file not found: {candidate}"
            raise PresetError(message)
        return candidate

    def _load_one(self, path: Path, data: JsonObject) -> None:
        self._loaded_files.add(path)
        for family in FAMILIES:
            entries_value = data.get(family, [])
            if entries_value is None:
                continue
            if not _is_list(entries_value):
                message = f"{path}: {family} must be an array"
                raise PresetError(message)
            for entry_value in entries_value:
                if not _is_object(entry_value):
                    message = f"{path}: {family} entries must be objects"
                    raise PresetError(message)
                self._register_preset(family, path, entry_value)

    def _register_preset(self, family: str, path: Path, entry: JsonObject) -> None:
        name = _string_field(entry, "name")
        if not name:
            message = f"{path}: a {family} preset has no name"
            raise PresetError(message)
        origin = self.origins.get((family, name))
        if name in self.families[family]:
            severity = "error" if origin == path else "warning"
            self.issues.append(
                Issue(
                    severity,
                    name,
                    f"duplicate {family} preset name; the definition in {path} replaces {origin}",
                )
            )
        self.families[family][name] = entry
        self.origins[(family, name)] = path

    def _read_json(self, path: Path) -> JsonObject:
        try:
            text = path.read_text(encoding="utf-8-sig")
        except OSError as exc:
            message = f"cannot read {path}: {exc}"
            raise PresetError(message) from exc
        try:
            data = json.loads(text, object_pairs_hook=_pairs_hook)
        except (json.JSONDecodeError, ValueError) as exc:
            message = f"invalid JSON in {path}: {exc}"
            raise PresetError(message) from exc
        if not _is_object(data):
            message = f"{path}: top-level JSON value must be an object"
            raise PresetError(message)
        return data

    def resolve(self, family: str, name: str) -> JsonObject:
        """Return the fully merged definition of a preset."""
        key = (family, name)
        if key in self._resolved:
            return copy.deepcopy(self._resolved[key])
        if key in self._resolving:
            message = f"inheritance cycle while resolving {family} preset {name!r}"
            raise PresetError(message)
        raw = self.families[family].get(name)
        if raw is None:
            message = f"unknown {family} preset {name!r}"
            raise PresetError(message)
        self._resolving.add(key)
        try:
            merged: JsonObject = {}
            for parent in _string_list(raw.get("inherits", [])):
                inherited = self.resolve(family, parent)
                # CMake does not inherit the `hidden` flag; it only applies to
                # the preset that declares it.
                inherited.pop("hidden", None)
                merged = _merge_presets(merged, inherited)
            own: JsonObject = {key_: value for key_, value in raw.items() if key_ != "inherits"}
            merged = _merge_presets(merged, own)
            merged["name"] = name
            merged.pop("inherits", None)
            self._resolved[key] = merged
            return copy.deepcopy(merged)
        finally:
            self._resolving.discard(key)

    def origin(self, family: str, name: str) -> Path:
        """Return the file that defined a preset."""
        return self.origins.get((family, name), self.source_dir / "CMakePresets.json")

    def visible_names(self, family: str) -> list[str]:
        """Return non-hidden preset names in file order."""
        names: list[str] = []
        for name in self.families[family]:
            try:
                resolved = self.resolve(family, name)
            except PresetError:
                continue
            if not _bool_field(resolved, "hidden"):
                names.append(name)
        return names


# --------------------------------------------------------------------------
# Conditions
# --------------------------------------------------------------------------


def _condition_text(condition: JsonObject, key: str, source_dir: Path, file_dir: Path) -> str:
    return _expand_text(_display_value(condition.get(key, "")), source_dir, file_dir)


def _eval_comparison(
    condition: JsonObject,
    ctype: str,
    source_dir: Path,
    file_dir: Path,
) -> bool:
    lhs = _condition_text(condition, "lhs", source_dir, file_dir)
    rhs = _condition_text(condition, "rhs", source_dir, file_dir)
    if ctype == "equals":
        return lhs == rhs
    if ctype == "notEquals":
        return lhs != rhs
    try:
        return re.search(rhs, lhs) is not None
    except re.error:
        return False


def _eval_in_list(condition: JsonObject, source_dir: Path, file_dir: Path) -> bool:
    lhs = _condition_text(condition, "lhs", source_dir, file_dir)
    items_value = condition.get("list", [])
    if not _is_list(items_value):
        return False
    return any(
        lhs == _expand_text(_display_value(item), source_dir, file_dir) for item in items_value
    )


def _eval_logical(
    condition: JsonObject,
    ctype: str,
    source_dir: Path,
    file_dir: Path,
) -> bool:
    if ctype == "not":
        return not eval_condition(condition.get("condition"), source_dir, file_dir)
    conditions_value = condition.get("conditions", [])
    if not _is_list(conditions_value):
        return False
    results = (eval_condition(item, source_dir, file_dir) for item in conditions_value)
    return any(results) if ctype == "anyOf" else all(results)


def eval_condition(
    condition: JsonValue | None,
    source_dir: Path,
    file_dir: Path | None = None,
) -> bool:
    """Evaluate a CMake preset condition on this host."""
    if condition is None:
        return True
    if not _is_object(condition):
        return False
    resolved_file_dir = file_dir or source_dir
    ctype = _string_field(condition, "type") or ""
    if ctype in {"equals", "notEquals", "matches"}:
        return _eval_comparison(condition, ctype, source_dir, resolved_file_dir)
    if ctype == "inList":
        return _eval_in_list(condition, source_dir, resolved_file_dir)
    if ctype in {"anyOf", "allOf", "not"}:
        return _eval_logical(condition, ctype, source_dir, resolved_file_dir)
    return False


# --------------------------------------------------------------------------
# Toolchain and environment discovery
# --------------------------------------------------------------------------


def _program_stem(value: str | None) -> str:
    """Return the lower-case executable name without a Windows extension."""
    text = (value or "").strip().strip('"').replace("\\", "/").rstrip("/")
    name = text.rsplit("/", 1)[-1].lower()
    return name.removesuffix(".exe")


def program_available(value: str | None, env: dict[str, str]) -> bool:
    """Return whether a program from a preset cache variable is available."""
    text = (value or "").strip()
    if not text:
        return True
    first = text.split(";")[0].strip().strip('"')
    if Path(first).is_absolute() or "/" in first or "\\" in first:
        return Path(first).is_file()
    return shutil.which(first, path=env.get("PATH", "")) is not None


def toolchain_family(cache: dict[str, str]) -> str:
    """Classify the toolchain family from the preset or the compilers."""
    kind = cache.get("MSYS2_UCRT64_COMPILER_KIND")
    if kind in {"clang", "gcc"}:
        return kind
    compiler = cache.get("CMAKE_CXX_COMPILER") or cache.get("CMAKE_C_COMPILER") or ""
    stem = _program_stem(compiler)
    if stem.startswith("clang"):
        return "clang"
    if stem in {"gcc", "g++", "cc", "c++"} or stem.startswith(("gcc", "g++")):
        return "gcc"
    if stem in {"cl", "clang-cl"}:
        return "msvc"
    return "unknown"


def find_msys2_ucrt64() -> Path | None:
    """Locate the MSYS2 UCRT64 ``bin`` directory."""
    roots: list[Path] = []
    env_root = os.environ.get("MSYS2_ROOT")
    if env_root:
        roots.append(Path(env_root))
    roots.extend(
        Path(candidate)
        for candidate in (
            r"C:\msys64",
            r"C:\msys2",
            r"C:\tools\msys64",
            r"C:\tools\msys2",
        )
    )
    for root in roots:
        binary_dir = root / "ucrt64" / "bin"
        if (binary_dir / "clang++.exe").is_file() or (binary_dir / "g++.exe").is_file():
            return binary_dir
    for executable in ("clang++.exe", "g++.exe"):
        found = shutil.which(executable)
        if found:
            binary_dir = Path(found).parent
            if binary_dir.name.lower() == "bin" and binary_dir.parent.name.lower() == "ucrt64":
                return binary_dir
    return None


def find_vswhere() -> Path | None:
    """Locate ``vswhere.exe`` from the Visual Studio installer."""
    candidates: list[Path] = []
    program_files_x86 = os.environ.get("PROGRAMFILES(X86)")
    if program_files_x86:
        candidates.append(
            Path(program_files_x86) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
        )
    found = shutil.which("vswhere.exe")
    if found:
        candidates.append(Path(found))
    return next((candidate for candidate in candidates if candidate.is_file()), None)


def _console_encoding() -> str:
    encoding = getattr(sys.stdout, "encoding", None)
    return encoding or "utf-8"


def _run_vswhere(vswhere: Path, arguments: Sequence[str]) -> list[str]:
    with contextlib.suppress(OSError, subprocess.SubprocessError):
        result = subprocess.run(
            [str(vswhere), *arguments],
            capture_output=True,
            text=True,
            encoding=_console_encoding(),
            errors="replace",
            timeout=60,
            check=False,
        )
        if result.returncode == 0:
            return result.stdout.splitlines()
    return []


def _vcvarsall_from_vswhere(vswhere: Path) -> Path | None:
    lines = _run_vswhere(
        vswhere,
        [
            "-latest",
            "-products",
            "*",
            "-requires",
            "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
            "-find",
            r"VC\Auxiliary\Build\vcvarsall.bat",
        ],
    )
    for line in lines:
        candidate = Path(line.strip())
        if candidate.is_file():
            return candidate
    lines = _run_vswhere(
        vswhere,
        [
            "-latest",
            "-products",
            "*",
            "-requires",
            "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
            "-property",
            "installationPath",
        ],
    )
    for line in lines:
        candidate = Path(line.strip()) / "VC" / "Auxiliary" / "Build" / "vcvarsall.bat"
        if candidate.is_file():
            return candidate
    return None


def _visual_studio_roots() -> list[Path]:
    roots: list[Path] = []
    for variable in ("VSINSTALLDIR", "PROGRAMFILES", "PROGRAMFILES(X86)"):
        value = os.environ.get(variable)
        if not value:
            continue
        base = Path(value)
        roots.append(base if variable == "VSINSTALLDIR" else base / "Microsoft Visual Studio")
    return roots


def _vcvarsall_from_roots() -> Path | None:
    for root in _visual_studio_roots():
        if not root.is_dir():
            continue
        for pattern in (
            "*/VC/Auxiliary/Build/vcvarsall.bat",
            "*/*/VC/Auxiliary/Build/vcvarsall.bat",
        ):
            for candidate in sorted(root.glob(pattern), reverse=True):
                if candidate.is_file():
                    return candidate
    return None


def find_vcvarsall() -> Path | None:
    """Locate ``vcvarsall.bat`` for the installed Visual Studio."""
    vswhere = find_vswhere()
    if vswhere is not None:
        found = _vcvarsall_from_vswhere(vswhere)
        if found is not None:
            return found
    return _vcvarsall_from_roots()


def _merge_environment(base: dict[str, str], extra: dict[str, str]) -> dict[str, str]:
    result = dict(base)
    index = {key.upper(): key for key in result}
    for key, value in extra.items():
        if key.startswith("="):
            continue
        out_key = key.upper() if os.name == "nt" else key
        previous = index.get(out_key.upper())
        if previous is not None and previous != out_key:
            del result[previous]
        index[out_key.upper()] = out_key
        result[out_key] = value
    return result


def _parse_environment(output: str, base: dict[str, str]) -> dict[str, str]:
    extra: dict[str, str] = {}
    for line in output.splitlines():
        if "=" not in line or line.startswith("="):
            continue
        name, value = line.split("=", 1)
        extra[name] = value
    return _merge_environment(base, extra)


def _run_vcvarsall(vcvarsall: Path, arch_arg: str) -> tuple[dict[str, str] | None, str | None]:
    handle, wrapper_name = tempfile.mkstemp(prefix="verify-presets-vcvars-", suffix=".cmd")
    os.close(handle)
    wrapper = Path(wrapper_name)
    try:
        try:
            wrapper.write_text(
                f'@echo off\r\ncall "{vcvarsall}" {arch_arg} >nul\r\nset\r\n',
                encoding="mbcs" if os.name == "nt" else "utf-8",
            )
            result = subprocess.run(
                ["cmd.exe", "/d", "/c", str(wrapper)],
                capture_output=True,
                text=True,
                encoding=_console_encoding(),
                errors="replace",
                timeout=180,
                check=False,
            )
        except (OSError, subprocess.SubprocessError) as exc:
            return None, f"failed to run vcvarsall.bat: {exc}"
    finally:
        with contextlib.suppress(OSError):
            wrapper.unlink()
    if result.returncode != 0:
        return None, f"vcvarsall.bat {arch_arg} failed with exit code {result.returncode}"
    return _parse_environment(result.stdout, os.environ.copy()), None


def _prepare_msvc(arch: str | None) -> EnvironmentResult:
    if os.name != "nt":
        return EnvironmentResult(None, [], "MSVC presets require Windows")
    vcvarsall = find_vcvarsall()
    if vcvarsall is None:
        return EnvironmentResult(
            None, [], "Visual Studio C++ build tools (vcvarsall.bat) were not found"
        )
    arch_arg = arch if arch in {"x64", "x86", "arm64"} else "x64"
    env, reason = _run_vcvarsall(vcvarsall, arch_arg)
    if env is None:
        return EnvironmentResult(None, [], reason)
    return EnvironmentResult(
        env,
        [f"MSVC {arch_arg} developer environment ({vcvarsall.name})"],
        None,
    )


def _prepare_msys2(arch: str | None) -> EnvironmentResult:
    binary_dir = find_msys2_ucrt64()
    if binary_dir is None:
        return EnvironmentResult(
            None,
            [],
            "MSYS2 UCRT64 toolchain not found; set MSYS2_ROOT or add "
            r"<msys2>\ucrt64\bin to PATH",
        )
    env = os.environ.copy()
    env["PATH"] = str(binary_dir) + os.pathsep + env.get("PATH", "")
    env["MSYSTEM"] = "UCRT64"
    notes = [f"MSYS2 UCRT64 environment ({binary_dir})"]
    if arch is not None and arch.lower() == "x86":
        notes.append(
            "preset targets x86, but only the 64-bit UCRT64 toolchain is "
            "provisioned; the build will target x64"
        )
    return EnvironmentResult(env, notes, None)


_ENV_CACHE: dict[tuple[str, str], EnvironmentResult] = {}


def _copy_environment_result(result: EnvironmentResult) -> EnvironmentResult:
    return EnvironmentResult(
        env=dict(result.env) if result.env is not None else None,
        notes=list(result.notes),
        skip_reason=result.skip_reason,
    )


def prepare_environment(family: str, arch: str | None) -> EnvironmentResult:
    """Prepare the compiler environment for a toolchain family."""
    key = (family, arch or "")
    cached = _ENV_CACHE.get(key)
    if cached is not None:
        return _copy_environment_result(cached)
    if family == "msvc":
        result = _prepare_msvc(arch)
    elif family in {"clang", "gcc"}:
        result = _prepare_msys2(arch)
    else:
        result = EnvironmentResult(
            None, [], "cannot determine the toolchain family from the compilers"
        )
    _ENV_CACHE[key] = result
    return _copy_environment_result(result)


# --------------------------------------------------------------------------
# Static analysis and planning
# --------------------------------------------------------------------------


def _is_single_config(generator: str | None) -> bool:
    if not generator:
        return False
    lowered = generator.lower()
    return (
        "multi-config" not in lowered and "visual studio" not in lowered and "xcode" not in lowered
    )


def _slug(text: str) -> str:
    slug = re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")
    return slug or "preset"


def _matches(name: str, includes: Sequence[str], excludes: Sequence[str]) -> bool:
    if includes and not any(fnmatch.fnmatch(name, pattern) for pattern in includes):
        return False
    return not (excludes and any(fnmatch.fnmatch(name, pattern) for pattern in excludes))


def _version_issues(catalog: PresetCatalog) -> list[Issue]:
    if catalog.version is None:
        return [Issue("warning", "CMakePresets.json", "preset file has no version field")]
    if catalog.version < MIN_PRESET_VERSION:
        return [
            Issue(
                "warning",
                "CMakePresets.json",
                f"unsupported preset file version {catalog.version}",
            )
        ]
    return []


def _resolve_all(catalog: PresetCatalog) -> tuple[dict[tuple[str, str], JsonObject], list[Issue]]:
    effective: dict[tuple[str, str], JsonObject] = {}
    issues: list[Issue] = []
    for family in FAMILIES:
        for name in catalog.families[family]:
            try:
                effective[(family, name)] = catalog.resolve(family, name)
            except PresetError as exc:
                issues.append(Issue("error", name, str(exc)))
    return effective, issues


def _build_mappings(
    catalog: PresetCatalog, effective: dict[tuple[str, str], JsonObject]
) -> tuple[dict[str, list[str]], dict[str, list[str]], list[Issue]]:
    issues: list[Issue] = []
    build_map: dict[str, list[str]] = defaultdict(list)
    test_map: dict[str, list[str]] = defaultdict(list)
    for family, mapping in ((BUILD_FAMILY, build_map), (TEST_FAMILY, test_map)):
        for name in catalog.families[family]:
            preset = effective.get((family, name))
            if preset is None or _bool_field(preset, "hidden"):
                continue
            target = _string_field(preset, "configurePreset")
            if not target:
                issues.append(Issue("error", name, f"{family} preset does not set configurePreset"))
                continue
            if target not in catalog.families[CONFIGURE_FAMILY]:
                issues.append(
                    Issue("error", name, f"references unknown configure preset {target!r}")
                )
                continue
            target_preset = effective.get((CONFIGURE_FAMILY, target))
            if target_preset is None:
                continue
            if _bool_field(target_preset, "hidden"):
                issues.append(
                    Issue("error", name, f"references hidden configure preset {target!r}")
                )
                continue
            mapping[target].append(name)
    return build_map, test_map, issues


def _cache_values(preset: JsonObject) -> dict[str, str]:
    cache_raw = _object_field(preset, "cacheVariables")
    return {key: _display_value(value) for key, value in cache_raw.items()}


def _architecture(preset: JsonObject) -> str | None:
    return _string_field(_object_field(preset, "architecture"), "value")


def _compiler_issues(plan: Plan) -> list[Issue]:
    issues: list[Issue] = []
    c_compiler = plan.cache.get("CMAKE_C_COMPILER")
    if c_compiler and "++" in _program_stem(c_compiler):
        issues.append(
            Issue(
                "error",
                plan.name,
                f"CMAKE_C_COMPILER is {c_compiler!r}, which is a C++ compiler; "
                "CMake rejects this, use the matching C driver instead",
            )
        )
    cxx_compiler = plan.cache.get("CMAKE_CXX_COMPILER")
    if cxx_compiler and _program_stem(cxx_compiler) in {"gcc", "cc", "clang"}:
        issues.append(
            Issue(
                "error",
                plan.name,
                f"CMAKE_CXX_COMPILER is {cxx_compiler!r}, which is a C compiler; "
                "CMake rejects this, use the matching C++ driver instead",
            )
        )
    return issues


def _test_preset_issues(plan: Plan) -> list[Issue]:
    issues: list[Issue] = []
    for test_name in plan.test_presets:
        execution = _object_field(plan.test_effective[test_name], "execution")
        if _string_field(execution, "noTestsAction") != "error":
            issues.append(
                Issue(
                    "warning",
                    test_name,
                    "test preset does not set execution.noTestsAction=error; "
                    "a configuration with zero discovered tests would pass silently",
                )
            )
    return issues


def _structure_issues(plan: Plan) -> list[Issue]:
    preset = plan.effective
    issues: list[Issue] = []
    generator = _string_field(preset, "generator")
    if not generator:
        issues.append(Issue("error", plan.name, "configure preset does not set a generator"))
    if not _string_field(preset, "binaryDir"):
        issues.append(Issue("error", plan.name, "configure preset does not set binaryDir"))
    if not plan.build_presets:
        issues.append(Issue("error", plan.name, "no build preset references this configure preset"))
    if not plan.test_presets:
        issues.append(Issue("error", plan.name, "no test preset references this configure preset"))
    if _is_single_config(generator) and "CMAKE_BUILD_TYPE" not in plan.cache:
        issues.append(
            Issue(
                "warning",
                plan.name,
                f'single-config generator "{generator}" has no CMAKE_BUILD_TYPE',
            )
        )
    issues.extend(_compiler_issues(plan))
    if plan.family in {"clang", "gcc"} and plan.arch is not None and plan.arch.lower() == "x86":
        issues.append(
            Issue(
                "warning",
                plan.name,
                "preset targets x86, but MSYS2 UCRT64 provides a 64-bit "
                "toolchain; architecture is not enforced with Ninja",
            )
        )
    return issues


def _missing_program_reason(cache: dict[str, str], env: dict[str, str]) -> str | None:
    for variable in (*COMPILER_VARIABLES, *LAUNCHER_VARIABLES):
        value = cache.get(variable)
        if value and not program_available(value, env):
            return f"{variable}={value!r} is not available"
    return None


def _plan_paths(binary_root: Path, slug: str) -> tuple[Path, Path, Path]:
    return (
        binary_root / slug,
        binary_root / "install" / slug,
        binary_root / "logs" / slug,
    )


def _unique_slug(name: str, used: set[str]) -> str:
    candidate = _slug(name)
    base = candidate
    counter = 2
    while candidate in used:
        candidate = f"{base}-{counter}"
        counter += 1
    used.add(candidate)
    return candidate


def _create_plan(context: AnalysisContext, name: str, used_slugs: set[str]) -> Plan:
    preset = context.effective[(CONFIGURE_FAMILY, name)]
    cache = _cache_values(preset)
    build_dir, install_dir, log_dir = _plan_paths(
        context.binary_root, _unique_slug(name, used_slugs)
    )
    build_presets = context.build_map.get(name, [])
    test_presets = context.test_map.get(name, [])
    plan = Plan(
        name=name,
        effective=preset,
        cache=cache,
        origin_file=context.catalog.origin(CONFIGURE_FAMILY, name),
        family=toolchain_family(cache),
        arch=_architecture(preset),
        build_presets=build_presets,
        test_presets=test_presets,
        build_effective={
            build_name: context.effective[(BUILD_FAMILY, build_name)]
            for build_name in build_presets
        },
        test_effective={
            test_name: context.effective[(TEST_FAMILY, test_name)] for test_name in test_presets
        },
        build_dir=build_dir,
        install_dir=install_dir,
        log_dir=log_dir,
        env={},
        notes=[],
        skip_reason=None,
    )
    condition_ok = eval_condition(
        preset.get("condition"),
        context.source_dir,
        context.catalog.origin(CONFIGURE_FAMILY, name).parent,
    )
    if not condition_ok:
        plan.skip_reason = "preset condition is false on this host"
        return plan
    plan.issues.extend(_structure_issues(plan))
    plan.issues.extend(_test_preset_issues(plan))
    environment = prepare_environment(plan.family, plan.arch)
    plan.notes.extend(environment.notes)
    if environment.env is None:
        plan.skip_reason = environment.skip_reason or "toolchain is unavailable"
        return plan
    plan.env = environment.env
    plan.skip_reason = _missing_program_reason(plan.cache, plan.env)
    return plan


def analyse(
    catalog: PresetCatalog,
    source_dir: Path,
    includes: Sequence[str],
    excludes: Sequence[str],
    binary_root: Path,
) -> tuple[list[Plan], list[Issue]]:
    """Validate the preset matrix and build one plan per selected preset."""
    issues: list[Issue] = [*catalog.issues, *_version_issues(catalog)]
    effective, resolve_issues = _resolve_all(catalog)
    issues.extend(resolve_issues)
    build_map, test_map, mapping_issues = _build_mappings(catalog, effective)
    issues.extend(mapping_issues)
    visible = [
        name
        for name in catalog.visible_names(CONFIGURE_FAMILY)
        if _matches(name, includes, excludes)
    ]
    context = AnalysisContext(
        catalog=catalog,
        source_dir=source_dir,
        binary_root=binary_root,
        effective=effective,
        build_map=build_map,
        test_map=test_map,
    )
    used_slugs: set[str] = set()
    plans: list[Plan] = []
    for name in visible:
        plan = _create_plan(context, name, used_slugs)
        plans.append(plan)
        issues.extend(plan.issues)
    return plans, issues


# --------------------------------------------------------------------------
# Derived presets file (isolation without touching the shared build dir)
# --------------------------------------------------------------------------


def _flattened_preset(preset: JsonObject, origin_file: Path, source_dir: Path) -> JsonObject:
    flattened = _expand_macros(copy.deepcopy(preset), source_dir, origin_file.parent)
    if not _is_object(flattened):
        message = "internal error: flattened preset is not an object"
        raise PresetError(message)
    flattened.pop("inherits", None)
    flattened.pop("hidden", None)
    return flattened


def write_derived_presets(
    plans: Sequence[Plan], catalog: PresetCatalog, source_dir: Path, path: Path
) -> None:
    """Write flattened presets with isolated binary/install directories."""
    configure_presets: list[JsonValue] = []
    build_presets: list[JsonValue] = []
    test_presets: list[JsonValue] = []
    seen_build: set[str] = set()
    seen_test: set[str] = set()
    for plan in plans:
        if plan.skip_reason:
            continue
        configure = _flattened_preset(plan.effective, plan.origin_file, source_dir)
        configure["binaryDir"] = plan.build_dir.as_posix()
        if "installDir" in configure:
            configure["installDir"] = plan.install_dir.as_posix()
        configure_presets.append(configure)
        for name in plan.build_presets:
            if name in seen_build:
                continue
            seen_build.add(name)
            build_presets.append(
                _flattened_preset(
                    plan.build_effective[name],
                    catalog.origin(BUILD_FAMILY, name),
                    source_dir,
                )
            )
        for name in plan.test_presets:
            if name in seen_test:
                continue
            seen_test.add(name)
            test_presets.append(
                _flattened_preset(
                    plan.test_effective[name],
                    catalog.origin(TEST_FAMILY, name),
                    source_dir,
                )
            )
    payload: JsonObject = {
        "version": catalog.version or MIN_PRESET_VERSION,
        "configurePresets": configure_presets,
        "buildPresets": build_presets,
        "testPresets": test_presets,
    }
    if catalog.cmake_minimum_required is not None:
        payload["cmakeMinimumRequired"] = copy.deepcopy(catalog.cmake_minimum_required)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


# --------------------------------------------------------------------------
# Command execution
# --------------------------------------------------------------------------


def format_command(command: Sequence[str]) -> str:
    """Format a command line for logs and dry-run output."""
    if os.name == "nt":
        return subprocess.list2cmdline(list(command))
    return " ".join(shlex_quote(item) for item in command)


def shlex_quote(text: str) -> str:
    """Quote one POSIX shell argument."""
    if re.fullmatch(r"[A-Za-z0-9_./:\\=+,-]+", text):
        return text
    return "'" + text.replace("'", "'\\''") + "'"


def _kill_tree(process: subprocess.Popen[str]) -> None:
    if os.name == "nt":
        with contextlib.suppress(OSError, subprocess.SubprocessError):
            subprocess.run(
                ["taskkill", "/F", "/T", "/PID", str(process.pid)],
                capture_output=True,
                timeout=30,
                check=False,
            )
    else:
        killpg = getattr(os, "killpg", None)
        getpgid = getattr(os, "getpgid", None)
        sigkill = getattr(signal, "SIGKILL", signal.SIGTERM)
        if callable(killpg) and callable(getpgid):
            with contextlib.suppress(OSError):
                killpg(getpgid(process.pid), sigkill)
    with contextlib.suppress(OSError):
        process.kill()


def _pump(pipe: IO[str], log_file: TextIO, tail: deque[str], *, echo: bool) -> None:
    try:
        for line in pipe:
            log_file.write(line)
            log_file.flush()
            tail.append(line)
            if echo:
                sys.stdout.write(line)
    finally:
        pipe.close()


def _run_process(spec: StageSpec, settings: RunSettings) -> ProcessOutcome:
    log_path = settings.log_dir / spec.log_name
    log_path.parent.mkdir(parents=True, exist_ok=True)
    tail: deque[str] = deque(maxlen=400)
    started = time.monotonic()
    try:
        log_file = log_path.open("w", encoding="utf-8", errors="replace")
    except OSError as exc:
        return ProcessOutcome("fail", f"could not create log file: {exc}", 0.0, "")
    with log_file:
        log_file.write(f"# {spec.kind}: {spec.label}\n")
        log_file.write(f"# cwd: {settings.cwd}\n")
        log_file.write(f"# command: {format_command(spec.command)}\n\n")
        log_file.flush()
        creationflags = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0) if os.name == "nt" else 0
        try:
            process = subprocess.Popen(
                spec.command,
                cwd=str(settings.cwd),
                env=settings.env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                encoding="utf-8",
                errors="replace",
                bufsize=1,
                creationflags=creationflags,
                start_new_session=os.name != "nt",
            )
        except OSError as exc:
            return ProcessOutcome(
                "fail",
                f"could not start process: {exc}",
                time.monotonic() - started,
                "",
            )
        stdout = process.stdout
        if stdout is None:
            _kill_tree(process)
            return ProcessOutcome(
                "fail",
                "process has no stdout pipe",
                time.monotonic() - started,
                "",
            )
        reader = threading.Thread(
            target=partial(_pump, echo=settings.verbose),
            args=(stdout, log_file, tail),
            daemon=True,
        )
        reader.start()
        status = "pass"
        detail = ""
        try:
            returncode = process.wait(timeout=settings.timeout or None)
        except subprocess.TimeoutExpired:
            _kill_tree(process)
            reader.join(timeout=10)
            status = "fail"
            detail = f"timed out after {settings.timeout:g}s"
        except KeyboardInterrupt:
            _kill_tree(process)
            reader.join(timeout=10)
            raise
        else:
            reader.join(timeout=10)
            if returncode != 0:
                status = "fail"
                detail = f"exit code {returncode}"
    return ProcessOutcome(status, detail, time.monotonic() - started, "".join(tail))


def _call_validator(validator: Callable[[], str | None]) -> str | None:
    try:
        return validator()
    except Exception as exc:  # noqa: BLE001 - validators must never crash the run
        return f"verification failed: {exc}"


def _finish_stage(spec: StageSpec, settings: RunSettings, outcome: ProcessOutcome) -> StageResult:
    suffix = f" ({outcome.detail})" if outcome.detail else ""
    if settings.verbose:
        print(
            f"       -> {status_text(outcome.status, settings.style)}"
            f"{suffix} in {outcome.duration:.1f}s"
        )
    else:
        print(f"{status_text(outcome.status, settings.style)}{suffix} in {outcome.duration:.1f}s")
    if outcome.status == "fail":
        if not settings.verbose:
            lines = [line.rstrip() for line in outcome.tail.splitlines() if line.strip()]
            for line in lines[-25:]:
                print(f"          | {line}")
        print(f"          log: {settings.log_dir / spec.log_name}")
    return StageResult(
        kind=spec.kind,
        label=spec.label,
        status=outcome.status,
        duration=outcome.duration,
        detail=outcome.detail,
        log=settings.log_dir / spec.log_name,
        tail=outcome.tail,
    )


def run_stage(spec: StageSpec, settings: RunSettings) -> StageResult:
    """Run one stage command, apply its validator, and print the result."""
    if settings.verbose:
        print(f"       {spec.kind:<9} {settings.style(spec.label, 'dim')}")
    else:
        print(
            f"       {spec.kind:<9} {settings.style(spec.label, 'dim')} ... ",
            end="",
            flush=True,
        )
    outcome = _run_process(spec, settings)
    if outcome.status == "pass" and spec.validator is not None:
        problem = _call_validator(spec.validator)
        if problem:
            outcome = replace(outcome, status="fail", detail=problem)
    return _finish_stage(spec, settings, outcome)


# --------------------------------------------------------------------------
# Configuration verification
# --------------------------------------------------------------------------


def read_cmake_cache(path: Path) -> dict[str, str]:
    """Read a ``CMakeCache.txt`` file into a key/value mapping."""
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line or line.startswith(("//", "#")) or "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key.split(":", 1)[0]] = value
    return values


def _check_compilers(cache: dict[str, str], actual: dict[str, str]) -> list[str]:
    problems: list[str] = []
    for variable in COMPILER_VARIABLES:
        expected = cache.get(variable)
        if not expected:
            continue
        found = actual.get(variable)
        if not found:
            problems.append(f"{variable} missing from the cache")
        elif _program_stem(expected) != _program_stem(found):
            problems.append(f"{variable} is {found!r}, expected {expected!r}")
    return problems


def _check_launchers(cache: dict[str, str], actual: dict[str, str]) -> list[str]:
    problems: list[str] = []
    for variable in LAUNCHER_VARIABLES:
        expected = cache.get(variable)
        if not expected:
            continue
        found = actual.get(variable)
        if not found:
            problems.append(f"{variable} missing from the cache")
        elif _program_stem(expected) != _program_stem(found):
            problems.append(f"{variable} is {found!r}, expected {expected!r}")
    return problems


def _check_build_settings(plan: Plan, actual: dict[str, str]) -> list[str]:
    problems: list[str] = []
    expected_build_type = plan.cache.get("CMAKE_BUILD_TYPE")
    if expected_build_type:
        found_build_type = actual.get("CMAKE_BUILD_TYPE")
        if found_build_type != expected_build_type:
            problems.append(
                f"CMAKE_BUILD_TYPE is {found_build_type!r}, expected {expected_build_type!r}"
            )
    generator = _string_field(plan.effective, "generator")
    if generator:
        found_generator = actual.get("CMAKE_GENERATOR")
        if not found_generator or found_generator.lower() != generator.lower():
            problems.append(f"CMAKE_GENERATOR is {found_generator!r}, expected {generator!r}")
    return problems


def _check_home_directory(actual: dict[str, str]) -> list[str]:
    home = actual.get("CMAKE_HOME_DIRECTORY")
    if not home:
        return []
    try:
        same_home = os.path.normcase(str(Path(home).resolve())) == os.path.normcase(
            str(REPO_ROOT.resolve())
        )
    except OSError:
        same_home = False
    if same_home:
        return []
    return [f"CMAKE_HOME_DIRECTORY is {home!r}, expected {str(REPO_ROOT)!r}"]


def _check_testing(plan: Plan, actual: dict[str, str]) -> list[str]:
    if not plan.test_presets:
        return []
    build_testing = actual.get("BUILD_TESTING")
    if build_testing is not None and build_testing.upper() != "ON":
        return [f"BUILD_TESTING is {build_testing!r}, expected 'ON'"]
    return []


def verify_configure_output(plan: Plan) -> str | None:
    """Verify that the generated cache matches the preset's intent."""
    cache_path = plan.build_dir / "CMakeCache.txt"
    if not cache_path.is_file():
        return "CMakeCache.txt was not generated"
    actual = read_cmake_cache(cache_path)
    problems = [
        *_check_compilers(plan.cache, actual),
        *_check_launchers(plan.cache, actual),
        *_check_build_settings(plan, actual),
        *_check_home_directory(actual),
        *_check_testing(plan, actual),
    ]
    return "; ".join(problems) if problems else None


# --------------------------------------------------------------------------
# Execution of the preset matrix
# --------------------------------------------------------------------------


def _append_blocked(stages: list[StageResult], reason: str, selected: set[str]) -> None:
    existing = {stage.kind for stage in stages}
    if "build" in selected and "build" not in existing:
        stages.append(StageResult("build", "(all)", "blocked", 0.0, reason))
    if "test" in selected and "test" not in existing:
        stages.append(StageResult("test", "(all)", "blocked", 0.0, reason))


def _test_log_validator(log_path: Path) -> str | None:
    try:
        text = log_path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    if "No tests were found" in text:
        return "ctest discovered no tests"
    return None


def _configure_command(plan: Plan, options: RunOptions, derived: Path) -> list[str]:
    return [options.cmake_bin, "--preset", plan.name, "--presets-file", str(derived)]


def _build_command(build_name: str, options: RunOptions, derived: Path) -> list[str]:
    command = [
        options.cmake_bin,
        "--build",
        "--preset",
        build_name,
        "--presets-file",
        str(derived),
    ]
    if options.jobs:
        command.extend(["--parallel", str(options.jobs)])
    return command


def _test_command(test_name: str, options: RunOptions, derived: Path) -> list[str]:
    return [
        options.ctest_bin,
        "--preset",
        test_name,
        "--presets-file",
        str(derived),
    ]


def _run_configure(plan: Plan, options: RunOptions, derived: Path, style: Style) -> StageResult:
    settings = RunSettings(
        cwd=REPO_ROOT,
        env=plan.env,
        log_dir=plan.log_dir,
        timeout=options.timeout,
        verbose=options.verbose,
        style=style,
    )
    spec = StageSpec(
        kind="configure",
        label=plan.name,
        command=tuple(_configure_command(plan, options, derived)),
        log_name="configure.log",
        validator=partial(verify_configure_output, plan),
    )
    return run_stage(spec, settings)


def _run_builds(
    plan: Plan, options: RunOptions, derived: Path, style: Style
) -> tuple[list[StageResult], bool]:
    settings = RunSettings(
        cwd=REPO_ROOT,
        env=plan.env,
        log_dir=plan.log_dir,
        timeout=options.timeout,
        verbose=options.verbose,
        style=style,
    )
    stages: list[StageResult] = []
    if plan.build_presets:
        for build_name in plan.build_presets:
            spec = StageSpec(
                kind="build",
                label=build_name,
                command=tuple(_build_command(build_name, options, derived)),
                log_name=f"build-{_slug(build_name)}.log",
            )
            stages.append(run_stage(spec, settings))
            if stages[-1].status != "pass" and options.fail_fast:
                break
    else:
        command = [options.cmake_bin, "--build", str(plan.build_dir)]
        build_type = plan.cache.get("CMAKE_BUILD_TYPE")
        if build_type:
            command.extend(["--config", build_type])
        if options.jobs:
            command.extend(["--parallel", str(options.jobs)])
        spec = StageSpec(
            kind="build",
            label="(default)",
            command=tuple(command),
            log_name="build-default.log",
        )
        stages.append(run_stage(spec, settings))
    return stages, all(stage.status == "pass" for stage in stages)


def _run_tests(
    plan: Plan,
    *,
    build_ok: bool,
    options: RunOptions,
    derived: Path,
    style: Style,
) -> list[StageResult]:
    settings = RunSettings(
        cwd=REPO_ROOT,
        env=plan.env,
        log_dir=plan.log_dir,
        timeout=options.timeout,
        verbose=options.verbose,
        style=style,
    )
    if not build_ok:
        return [StageResult("test", "(all)", "blocked", 0.0, "build failed")]
    if not plan.test_presets:
        log_path = plan.log_dir / "test-default.log"
        spec = StageSpec(
            kind="test",
            label="(default)",
            command=(
                options.ctest_bin,
                "--test-dir",
                str(plan.build_dir),
                "--output-on-failure",
                "--no-tests=error",
            ),
            log_name=log_path.name,
            validator=partial(_test_log_validator, log_path),
        )
        return [run_stage(spec, settings)]
    stages: list[StageResult] = []
    for test_name in plan.test_presets:
        log_path = plan.log_dir / f"test-{_slug(test_name)}.log"
        spec = StageSpec(
            kind="test",
            label=test_name,
            command=tuple(_test_command(test_name, options, derived)),
            log_name=log_path.name,
            validator=partial(_test_log_validator, log_path),
        )
        stages.append(run_stage(spec, settings))
        if stages[-1].status == "fail" and options.fail_fast:
            break
    return stages


def _clean_plan(plan: Plan) -> None:
    for directory in (plan.build_dir, plan.install_dir, plan.log_dir):
        if directory.exists():
            shutil.rmtree(directory, ignore_errors=True)


def _ensure_configure_stage(plan: Plan, options: RunOptions, style: Style) -> StageResult | None:
    if "configure" in options.stages:
        return None
    if (plan.build_dir / "CMakeCache.txt").is_file():
        return None
    print(
        f"       {status_text('fail', style)} "
        "build directory is not configured; include the configure stage"
    )
    return StageResult(
        "configure",
        plan.name,
        "fail",
        0.0,
        "build directory is not configured",
    )


def _print_plan_header(index: int, total: int, plan: Plan, style: Style) -> None:
    arch = f" {plan.arch}" if plan.arch else ""
    print()
    print(
        style(f"[{index}/{total}] {plan.name}", "bold"),
        style(f"({plan.family}{arch})", "dim"),
    )
    for note in plan.notes:
        print(f"       note: {note}")


def _execute_plan(plan: Plan, options: RunOptions, derived: Path, style: Style) -> PlanResult:
    result = PlanResult(plan)
    if plan.skip_reason:
        print(f"       {status_text('skip', style)} {plan.skip_reason}")
        return result
    if options.clean:
        _clean_plan(plan)
    plan.log_dir.mkdir(parents=True, exist_ok=True)
    if "configure" in options.stages:
        stage = _run_configure(plan, options, derived, style)
        result.stages.append(stage)
        if stage.status != "pass":
            print(
                f"       {status_text('blocked', style)} "
                "build/test skipped after a configure failure"
            )
            _append_blocked(result.stages, "configure failed", set(options.stages))
            return result
    else:
        failure = _ensure_configure_stage(plan, options, style)
        if failure is not None:
            result.stages.append(failure)
            _append_blocked(result.stages, "configure stage was not run", set(options.stages))
            return result
    build_ok = True
    if "build" in options.stages:
        build_stages, build_ok = _run_builds(plan, options, derived, style)
        result.stages.extend(build_stages)
        if not build_ok and "test" in options.stages:
            print(f"       {status_text('blocked', style)} tests skipped after a build failure")
    if "test" in options.stages:
        result.stages.extend(
            _run_tests(plan, build_ok=build_ok, options=options, derived=derived, style=style)
        )
    return result


def execute_plans(
    plans: Sequence[Plan], options: RunOptions, derived: Path, style: Style
) -> list[PlanResult]:
    """Execute all selected presets and return their results."""
    results: list[PlanResult] = []
    total = len(plans)
    for index, plan in enumerate(plans, start=1):
        _print_plan_header(index, total, plan, style)
        result = _execute_plan(plan, options, derived, style)
        results.append(result)
        if options.fail_fast and result.status == "fail":
            break
    return results


# --------------------------------------------------------------------------
# Reporting
# --------------------------------------------------------------------------


def _errors(issues: Sequence[Issue]) -> list[Issue]:
    return [issue for issue in issues if issue.severity == "error"]


def print_static_issues(issues: Sequence[Issue], style: Style) -> None:
    """Print static validation findings grouped by severity."""
    errors = _errors(issues)
    warnings = [issue for issue in issues if issue.severity == "warning"]
    print(style("Static checks", "bold") + f": {len(errors)} error(s), {len(warnings)} warning(s)")
    for issue in errors:
        print(f"  {style('ERROR  ', 'red')} {issue.scope}: {issue.message}")
    for issue in warnings:
        print(f"  {style('WARNING', 'yellow')} {issue.scope}: {issue.message}")


def print_list(plans: Sequence[Plan], style: Style) -> None:
    """Print the selected presets and their build/test mappings."""
    print(style("Configure presets", "bold"))
    for plan in plans:
        arch = f" {plan.arch}" if plan.arch else ""
        if plan.skip_reason:
            state = f"skip: {plan.skip_reason}"
            state_style = ("yellow",)
        elif plan.has_static_error:
            state = "invalid (static check failed)"
            state_style = ("red",)
        else:
            state = "ready"
            state_style = ("green",)
        print(
            f"  {plan.name}  {style(f'[{plan.family}{arch}]', 'dim')} {style(state, *state_style)}"
        )
        print(f"      build: {', '.join(plan.build_presets) or '(none)'}")
        print(f"      test : {', '.join(plan.test_presets) or '(none)'}")


def _dry_run_commands(plan: Plan, options: RunOptions, derived: Path) -> list[str]:
    commands: list[str] = []
    if "configure" in options.stages:
        commands.append(format_command(_configure_command(plan, options, derived)))
    if "build" in options.stages:
        for build_name in plan.build_presets or ["(default)"]:
            if build_name == "(default)":
                commands.append(format_command([options.cmake_bin, "--build", str(plan.build_dir)]))
            else:
                commands.append(format_command(_build_command(build_name, options, derived)))
    if "test" in options.stages:
        for test_name in plan.test_presets or ["(default)"]:
            if test_name == "(default)":
                commands.append(
                    format_command(
                        [
                            options.ctest_bin,
                            "--test-dir",
                            str(plan.build_dir),
                            "--output-on-failure",
                        ]
                    )
                )
            else:
                commands.append(format_command(_test_command(test_name, options, derived)))
    return commands


def print_dry_run(plans: Sequence[Plan], derived: Path, options: RunOptions, style: Style) -> None:
    """Print the commands that would run for every selected preset."""
    print()
    print(style("Dry run", "bold"), f"(derived presets: {derived})")
    for plan in plans:
        print()
        print(style(plan.name, "bold"))
        if plan.skip_reason:
            print(f"  {status_text('skip', style)} {plan.skip_reason}")
            continue
        for command in _dry_run_commands(plan, options, derived):
            print(f"  {command}")


def print_summary(results: Sequence[PlanResult], issues: Sequence[Issue], style: Style) -> None:
    """Print the final pass/fail/skip summary."""
    passed = [result for result in results if result.status == "pass"]
    failed = [result for result in results if result.status == "fail"]
    skipped = [result for result in results if result.status == "skip"]
    static_errors = _errors(issues)
    static_warnings = [issue for issue in issues if issue.severity == "warning"]
    print()
    print(style("Summary", "bold"))
    print(
        f"  {style('PASS', 'green')} {len(passed)}   "
        f"{style('FAIL', 'red')} {len(failed)}   "
        f"{style('SKIP', 'yellow')} {len(skipped)}   "
        f"static: {len(static_errors)} error(s), {len(static_warnings)} warning(s)"
    )
    if failed:
        print("  Failed presets:")
        for result in failed:
            details = [
                f"{stage.kind}: {stage.detail}"
                for stage in result.stages
                if stage.status == "fail" and stage.detail
            ]
            details.extend(
                issue.message for issue in result.plan.issues if issue.severity == "error"
            )
            suffix = f"  ({'; '.join(details)})" if details else ""
            print(f"    - {result.plan.name}{suffix}")
    if skipped:
        print("  Skipped presets:")
        for result in skipped:
            print(f"    - {result.plan.name}: {result.plan.skip_reason}")
    log_root = next(
        (result.plan.log_dir.parent for result in results if result.plan.log_dir.parent.exists()),
        None,
    )
    if log_root is not None:
        print(f"  Logs: {log_root}")


def _preset_report(result: PlanResult) -> dict[str, object]:
    return {
        "configure_preset": result.plan.name,
        "toolchain": result.plan.family,
        "architecture": result.plan.arch,
        "build_presets": result.plan.build_presets,
        "test_presets": result.plan.test_presets,
        "build_dir": str(result.plan.build_dir),
        "status": result.status,
        "skip_reason": result.plan.skip_reason,
        "static_issues": [
            {"severity": issue.severity, "message": issue.message} for issue in result.plan.issues
        ],
        "stages": [
            {
                "kind": stage.kind,
                "label": stage.label,
                "status": stage.status,
                "duration_seconds": round(stage.duration, 3),
                "detail": stage.detail,
                "log": str(stage.log) if stage.log else None,
            }
            for stage in result.stages
        ],
    }


def write_report(path: Path, data: ReportData) -> None:
    """Write the machine-readable JSON report."""
    statuses: dict[str, int] = defaultdict(int)
    presets_payload: list[dict[str, object]] = []
    for result in data.results:
        statuses[result.status] += 1
        presets_payload.append(_preset_report(result))
    summary: dict[str, int] = dict(statuses)
    summary["static_errors"] = len(_errors(data.issues))
    summary["static_warnings"] = sum(1 for issue in data.issues if issue.severity == "warning")
    payload: dict[str, object] = {
        "generated_at": datetime.now(UTC).isoformat(timespec="seconds"),
        "source_dir": str(REPO_ROOT),
        "binary_root": str(data.binary_root),
        "cmake_version": data.cmake_version,
        "host": {"system": platform.system(), "machine": platform.machine()},
        "stages": list(data.stages),
        "static_issues": [
            {
                "severity": issue.severity,
                "scope": issue.scope,
                "message": issue.message,
            }
            for issue in data.issues
        ],
        "presets": presets_payload,
        "summary": summary,
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------


def build_parser() -> argparse.ArgumentParser:
    """Build the command-line parser."""
    parser = argparse.ArgumentParser(
        description="Verify all CMake configure, build and test presets.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Preset inheritance is flattened into a derived presets file under --binary-root;\n"
            "only binaryDir/installDir are overridden, so all presets run through the real\n"
            "cmake/ctest --preset code paths without sharing the project's build/ directory.\n"
            "Presets excluded by --exclude are not run."
        ),
    )
    parser.add_argument(
        "--presets-file",
        type=Path,
        default=DEFAULT_PRESETS_FILE,
        help=f"preset file to verify (default: {DEFAULT_PRESETS_FILE})",
    )
    parser.add_argument(
        "--binary-root",
        type=Path,
        default=DEFAULT_BINARY_ROOT,
        help=f"root for isolated build trees and logs (default: {DEFAULT_BINARY_ROOT})",
    )
    parser.add_argument(
        "--presets",
        action="append",
        default=[],
        metavar="GLOB",
        help="only configure presets matching this glob (repeatable)",
    )
    parser.add_argument(
        "--exclude",
        action="append",
        default=[],
        metavar="GLOB",
        help="exclude configure presets matching this glob (repeatable)",
    )
    parser.add_argument(
        "--stages",
        default="configure,build,test",
        help="comma-separated stages to run: configure, build, test (default: all)",
    )
    parser.add_argument(
        "--jobs",
        "-j",
        type=int,
        default=None,
        metavar="N",
        help="pass --parallel N to the build stage (default: preset/native default)",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=1800.0,
        metavar="SECONDS",
        help="timeout per stage; 0 disables it (default: 1800)",
    )
    parser.add_argument(
        "--clean",
        action="store_true",
        help="remove each preset's isolated build tree before configuring",
    )
    parser.add_argument(
        "--no-cpm-cache",
        action="store_true",
        help="do not share a CPM source cache between the isolated build trees",
    )
    parser.add_argument(
        "--fail-fast",
        action="store_true",
        help="stop at the first failing preset or stage",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="stream command output while it runs",
    )
    parser.add_argument(
        "--color",
        choices=("auto", "always", "never"),
        default="auto",
        help="coloured output (default: auto)",
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="list selected presets and their mappings, then exit",
    )
    parser.add_argument(
        "--check-only",
        action="store_true",
        help="run static checks and availability probes without building, then exit",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="print the commands that would run, then exit",
    )
    parser.add_argument(
        "--report",
        type=Path,
        default=None,
        metavar="PATH",
        help="write a JSON report (default: <binary-root>/report.json)",
    )
    return parser


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    """Parse and validate command-line arguments."""
    parser = build_parser()
    args = parser.parse_args(argv)
    stages = [stage.strip() for stage in args.stages.split(",") if stage.strip()]
    unknown = [stage for stage in stages if stage not in STAGE_ORDER]
    if unknown:
        parser.error(
            f"unknown stage(s): {', '.join(unknown)}; choose from {', '.join(STAGE_ORDER)}"
        )
    if not stages:
        parser.error("--stages must select at least one of: " + ", ".join(STAGE_ORDER))
    args.stages = stages
    if args.jobs is not None and args.jobs <= 0:
        parser.error("--jobs must be greater than zero")
    if args.timeout < 0:
        parser.error("--timeout must be non-negative")
    return args


def get_cmake_version(cmake_bin: str) -> str | None:
    """Return the CMake version string, or ``None`` when CMake is unavailable."""
    try:
        result = subprocess.run(
            [cmake_bin, "--version"],
            capture_output=True,
            text=True,
            encoding=_console_encoding(),
            errors="replace",
            timeout=60,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if result.returncode != 0:
        return None
    first_line = result.stdout.splitlines()[0] if result.stdout else ""
    match = re.search(r"cmake version ([0-9][0-9.]*)", first_line, re.IGNORECASE)
    return match.group(1) if match else first_line.strip() or None


def project_min_cmake(source_dir: Path) -> str | None:
    """Return the CMake version required by ``CMakeLists.txt``."""
    cmake_lists = source_dir / "CMakeLists.txt"
    if not cmake_lists.is_file():
        return None
    try:
        text = cmake_lists.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    match = re.search(r"cmake_minimum_required\s*\(\s*VERSION\s+([0-9.]+)", text, re.IGNORECASE)
    return match.group(1) if match else None


def version_tuple(text: str) -> tuple[int, ...]:
    """Convert a dotted version string into a comparable tuple."""
    return tuple(int(part) for part in re.findall(r"\d+", text)[:3])


def _print_error(style: Style, message: str) -> None:
    print(style(f"error: {message}", "red"), file=sys.stderr)


def _find_tools(stages: Sequence[str], style: Style) -> Toolchain | None:
    cmake_bin = shutil.which("cmake")
    if cmake_bin is None:
        _print_error(style, "cmake was not found on PATH")
        return None
    ctest_bin = shutil.which("ctest")
    if ctest_bin is None and "test" in stages:
        _print_error(style, "ctest was not found on PATH")
        return None
    return Toolchain(cmake_bin, ctest_bin or "ctest")


def _build_options(args: argparse.Namespace, tools: Toolchain) -> RunOptions:
    return RunOptions(
        presets_file=args.presets_file,
        binary_root=args.binary_root.resolve(),
        includes=tuple(args.presets),
        excludes=tuple(args.exclude),
        stages=tuple(args.stages),
        jobs=args.jobs,
        timeout=args.timeout,
        clean=args.clean,
        fail_fast=args.fail_fast,
        verbose=args.verbose,
        cmake_bin=tools.cmake,
        ctest_bin=tools.ctest,
        report=args.report,
        list_only=args.list,
        check_only=args.check_only,
        dry_run=args.dry_run,
        no_cpm_cache=args.no_cpm_cache,
    )


def _global_issues(cmake_version: str) -> list[Issue]:
    minimum = project_min_cmake(REPO_ROOT)
    if minimum and version_tuple(cmake_version) < version_tuple(minimum):
        return [
            Issue(
                "error",
                "CMakeLists.txt",
                f"CMake {minimum} is required but cmake {cmake_version} is on PATH",
            )
        ]
    return []


def _configure_cpm_cache(
    plans: Sequence[Plan], binary_root: Path, *, no_cpm_cache: bool
) -> str | None:
    if no_cpm_cache:
        return None
    existing = os.environ.get("CPM_SOURCE_CACHE")
    if existing:
        return existing
    cache_dir = binary_root / ".cpm-cache"
    for plan in plans:
        if plan.env:
            plan.env.setdefault("CPM_SOURCE_CACHE", str(cache_dir))
    return str(cache_dir)


def _print_header(options: RunOptions, data: HeaderData, style: Style) -> None:
    print(style("CMake preset verification", "bold"))
    print(f"  source dir : {REPO_ROOT}")
    print(f"  build root : {options.binary_root}")
    print(f"  cmake      : {data.cmake_version} ({options.cmake_bin})")
    print(f"  host       : {platform.system()} ({platform.machine()})")
    if data.cpm_cache:
        print(f"  cpm cache  : {data.cpm_cache}")
    print(f"  selected   : {data.selected} configure preset(s) of {data.visible} visible")
    print()


def _bootstrap(args: argparse.Namespace, style: Style) -> Session | None:
    try:
        catalog = PresetCatalog(REPO_ROOT)
        catalog.load(args.presets_file)
    except PresetError as exc:
        _print_error(style, str(exc))
        return None
    tools = _find_tools(args.stages, style)
    if tools is None:
        return None
    cmake_version = get_cmake_version(tools.cmake)
    if cmake_version is None:
        _print_error(style, "could not determine the CMake version")
        return None
    options = _build_options(args, tools)
    plans, issues = analyse(
        catalog,
        REPO_ROOT,
        options.includes,
        options.excludes,
        options.binary_root,
    )
    issues = [*_global_issues(cmake_version), *issues]
    cpm_cache = _configure_cpm_cache(plans, options.binary_root, no_cpm_cache=options.no_cpm_cache)
    _print_header(
        options,
        HeaderData(
            selected=len(plans),
            visible=len(catalog.visible_names(CONFIGURE_FAMILY)),
            cmake_version=cmake_version,
            cpm_cache=cpm_cache,
        ),
        style,
    )
    print_static_issues(issues, style)
    if not plans:
        _print_error(style, "no configure preset matched the selection")
        return None
    return Session(
        options=options,
        catalog=catalog,
        plans=plans,
        issues=issues,
        cmake_version=cmake_version,
        style=style,
    )


def _check_only(session: Session) -> int:
    print()
    print(session.style("Availability", "bold"))
    for plan in session.plans:
        if plan.skip_reason:
            print(f"  {status_text('skip', session.style)} {plan.name}: {plan.skip_reason}")
        elif plan.has_static_error:
            print(f"  {session.style('INVALID', 'red')} {plan.name}")
        else:
            print(f"  {session.style('READY', 'green')} {plan.name}")
    return 1 if _errors(session.issues) else 0


def _execute(session: Session, derived: Path) -> int:
    executable = [plan for plan in session.plans if plan.skip_reason is None]
    if not executable:
        print(
            session.style("error: every selected preset was skipped; nothing was verified", "red")
        )
        for plan in session.plans:
            print(f"  - {plan.name}: {plan.skip_reason}")
        return 2
    try:
        results = execute_plans(session.plans, session.options, derived, session.style)
    except KeyboardInterrupt:
        print()
        print(session.style("Interrupted.", "yellow"))
        return 130
    print_summary(results, session.issues, session.style)
    report_path = session.options.report or session.options.binary_root / "report.json"
    data = ReportData(
        cmake_version=session.cmake_version,
        stages=session.options.stages,
        binary_root=session.options.binary_root,
        results=results,
        issues=session.issues,
    )
    try:
        write_report(report_path, data)
    except OSError as exc:
        print(session.style(f"warning: could not write report {report_path}: {exc}", "yellow"))
    else:
        print(f"  Report: {report_path}")
    failed = any(result.status == "fail" for result in results) or bool(_errors(session.issues))
    return 1 if failed else 0


def _dispatch(session: Session, args: argparse.Namespace) -> int:
    if args.list:
        print()
        print_list(session.plans, session.style)
        return 0
    if args.check_only:
        return _check_only(session)
    derived = session.options.binary_root / "presets.verify.json"
    try:
        write_derived_presets(
            session.plans,
            session.catalog,
            REPO_ROOT,
            derived,
        )
    except OSError as exc:
        _print_error(session.style, f"could not write {derived}: {exc}")
        return 2
    if args.dry_run:
        print_dry_run(session.plans, derived, session.options, session.style)
        return 1 if _errors(session.issues) else 0
    return _execute(session, derived)


def main(argv: Sequence[str] | None = None) -> int:
    """Run the preset verification CLI."""
    args = parse_args(argv)
    style = Style(args.color)
    session = _bootstrap(args, style)
    if session is None:
        return 2
    return _dispatch(session, args)


if __name__ == "__main__":
    sys.exit(main())
