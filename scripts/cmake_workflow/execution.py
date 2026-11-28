"""Command execution: stages, workflow runs and parallel plan execution."""

from __future__ import annotations

import contextlib
import os
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
from collections import deque
from concurrent.futures import ThreadPoolExecutor
from dataclasses import replace
from functools import partial
from pathlib import Path
from typing import TYPE_CHECKING

from .console import Style, _emit, _set_output_prefix, status_text
from .constants import (
    COMPILER_VARIABLES,
    LAUNCHER_VARIABLES,
    STAGE_ORDER,
    WORKFLOW_STEP_RE,
)
from .environment import _program_stem
from .jsonutil import _string_field
from .models import (
    Plan,
    PlanResult,
    ProcessOutcome,
    RunOptions,
    RunSettings,
    StageResult,
    StageSpec,
)
from .static import _slug

if TYPE_CHECKING:
    from collections.abc import Callable, Sequence
    from typing import IO, TextIO


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


_LOG_FLUSH_LINES = 64


def _pump(pipe: IO[str], log_file: TextIO, tail: deque[str], *, echo: bool) -> None:
    """Copy process output into the log, flushing in batches."""
    try:
        pending = 0
        for line in pipe:
            log_file.write(line)
            tail.append(line)
            if echo:
                sys.stdout.write(line)
            pending += 1
            if pending >= _LOG_FLUSH_LINES:
                log_file.flush()
                pending = 0
    finally:
        log_file.flush()
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
        _emit(
            f"       -> {status_text(outcome.status, settings.style)}"
            f"{suffix} in {outcome.duration:.1f}s"
        )
    else:
        _emit(f"{status_text(outcome.status, settings.style)}{suffix} in {outcome.duration:.1f}s")
    if outcome.status == "fail":
        if not settings.verbose:
            lines = [line.rstrip() for line in outcome.tail.splitlines() if line.strip()]
            for line in lines[-25:]:
                _emit(f"          | {line}")
        _emit(f"          log: {settings.log_dir / spec.log_name}")
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
        _emit(f"       {spec.kind:<9} {settings.style(spec.label, 'dim')}")
    else:
        _emit(
            f"       {spec.kind:<9} {settings.style(spec.label, 'dim')} ... ",
            end="",
        )
    outcome = _run_process(spec, settings)
    if outcome.status == "pass" and spec.validator is not None:
        problem = _call_validator(spec.validator)
        if problem:
            outcome = replace(outcome, status="fail", detail=problem)
    return _finish_stage(spec, settings, outcome)


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


def _check_home_directory(actual: dict[str, str], source_dir: Path) -> list[str]:
    home = actual.get("CMAKE_HOME_DIRECTORY")
    if not home:
        return []
    try:
        same_home = os.path.normcase(str(Path(home).resolve())) == os.path.normcase(
            str(source_dir.resolve())
        )
    except OSError:
        same_home = False
    if same_home:
        return []
    return [f"CMAKE_HOME_DIRECTORY is {home!r}, expected {str(source_dir)!r}"]


def _check_testing(plan: Plan, actual: dict[str, str]) -> list[str]:
    if not plan.test_presets:
        return []
    build_testing = actual.get("BUILD_TESTING")
    if build_testing is not None and build_testing.upper() != "ON":
        return [f"BUILD_TESTING is {build_testing!r}, expected 'ON'"]
    return []


def verify_configure_output(plan: Plan, source_dir: Path) -> str | None:
    """Verify that the generated cache matches the preset's intent."""
    cache_path = plan.build_dir / "CMakeCache.txt"
    if not cache_path.is_file():
        return "CMakeCache.txt was not generated"
    actual = read_cmake_cache(cache_path)
    problems = [
        *_check_compilers(plan.cache, actual),
        *_check_launchers(plan.cache, actual),
        *_check_build_settings(plan, actual),
        *_check_home_directory(actual, source_dir),
        *_check_testing(plan, actual),
    ]
    return "; ".join(problems) if problems else None


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


def _stage_settings(plan: Plan, options: RunOptions, style: Style) -> RunSettings:
    return RunSettings(
        cwd=options.source_dir,
        env=plan.env,
        log_dir=plan.log_dir,
        timeout=options.timeout,
        verbose=options.verbose,
        style=style,
    )


def _run_configure(plan: Plan, options: RunOptions, derived: Path, style: Style) -> StageResult:
    settings = _stage_settings(plan, options, style)
    spec = StageSpec(
        kind="configure",
        label=plan.name,
        command=tuple(_configure_command(plan, options, derived)),
        log_name="configure.log",
        validator=partial(verify_configure_output, plan, options.source_dir),
    )
    return run_stage(spec, settings)


def _run_builds(
    plan: Plan, options: RunOptions, derived: Path, style: Style
) -> tuple[list[StageResult], bool]:
    settings = _stage_settings(plan, options, style)
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
    settings = _stage_settings(plan, options, style)
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


def _use_workflow(plan: Plan, options: RunOptions) -> bool:
    """Return whether the plan runs as one CMake workflow invocation."""
    return plan.workflow is not None and set(options.stages) == set(STAGE_ORDER)


def _workflow_command(workflow: str, options: RunOptions, derived: Path) -> list[str]:
    """Build the ``cmake --workflow`` command for one preset."""
    return [
        options.cmake_bin,
        "--workflow",
        "--preset",
        workflow,
        "--presets-file",
        str(derived),
    ]


def _read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def _workflow_steps(text: str) -> list[tuple[str, str]]:
    """Return the (kind, preset) pairs CMake reported while running."""
    return [(result.group(1), result.group(2)) for result in WORKFLOW_STEP_RE.finditer(text)]


def _workflow_stage_results(
    plan: Plan,
    steps: Sequence[tuple[str, str]],
    outcome: ProcessOutcome,
    log_path: Path,
) -> list[StageResult]:
    """Turn one workflow invocation into per-step stage results."""
    if not steps:
        return [
            StageResult(
                kind="workflow",
                label=plan.workflow or plan.name,
                status=outcome.status,
                duration=outcome.duration,
                detail=outcome.detail,
                log=log_path,
                tail=outcome.tail,
            )
        ]
    failing = len(steps) - 1 if outcome.status != "pass" else -1
    return [
        StageResult(
            kind=kind,
            label=label,
            status="fail" if index == failing else "pass",
            duration=outcome.duration if index == failing else 0.0,
            detail=outcome.detail if index == failing else "",
            log=log_path,
            tail=outcome.tail if index == failing else "",
        )
        for index, (kind, label) in enumerate(steps)
    ]


def _apply_workflow_validators(
    plan: Plan, stages: list[StageResult], log_path: Path, source_dir: Path
) -> None:
    """Apply the configure and test validators to a finished workflow."""
    for index, stage in enumerate(stages):
        if stage.status != "pass":
            continue
        problem: str | None = None
        if stage.kind == "configure":
            problem = verify_configure_output(plan, source_dir)
        elif stage.kind == "test":
            problem = _test_log_validator(log_path)
        if problem:
            stages[index] = replace(stage, status="fail", detail=problem)


def _run_workflow(
    plan: Plan, options: RunOptions, derived: Path, style: Style
) -> list[StageResult]:
    """Run one plan through its CMake workflow preset."""
    settings = _stage_settings(plan, options, style)
    spec = StageSpec(
        kind="workflow",
        label=plan.workflow or plan.name,
        command=tuple(_workflow_command(plan.workflow or "", options, derived)),
        log_name="workflow.log",
    )
    outcome = _run_process(spec, settings)
    log_path = plan.log_dir / spec.log_name
    stages = _workflow_stage_results(
        plan,
        _workflow_steps(_read_text(log_path)),
        outcome,
        log_path,
    )
    _apply_workflow_validators(plan, stages, log_path, options.source_dir)
    failed = next((stage for stage in stages if stage.status == "fail"), None)
    if failed is not None and outcome.status == "pass":
        outcome = replace(outcome, status="fail", detail=failed.detail)
    _finish_stage(spec, settings, outcome)
    return stages


def _clean_plan(plan: Plan) -> None:
    for directory in (plan.build_dir, plan.install_dir, plan.log_dir):
        if directory.exists():
            shutil.rmtree(directory, ignore_errors=True)


def _remove_build_products(plan: Plan) -> None:
    """Delete the build and install trees, keeping the logs."""
    for directory in (plan.build_dir, plan.install_dir):
        if directory.exists():
            shutil.rmtree(directory, ignore_errors=True)


def _ensure_configure_stage(plan: Plan, options: RunOptions, style: Style) -> StageResult | None:
    if "configure" in options.stages:
        return None
    if (plan.build_dir / "CMakeCache.txt").is_file():
        return None
    _emit(
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
    title = style(f"[{index}/{total}] {plan.name}", "bold")
    meta = style(f"({plan.family}{arch})", "dim")
    _emit()
    _emit(f"{title} {meta}")
    for note in plan.notes:
        _emit(f"       note: {note}")


def _execute_plan(plan: Plan, options: RunOptions, derived: Path, style: Style) -> PlanResult:
    result = PlanResult(plan)
    if plan.skip_reason:
        _emit(f"       {status_text('skip', style)} {plan.skip_reason}")
        return result
    if options.clean:
        _clean_plan(plan)
    plan.log_dir.mkdir(parents=True, exist_ok=True)
    if _use_workflow(plan, options):
        result.stages.extend(_run_workflow(plan, options, derived, style))
        return result
    if "configure" in options.stages:
        stage = _run_configure(plan, options, derived, style)
        result.stages.append(stage)
        if stage.status != "pass":
            _emit(
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
            _emit(f"       {status_text('blocked', style)} tests skipped after a build failure")
    if "test" in options.stages:
        result.stages.extend(
            _run_tests(plan, build_ok=build_ok, options=options, derived=derived, style=style)
        )
    return result


def _execute_sequential(
    plans: Sequence[Plan], options: RunOptions, derived: Path, style: Style
) -> list[PlanResult]:
    """Run every plan one after another, stopping early on --fail-fast."""
    results: list[PlanResult] = []
    total = len(plans)
    for index, plan in enumerate(plans, start=1):
        _print_plan_header(index, total, plan, style)
        try:
            result = _execute_plan(plan, options, derived, style)
        finally:
            if options.clean_after:
                _remove_build_products(plan)
        results.append(result)
        if options.fail_fast and result.status == "fail":
            break
    return results


def _execute_concurrent(
    plans: Sequence[Plan], options: RunOptions, derived: Path, style: Style
) -> list[PlanResult]:
    """Run several plans in parallel, each in its own build tree."""
    results: dict[int, PlanResult] = {}
    stop = threading.Event()
    lock = threading.Lock()

    def run(index: int, plan: Plan) -> None:
        _set_output_prefix(f"[{plan.name}] ")
        try:
            if stop.is_set():
                return
            for note in plan.notes:
                _emit(f"       note: {note}")
            try:
                result = _execute_plan(plan, options, derived, style)
            finally:
                if options.clean_after:
                    _remove_build_products(plan)
            with lock:
                results[index] = result
            if result.status == "fail" and options.fail_fast:
                stop.set()
        finally:
            _set_output_prefix("")

    with ThreadPoolExecutor(max_workers=options.concurrency) as pool:
        futures = [pool.submit(run, index, plan) for index, plan in enumerate(plans, start=1)]
        for future in futures:
            future.result()
    return [results[index] for index in sorted(results)]


def execute_plans(
    plans: Sequence[Plan], options: RunOptions, derived: Path, style: Style
) -> list[PlanResult]:
    """Execute all selected presets, in parallel when concurrency allows."""
    if options.concurrency <= 1 or len(plans) <= 1:
        return _execute_sequential(plans, options, derived, style)
    return _execute_concurrent(plans, options, derived, style)
