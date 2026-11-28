"""Toolchain environment discovery.

The generic defaults cover MSYS2 UCRT64 and MSVC (visual Studio) since they are
the common Windows toolchains; a project can replace the whole policy through
:attr:`cmake_workflow.hooks.ProjectHooks.prepare_environment`.
"""

from __future__ import annotations

import contextlib
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import TYPE_CHECKING

from .models import EnvironmentResult

if TYPE_CHECKING:
    from collections.abc import Sequence


def _program_stem(value: str | None) -> str:
    """Return the lower-case executable name without a Windows extension."""
    text = (value or "").strip().strip('"').replace("\\", "/").rstrip("/")
    name = text.rsplit("/", 1)[-1].lower()
    return name.removesuffix(".exe")


def program_available(value: str | None, env: dict[str, str]) -> bool:
    """Return whether a program from a preset cache variable is available."""
    text = (value or "").strip()
    if not text:
        return True
    first = text.split(";")[0].strip().strip('"')
    if Path(first).is_absolute() or "/" in first or "\\" in first:
        return Path(first).is_file()
    return shutil.which(first, path=env.get("PATH", "")) is not None


def toolchain_family(cache: dict[str, str]) -> str:
    """Classify the toolchain family from the preset or the compilers."""
    kind = cache.get("MSYS2_UCRT64_COMPILER_KIND")
    if kind in {"clang", "gcc"}:
        return kind
    compiler = cache.get("CMAKE_CXX_COMPILER") or cache.get("CMAKE_C_COMPILER") or ""
    stem = _program_stem(compiler)
    if stem.startswith("clang"):
        return "clang"
    if stem in {"gcc", "g++", "cc", "c++"} or stem.startswith(("gcc", "g++")):
        return "gcc"
    if stem in {"cl", "clang-cl"}:
        return "msvc"
    return "unknown"


def find_msys2_ucrt64() -> Path | None:
    """Locate the MSYS2 UCRT64 ``bin`` directory."""
    roots: list[Path] = []
    env_root = os.environ.get("MSYS2_ROOT")
    if env_root:
        roots.append(Path(env_root))
    roots.extend(
        Path(candidate)
        for candidate in (
            r"C:\msys64",
            r"C:\msys2",
            r"C:\tools\msys64",
            r"C:\tools\msys2",
        )
    )
    for root in roots:
        binary_dir = root / "ucrt64" / "bin"
        if (binary_dir / "clang++.exe").is_file() or (binary_dir / "g++.exe").is_file():
            return binary_dir
    for executable in ("clang++.exe", "g++.exe"):
        found = shutil.which(executable)
        if found:
            binary_dir = Path(found).parent
            if binary_dir.name.lower() == "bin" and binary_dir.parent.name.lower() == "ucrt64":
                return binary_dir
    return None


def find_vswhere() -> Path | None:
    """Locate ``vswhere.exe`` from the Visual Studio installer."""
    candidates: list[Path] = []
    program_files_x86 = os.environ.get("PROGRAMFILES(X86)")
    if program_files_x86:
        candidates.append(
            Path(program_files_x86) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
        )
    found = shutil.which("vswhere.exe")
    if found:
        candidates.append(Path(found))
    return next((candidate for candidate in candidates if candidate.is_file()), None)


def _console_encoding() -> str:
    encoding = getattr(sys.stdout, "encoding", None)
    return encoding or "utf-8"


def _run_vswhere(vswhere: Path, arguments: Sequence[str]) -> list[str]:
    with contextlib.suppress(OSError, subprocess.SubprocessError):
        result = subprocess.run(
            [str(vswhere), *arguments],
            capture_output=True,
            text=True,
            encoding=_console_encoding(),
            errors="replace",
            timeout=60,
            check=False,
        )
        if result.returncode == 0:
            return result.stdout.splitlines()
    return []


def _vcvarsall_from_vswhere(vswhere: Path) -> Path | None:
    lines = _run_vswhere(
        vswhere,
        [
            "-latest",
            "-products",
            "*",
            "-requires",
            "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
            "-find",
            r"VC\Auxiliary\Build\vcvarsall.bat",
        ],
    )
    for line in lines:
        candidate = Path(line.strip())
        if candidate.is_file():
            return candidate
    lines = _run_vswhere(
        vswhere,
        [
            "-latest",
            "-products",
            "*",
            "-requires",
            "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
            "-property",
            "installationPath",
        ],
    )
    for line in lines:
        candidate = Path(line.strip()) / "VC" / "Auxiliary" / "Build" / "vcvarsall.bat"
        if candidate.is_file():
            return candidate
    return None


def _visual_studio_roots() -> list[Path]:
    roots: list[Path] = []
    for variable in ("VSINSTALLDIR", "PROGRAMFILES", "PROGRAMFILES(X86)"):
        value = os.environ.get(variable)
        if not value:
            continue
        base = Path(value)
        roots.append(base if variable == "VSINSTALLDIR" else base / "Microsoft Visual Studio")
    return roots


def _vcvarsall_from_roots() -> Path | None:
    for root in _visual_studio_roots():
        if not root.is_dir():
            continue
        for pattern in (
            "*/VC/Auxiliary/Build/vcvarsall.bat",
            "*/*/VC/Auxiliary/Build/vcvarsall.bat",
        ):
            for candidate in sorted(root.glob(pattern), reverse=True):
                if candidate.is_file():
                    return candidate
    return None


def find_vcvarsall() -> Path | None:
    """Locate ``vcvarsall.bat`` for the installed Visual Studio."""
    vswhere = find_vswhere()
    if vswhere is not None:
        found = _vcvarsall_from_vswhere(vswhere)
        if found is not None:
            return found
    return _vcvarsall_from_roots()


def _merge_environment(base: dict[str, str], extra: dict[str, str]) -> dict[str, str]:
    result = dict(base)
    index = {key.upper(): key for key in result}
    for key, value in extra.items():
        if key.startswith("="):
            continue
        out_key = key.upper() if os.name == "nt" else key
        previous = index.get(out_key.upper())
        if previous is not None and previous != out_key:
            del result[previous]
        index[out_key.upper()] = out_key
        result[out_key] = value
    return result


def _parse_environment(output: str, base: dict[str, str]) -> dict[str, str]:
    extra: dict[str, str] = {}
    for line in output.splitlines():
        if "=" not in line or line.startswith("="):
            continue
        name, value = line.split("=", 1)
        extra[name] = value
    return _merge_environment(base, extra)


def _run_vcvarsall(vcvarsall: Path, arch_arg: str) -> tuple[dict[str, str] | None, str | None]:
    handle, wrapper_name = tempfile.mkstemp(prefix="verify-presets-vcvars-", suffix=".cmd")
    os.close(handle)
    wrapper = Path(wrapper_name)
    try:
        try:
            wrapper.write_text(
                f'@echo off\r\ncall "{vcvarsall}" {arch_arg} >nul\r\nset\r\n',
                encoding="mbcs" if os.name == "nt" else "utf-8",
            )
            result = subprocess.run(
                ["cmd.exe", "/d", "/c", str(wrapper)],
                capture_output=True,
                text=True,
                encoding=_console_encoding(),
                errors="replace",
                timeout=180,
                check=False,
            )
        except (OSError, subprocess.SubprocessError) as exc:
            return None, f"failed to run vcvarsall.bat: {exc}"
    finally:
        with contextlib.suppress(OSError):
            wrapper.unlink()
    if result.returncode != 0:
        return None, f"vcvarsall.bat {arch_arg} failed with exit code {result.returncode}"
    return _parse_environment(result.stdout, os.environ.copy()), None


def _prepare_msvc(arch: str | None) -> EnvironmentResult:
    if os.name != "nt":
        return EnvironmentResult(None, [], "MSVC presets require Windows")
    vcvarsall = find_vcvarsall()
    if vcvarsall is None:
        return EnvironmentResult(
            None, [], "Visual Studio C++ build tools (vcvarsall.bat) were not found"
        )
    arch_arg = arch if arch in {"x64", "x86", "arm64"} else "x64"
    env, reason = _run_vcvarsall(vcvarsall, arch_arg)
    if env is None:
        return EnvironmentResult(None, [], reason)
    return EnvironmentResult(
        env,
        [f"MSVC {arch_arg} developer environment ({vcvarsall.name})"],
        None,
    )


def _prepare_msys2(arch: str | None) -> EnvironmentResult:
    binary_dir = find_msys2_ucrt64()
    if binary_dir is None:
        return EnvironmentResult(
            None,
            [],
            "MSYS2 UCRT64 toolchain not found; set MSYS2_ROOT or add "
            r"<msys2>\ucrt64\bin to PATH",
        )
    env = os.environ.copy()
    env["PATH"] = str(binary_dir) + os.pathsep + env.get("PATH", "")
    env["MSYSTEM"] = "UCRT64"
    notes = [f"MSYS2 UCRT64 environment ({binary_dir})"]
    if arch is not None and arch.lower() == "x86":
        notes.append(
            "preset targets x86, but only the 64-bit UCRT64 toolchain is "
            "provisioned; the build will target x64"
        )
    return EnvironmentResult(env, notes, None)


_ENV_CACHE: dict[tuple[str, str], EnvironmentResult] = {}


def _copy_environment_result(result: EnvironmentResult) -> EnvironmentResult:
    return EnvironmentResult(
        env=dict(result.env) if result.env is not None else None,
        notes=list(result.notes),
        skip_reason=result.skip_reason,
    )


def prepare_environment(family: str, arch: str | None) -> EnvironmentResult:
    """Prepare the compiler environment for a toolchain family."""
    key = (family, arch or "")
    cached = _ENV_CACHE.get(key)
    if cached is not None:
        return _copy_environment_result(cached)
    if family == "msvc":
        result = _prepare_msvc(arch)
    elif family in {"clang", "gcc"}:
        result = _prepare_msys2(arch)
    else:
        result = EnvironmentResult(
            None, [], "cannot determine the toolchain family from the compilers"
        )
    _ENV_CACHE[key] = result
    return _copy_environment_result(result)
