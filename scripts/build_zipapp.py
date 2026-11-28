#!/usr/bin/env python3
"""Build a single-file zipapp for the CMake preset verifier.

The generic archive contains only the ``cmake_workflow`` package.  With
``--project`` the repository entry point (project hooks) is embedded as
``__main__.py`` instead, producing a ready-to-use executable for this repo.

Examples:
    python scripts/build_zipapp.py                 # dist/cmake_workflow.pyz
    python scripts/build_zipapp.py --project       # dist/run_cmake_workflow.pyz
    python scripts/build_zipapp.py --output build/tool.pyz
"""

from __future__ import annotations

import argparse
import shutil
import sys
import tempfile
import zipapp
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from collections.abc import Sequence

REPO_ROOT = Path(__file__).resolve().parent.parent
PACKAGE_DIR = REPO_ROOT / "scripts" / "cmake_workflow"
PROJECT_ENTRY = REPO_ROOT / "scripts" / "run_cmake_workflow.py"
DIST_DIR = REPO_ROOT / "scripts" / "dist"

GENERIC_MAIN = """\
from __future__ import annotations

import sys

from cmake_workflow.cli import main

if __name__ == "__main__":
    sys.exit(main())
"""


def build_archive(target: Path, *, project: bool) -> None:
    """Create the zipapp at *target* from the package and the chosen entry point."""
    with tempfile.TemporaryDirectory(prefix="cmake-workflow-zipapp-") as tmp:
        staging = Path(tmp)
        shutil.copytree(
            PACKAGE_DIR,
            staging / "cmake_workflow",
            ignore=shutil.ignore_patterns("__pycache__", "*.pyc"),
        )
        if project:
            shutil.copyfile(PROJECT_ENTRY, staging / "__main__.py")
        else:
            (staging / "__main__.py").write_text(GENERIC_MAIN, encoding="utf-8")
        target.parent.mkdir(parents=True, exist_ok=True)
        zipapp.create_archive(staging, target, interpreter="/usr/bin/env python3")


def main(argv: Sequence[str] | None = None) -> int:
    """Build the requested zipapp."""
    parser = argparse.ArgumentParser(description="Build a single-file zipapp.")
    parser.add_argument(
        "--project",
        action="store_true",
        help="embed scripts/run_cmake_workflow.py (project hooks) as __main__.py",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="archive path (default: dist/<name>.pyz)",
    )
    args = parser.parse_args(argv)
    name = "run_cmake_workflow.pyz" if args.project else "cmake_workflow.pyz"
    target = args.output or DIST_DIR / name
    build_archive(target, project=args.project)
    print(f"built {target}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
