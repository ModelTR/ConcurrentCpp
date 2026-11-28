"""Static validation and plan construction."""

from __future__ import annotations

import fnmatch
import re
from typing import TYPE_CHECKING

from .constants import (
    BUILD_FAMILY,
    COMPILER_VARIABLES,
    CONFIGURE_FAMILY,
    LAUNCHER_VARIABLES,
    MIN_PRESET_VERSION,
    TEST_FAMILY,
)
from .environment import (
    _program_stem,
    prepare_environment as default_prepare_environment,
    program_available,
    toolchain_family,
)
from .jsonutil import _display_value, _object_field, _string_field, eval_condition
from .models import Issue, Plan
from .presets import (
    AnalysisContext,
    PresetCatalog,
    _build_mappings,
    _resolve_all,
    _workflow_mapping,
)

if TYPE_CHECKING:
    from collections.abc import Sequence
    from pathlib import Path

    from .hooks import ProjectHooks
    from .models import JsonObject, RunOptions


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
    if not plan.workflow:
        issues.append(
            Issue(
                "warning",
                plan.name,
                "no workflow preset drives this configure preset; "
                "the stages run one by one instead of as a single workflow",
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
        workflow=context.workflow_map.get(name),
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
    if context.extra_issues is not None:
        plan.issues.extend(context.extra_issues(plan))
    environment = context.prepare_environment(plan.family, plan.arch)
    plan.notes.extend(environment.notes)
    if environment.env is None:
        plan.skip_reason = environment.skip_reason or "toolchain is unavailable"
        return plan
    plan.env = environment.env
    plan.skip_reason = _missing_program_reason(plan.cache, plan.env)
    return plan


def analyse(
    catalog: PresetCatalog,
    options: RunOptions,
    hooks: ProjectHooks | None = None,
) -> tuple[list[Plan], list[Issue]]:
    """Validate the preset matrix and build one plan per selected preset."""
    prepare = (
        hooks.prepare_environment
        if hooks is not None and hooks.prepare_environment is not None
        else default_prepare_environment
    )
    extra_issues = hooks.extra_issues if hooks is not None else None
    issues: list[Issue] = [*catalog.issues, *_version_issues(catalog)]
    effective, resolve_issues = _resolve_all(catalog)
    issues.extend(resolve_issues)
    build_map, test_map, mapping_issues = _build_mappings(catalog, effective)
    issues.extend(mapping_issues)
    workflow_map, workflow_issues = _workflow_mapping(catalog, effective)
    issues.extend(workflow_issues)
    visible = [
        name
        for name in catalog.visible_names(CONFIGURE_FAMILY)
        if _matches(name, options.includes, options.excludes)
    ]
    context = AnalysisContext(
        catalog=catalog,
        source_dir=options.source_dir,
        binary_root=options.binary_root,
        effective=effective,
        build_map=build_map,
        test_map=test_map,
        workflow_map=workflow_map,
        prepare_environment=prepare,
        extra_issues=extra_issues,
    )
    used_slugs: set[str] = set()
    plans: list[Plan] = []
    for name in visible:
        plan = _create_plan(context, name, used_slugs)
        plans.append(plan)
        issues.extend(plan.issues)
    return plans, issues
