#!/usr/bin/env python3
"""Verify ConcurrentCpp's CMake presets (project entry point).

The generic logic lives in the ``cmake_workflow`` package; this module only
supplies the project hooks: the MSYS2 UCRT64 x86 warning and the shared CPM
source cache.  CLI options, output and ``report.json`` are unchanged from the
previous single-file script.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path
from typing import TYPE_CHECKING

from cmake_workflow.cli import CliDefaults, main as cli_main
from cmake_workflow.hooks import ProjectHooks
from cmake_workflow.models import Issue, Plan

if TYPE_CHECKING:
    from collections.abc import Sequence


def _source_dir() -> Path:
    """Return the project root, whether run from source or from a zipapp."""
    env = os.environ.get("CMAKE_WORKFLOW_SOURCE_DIR")
    if env:
        return Path(env).resolve()
    here = Path(__file__).resolve()
    root = here.parent.parent
    for candidate in (root, root.parent, Path.cwd()):
        if (candidate / "CMakeLists.txt").is_file():
            return candidate
    return Path.cwd()


def _x86_ucrt_warning(plan: Plan) -> list[Issue]:
    """Warn when a UCRT preset targets x86 (only a 64-bit toolchain exists)."""
    if plan.family in {"clang", "gcc"} and plan.arch is not None and plan.arch.lower() == "x86":
        return [
            Issue(
                "warning",
                plan.name,
                "preset targets x86, but MSYS2 UCRT64 provides a 64-bit "
                "toolchain; architecture is not enforced with Ninja",
            )
        ]
    return []


def _configure_cpm_cache(
    plans: Sequence[Plan],
    binary_root: Path,
    no_cpm_cache: bool,  # noqa: FBT001
) -> str | None:
    """Share one CPM source cache between the isolated build trees."""
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


def _defaults() -> CliDefaults:
    root = _source_dir()
    return CliDefaults(
        source_dir=root,
        presets_file=root / "CMakePresets.json",
        binary_root=root / "build" / "preset-verify",
    )


def main(argv: Sequence[str] | None = None) -> int:
    """Run the project verification with its toolchain hooks."""
    hooks = ProjectHooks(
        extra_issues=_x86_ucrt_warning,
        configure_environment=_configure_cpm_cache,
    )
    return cli_main(argv, hooks=hooks, defaults=_defaults())


if __name__ == "__main__":
    sys.exit(main())
