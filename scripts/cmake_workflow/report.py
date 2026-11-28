"""Static-issue printing, list/dry-run output and the JSON report."""

from __future__ import annotations

import json
import platform
from collections import defaultdict
from datetime import UTC, datetime
from typing import TYPE_CHECKING

from .console import Style, status_text
from .execution import (
    _build_command,
    _configure_command,
    _test_command,
    _use_workflow,
    _workflow_command,
    format_command,
)

if TYPE_CHECKING:
    from collections.abc import Sequence
    from pathlib import Path

    from .models import Issue, Plan, PlanResult, ReportData, RunOptions


def _errors(issues: Sequence[Issue]) -> list[Issue]:
    return [issue for issue in issues if issue.severity == "error"]


def print_static_issues(issues: Sequence[Issue], style: Style) -> None:
    """Print static validation findings grouped by severity."""
    errors = _errors(issues)
    warnings = [issue for issue in issues if issue.severity == "warning"]
    print(style("Static checks", "bold") + f": {len(errors)} error(s), {len(warnings)} warning(s)")
    for issue in [*errors, *warnings]:
        label = "ERROR  " if issue.severity == "error" else "WARNING"
        colour = "red" if issue.severity == "error" else "yellow"
        print(f"  {style(label, colour)} {issue.scope}: {issue.message}")


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
        print(f"      workflow: {plan.workflow or '(none)'}")
        print(f"      build: {', '.join(plan.build_presets) or '(none)'}")
        print(f"      test : {', '.join(plan.test_presets) or '(none)'}")


def _dry_run_commands(plan: Plan, options: RunOptions, derived: Path) -> list[str]:
    if _use_workflow(plan, options):
        return [format_command(_workflow_command(plan.workflow or "", options, derived))]
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
        "workflow_preset": result.plan.workflow,
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
        "source_dir": str(data.source_dir),
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
