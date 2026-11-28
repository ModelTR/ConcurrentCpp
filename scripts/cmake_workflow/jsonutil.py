"""JSON helpers and CMake preset condition evaluation."""

from __future__ import annotations

import json
import platform
import re
from typing import TYPE_CHECKING

from .constants import MACRO_RE

if TYPE_CHECKING:
    from pathlib import Path
    from typing import TypeGuard

    from .models import JsonObject, JsonValue


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
