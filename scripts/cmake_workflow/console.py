"""Console output helpers: styles, status labels and atomic printing."""

from __future__ import annotations

import os
import sys
import threading
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from typing import ClassVar


class Style:
    """Render terminal text with optional ANSI colours."""

    _CODES: ClassVar[dict[str, str]] = {
        "red": "31",
        "green": "32",
        "yellow": "33",
        "cyan": "36",
        "bold": "1",
        "dim": "2",
    }

    def __init__(self, mode: str) -> None:
        """Create a style for ``auto``, ``always`` or ``never`` colour output."""
        if mode == "always":
            self.enabled = True
        elif mode == "never":
            self.enabled = False
        else:
            self.enabled = sys.stdout.isatty() and os.environ.get("NO_COLOR") is None

    def __call__(self, text: str, *styles: str) -> str:
        """Render *text* with the requested ANSI styles."""
        if not self.enabled:
            return text
        prefix = "".join(f"\033[{self._CODES[name]}m" for name in styles)
        return f"{prefix}{text}\033[0m"


def status_text(status: str, style: Style) -> str:
    """Return a padded, coloured status label."""
    labels = {
        "pass": ("PASS", "green"),
        "fail": ("FAIL", "red"),
        "skip": ("SKIP", "yellow"),
        "blocked": ("BLOCKED", "yellow"),
        "not-run": ("NOT RUN", "dim"),
    }
    label, colour = labels.get(status, (status.upper(), "dim"))
    return style(f"{label:<7}", colour)


_PRINT_LOCK = threading.Lock()
_OUTPUT = threading.local()


def _set_output_prefix(prefix: str) -> None:
    """Set the prefix prepended to every line printed by this thread."""
    _OUTPUT.prefix = prefix


def _emit(text: str = "", *, end: str = "\n") -> None:
    """Print one line atomically so concurrent presets do not interleave."""
    prefix = getattr(_OUTPUT, "prefix", "")
    with _PRINT_LOCK:
        if prefix and text:
            print(f"{prefix}{text}", end=end, flush=True)
        else:
            print(text, end=end, flush=True)
