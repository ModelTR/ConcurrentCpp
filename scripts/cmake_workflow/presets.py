"""Preset loading, inheritance resolution and derived-preset generation."""

from __future__ import annotations

import copy
import json
from collections import OrderedDict, defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING

from .constants import (
    BUILD_FAMILY,
    CONFIGURE_FAMILY,
    FAMILIES,
    MIN_PRESET_VERSION,
    TEST_FAMILY,
    WORKFLOW_FAMILY,
)
from .jsonutil import (
    _bool_field,
    _expand_macros,
    _expand_text,
    _is_list,
    _is_object,
    _pairs_hook,
    _string_field,
    _string_list,
)
from .models import Issue, PresetError

if TYPE_CHECKING:
    from collections.abc import Callable, Sequence

    from .models import EnvironmentResult, JsonObject, JsonValue, Plan


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
        if not isinstance(data, dict):
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


@dataclass(frozen=True)
class AnalysisContext:
    """Immutable inputs shared while building plans."""

    catalog: PresetCatalog
    source_dir: Path
    binary_root: Path
    effective: dict[tuple[str, str], JsonObject]
    build_map: dict[str, list[str]]
    test_map: dict[str, list[str]]
    workflow_map: dict[str, str]
    prepare_environment: Callable[[str, str | None], EnvironmentResult]
    extra_issues: Callable[[Plan], list[Issue]] | None


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


def _workflow_mapping(
    catalog: PresetCatalog, effective: dict[tuple[str, str], JsonObject]
) -> tuple[dict[str, str], list[Issue]]:
    """Map each configure preset to the workflow preset that drives it."""
    mapping: dict[str, str] = {}
    issues: list[Issue] = []
    for name in catalog.families[WORKFLOW_FAMILY]:
        preset = effective.get((WORKFLOW_FAMILY, name))
        if preset is None or _bool_field(preset, "hidden"):
            continue
        steps_value = preset.get("steps")
        if not _is_list(steps_value):
            issues.append(Issue("error", name, "workflow preset has no steps"))
            continue
        configure = next(
            (
                _string_field(step, "name")
                for step in steps_value
                if _is_object(step) and _string_field(step, "type") == "configure"
            ),
            None,
        )
        if not configure:
            issues.append(Issue("error", name, "workflow preset has no configure step"))
            continue
        if configure in mapping:
            issues.append(
                Issue(
                    "warning",
                    name,
                    f"duplicate workflow for configure preset {configure!r}; "
                    f"keeping {mapping[configure]!r}",
                )
            )
            continue
        mapping[configure] = name
    return mapping, issues


def _flattened_preset(preset: JsonObject, origin_file: Path, source_dir: Path) -> JsonObject:
    flattened = _expand_macros(copy.deepcopy(preset), source_dir, origin_file.parent)
    if not _is_object(flattened):
        message = "internal error: flattened preset is not an object"
        raise PresetError(message)
    flattened.pop("inherits", None)
    flattened.pop("hidden", None)
    return flattened


def _workflow_presets(
    plans: Sequence[Plan], catalog: PresetCatalog, source_dir: Path
) -> list[JsonValue]:
    """Collect the workflow presets that drive the selected plans."""
    presets: list[JsonValue] = []
    seen: set[str] = set()
    for plan in plans:
        if plan.skip_reason or not plan.workflow or plan.workflow in seen:
            continue
        seen.add(plan.workflow)
        presets.append(
            _flattened_preset(
                catalog.resolve(WORKFLOW_FAMILY, plan.workflow),
                catalog.origin(WORKFLOW_FAMILY, plan.workflow),
                source_dir,
            )
        )
    return presets


def _configure_entry(plan: Plan, source_dir: Path) -> JsonObject:
    """Flatten one configure preset with its isolated directories."""
    preset = _flattened_preset(plan.effective, plan.origin_file, source_dir)
    preset["binaryDir"] = plan.build_dir.as_posix()
    if "installDir" in preset:
        preset["installDir"] = plan.install_dir.as_posix()
    return preset


def _plan_entries(
    plan: Plan,
    family: str,
    catalog: PresetCatalog,
    source_dir: Path,
    jobs: int | None = None,
) -> list[JsonObject]:
    """Flatten the build or test presets that belong to one plan."""
    names = plan.build_presets if family == BUILD_FAMILY else plan.test_presets
    effective = plan.build_effective if family == BUILD_FAMILY else plan.test_effective
    entries: list[JsonObject] = []
    for name in names:
        entry = _flattened_preset(effective[name], catalog.origin(family, name), source_dir)
        if jobs and family == BUILD_FAMILY:
            entry["jobs"] = jobs
        entries.append(entry)
    return entries


def _add_unique(target: list[JsonObject], seen: set[str], entries: Sequence[JsonObject]) -> None:
    """Append the entries whose name has not been emitted yet."""
    for entry in entries:
        name = _string_field(entry, "name")
        if name and name not in seen:
            seen.add(name)
            target.append(entry)


def write_derived_presets(
    plans: Sequence[Plan],
    catalog: PresetCatalog,
    source_dir: Path,
    path: Path,
    jobs: int | None = None,
) -> None:
    """Write flattened presets with isolated binary/install directories."""
    configure_presets: list[JsonObject] = []
    build_presets: list[JsonObject] = []
    test_presets: list[JsonObject] = []
    seen_build: set[str] = set()
    seen_test: set[str] = set()
    for plan in plans:
        if plan.skip_reason:
            continue
        configure_presets.append(_configure_entry(plan, source_dir))
        _add_unique(
            build_presets,
            seen_build,
            _plan_entries(plan, BUILD_FAMILY, catalog, source_dir, jobs),
        )
        _add_unique(test_presets, seen_test, _plan_entries(plan, TEST_FAMILY, catalog, source_dir))
    workflow_presets = _workflow_presets(plans, catalog, source_dir)
    payload: dict[str, object] = {
        "version": catalog.version or MIN_PRESET_VERSION,
        "configurePresets": configure_presets,
        "buildPresets": build_presets,
        "testPresets": test_presets,
    }
    if workflow_presets:
        payload["workflowPresets"] = workflow_presets
    if catalog.cmake_minimum_required is not None:
        payload["cmakeMinimumRequired"] = copy.deepcopy(catalog.cmake_minimum_required)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
