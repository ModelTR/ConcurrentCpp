"""Constants shared by the generic verifier."""

from __future__ import annotations

import re

FAMILIES = ("configurePresets", "buildPresets", "testPresets", "workflowPresets")
WORKFLOW_FAMILY = "workflowPresets"
STAGE_ORDER = ("configure", "build", "test")
MIN_PRESET_VERSION = 2
CONFIGURE_FAMILY = "configurePresets"
BUILD_FAMILY = "buildPresets"
TEST_FAMILY = "testPresets"
MACRO_RE = re.compile(r"\$\{(sourceDir|sourceParentDir|fileDir|hostSystemName)\}")
COMPILER_VARIABLES = ("CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER")
LAUNCHER_VARIABLES = ("CMAKE_C_COMPILER_LAUNCHER", "CMAKE_CXX_COMPILER_LAUNCHER")
WORKFLOW_STEP_RE = re.compile(
    r'Executing workflow step \d+ of \d+: (configure|build|test) preset "([^"]+)"',
)
