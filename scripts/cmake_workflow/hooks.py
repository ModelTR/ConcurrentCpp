"""Project-specific extension points for the generic verifier."""

from __future__ import annotations

from dataclasses import dataclass
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from collections.abc import Callable, Sequence
    from pathlib import Path

    from .models import EnvironmentResult, Issue, Plan


@dataclass(frozen=True)
class ProjectHooks:
    """Project-specific extensions injected into the generic verifier."""

    prepare_environment: Callable[[str, str | None], EnvironmentResult] | None = None
    extra_issues: Callable[[Plan], list[Issue]] | None = None
    configure_environment: Callable[[Sequence[Plan], Path, bool], str | None] | None = None
