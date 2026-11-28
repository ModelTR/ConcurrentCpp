"""Command-line interface for the generic CMake preset verifier."""

from __future__ import annotations

import argparse
import os
import platform
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING

from .console import Style, status_text
from .constants import CONFIGURE_FAMILY
from .environment import _console_encoding
from .execution import execute_plans
from .models import HeaderData, Issue, PresetError, ReportData, RunOptions, Toolchain
from .presets import PresetCatalog, write_derived_presets
from .report import (
    _errors,
    print_dry_run,
    print_list,
    print_static_issues,
    print_summary,
    write_report,
)
from .static import analyse

if TYPE_CHECKING:
    from collections.abc import Sequence

    from .hooks import ProjectHooks
    from .models import Plan


@dataclass(frozen=True)
class CliDefaults:
    """Project defaults injected into the generic CLI."""

    source_dir: Path
    presets_file: Path
    binary_root: Path


@dataclass(frozen=True)
class Session:
    """A fully analyzed verification run."""

    options: RunOptions
    catalog: PresetCatalog
    plans: list[Plan]
    issues: list[Issue]
    cmake_version: str
    style: Style


def default_cli_defaults() -> CliDefaults:
    """Return defaults derived from the current working directory."""
    cwd = Path.cwd()
    return CliDefaults(
        source_dir=cwd,
        presets_file=cwd / "CMakePresets.json",
        binary_root=cwd / "build" / "preset-verify",
    )


def build_parser(defaults: CliDefaults) -> argparse.ArgumentParser:
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
        "--source-dir",
        type=Path,
        default=None,
        help=f"project root used for defaults (default: {defaults.source_dir})",
    )
    parser.add_argument(
        "--presets-file",
        type=Path,
        default=None,
        help="preset file to verify (default: <source-dir>/CMakePresets.json)",
    )
    parser.add_argument(
        "--binary-root",
        type=Path,
        default=None,
        help="root for isolated build trees and logs (default: <source-dir>/build/preset-verify)",
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
        "--concurrency",
        type=int,
        default=None,
        metavar="N",
        help=(
            "number of presets to configure/build/test in parallel "
            "(default: min(4, CPU count); 1 runs them serially)"
        ),
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
        "--clean-after",
        action=argparse.BooleanOptionalAction,
        default=False,
        help=(
            "delete each preset's build and install tree once its log is written "
            "(default: disabled; use --clean-after to delete the products)"
        ),
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


def parse_args(argv: Sequence[str] | None, defaults: CliDefaults) -> argparse.Namespace:
    """Parse and validate command-line arguments."""
    parser = build_parser(defaults)
    args = parser.parse_args(argv)
    stages = [stage.strip() for stage in args.stages.split(",") if stage.strip()]
    unknown = [stage for stage in stages if stage not in {"configure", "build", "test"}]
    if unknown:
        parser.error(f"unknown stage(s): {', '.join(unknown)}; choose from configure, build, test")
    if not stages:
        parser.error("--stages must select at least one of: configure, build, test")
    args.stages = stages
    if args.jobs is not None and args.jobs <= 0:
        parser.error("--jobs must be greater than zero")
    if args.concurrency is not None and args.concurrency <= 0:
        parser.error("--concurrency must be greater than zero")
    if args.timeout < 0:
        parser.error("--timeout must be non-negative")
    source_dir = (args.source_dir or defaults.source_dir).resolve()
    default_source = defaults.source_dir.resolve()
    args.source_dir = source_dir
    if source_dir == default_source:
        fallback_presets = defaults.presets_file.resolve()
        fallback_binary = defaults.binary_root.resolve()
    else:
        fallback_presets = source_dir / "CMakePresets.json"
        fallback_binary = source_dir / "build" / "preset-verify"
    args.presets_file = (
        args.presets_file.resolve() if args.presets_file is not None else fallback_presets
    )
    args.binary_root = (
        args.binary_root.resolve() if args.binary_root is not None else fallback_binary
    )
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
        source_dir=args.source_dir,
        presets_file=args.presets_file,
        binary_root=args.binary_root,
        includes=tuple(args.presets),
        excludes=tuple(args.exclude),
        stages=tuple(args.stages),
        jobs=args.jobs,
        concurrency=args.concurrency or min(4, os.cpu_count() or 1),
        timeout=args.timeout,
        clean=args.clean,
        clean_after=args.clean_after,
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


def _global_issues(cmake_version: str, source_dir: Path) -> list[Issue]:
    minimum = project_min_cmake(source_dir)
    if minimum and version_tuple(cmake_version) < version_tuple(minimum):
        return [
            Issue(
                "error",
                "CMakeLists.txt",
                f"CMake {minimum} is required but cmake {cmake_version} is on PATH",
            )
        ]
    return []


def _print_header(options: RunOptions, data: HeaderData, style: Style) -> None:
    print(style("CMake preset verification", "bold"))
    print(f"  source dir : {options.source_dir}")
    print(f"  build root : {options.binary_root}")
    print(f"  cmake      : {data.cmake_version} ({options.cmake_bin})")
    print(f"  host       : {platform.system()} ({platform.machine()})")
    if data.cpm_cache:
        print(f"  cpm cache  : {data.cpm_cache}")
    print(f"  selected   : {data.selected} configure preset(s) of {data.visible} visible")
    print()


def _bootstrap(
    args: argparse.Namespace,
    style: Style,
    hooks: ProjectHooks | None,
) -> Session | None:
    try:
        catalog = PresetCatalog(args.source_dir)
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
    plans, issues = analyse(catalog, options, hooks)
    issues = [*_global_issues(cmake_version, options.source_dir), *issues]
    cache_hint = None
    if hooks is not None and hooks.configure_environment is not None:
        cache_hint = hooks.configure_environment(plans, options.binary_root, options.no_cpm_cache)
    _print_header(
        options,
        HeaderData(
            selected=len(plans),
            visible=len(catalog.visible_names(CONFIGURE_FAMILY)),
            cmake_version=cmake_version,
            cpm_cache=cache_hint,
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
        source_dir=session.options.source_dir,
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
            session.options.source_dir,
            derived,
            jobs=session.options.jobs,
        )
    except OSError as exc:
        _print_error(session.style, f"could not write {derived}: {exc}")
        return 2
    if args.dry_run:
        print_dry_run(session.plans, derived, session.options, session.style)
        return 1 if _errors(session.issues) else 0
    return _execute(session, derived)


def main(
    argv: Sequence[str] | None = None,
    *,
    hooks: ProjectHooks | None = None,
    defaults: CliDefaults | None = None,
) -> int:
    """Run the preset verification CLI."""
    resolved = defaults if defaults is not None else default_cli_defaults()
    args = parse_args(argv, resolved)
    style = Style(args.color)
    session = _bootstrap(args, style, hooks)
    if session is None:
        return 2
    return _dispatch(session, args)
