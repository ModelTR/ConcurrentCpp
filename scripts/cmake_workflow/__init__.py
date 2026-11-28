"""Generic CMake preset/workflow verification package.

The package is project-agnostic: it loads any CMake presets file, flattens
inheritance into an isolated build root, and runs configure/build/test either
through workflow presets or as separate stages (in parallel).  Project-specific
behaviour (toolchain discovery, extra static checks, cache directories) is
injected through :class:`cmake_workflow.hooks.ProjectHooks`.

The project entry point (``scripts/run_cmake_workflow.py``) wires those hooks
for this repository.
"""

from __future__ import annotations
