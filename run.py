#!/usr/bin/env python3
"""Build and run the Cutline desktop application.

Examples:
    python run.py --demo
    python run.py --open "D:/Projects/My Film.cutline"
    python run.py --build --demo
    python run.py --demo --do trigger:workspace.color
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parent
BUILD_DIR = ROOT / "build" / "app"
SOURCE_DIRS = (
    "app",
    "audio",
    "captions",
    "commands",
    "effects",
    "exporter",
    "media",
    "model",
    "playback",
    "project",
    "render",
    "speech",
    "time",
    "timeline",
    "third_party/sqlite3",
    "ui",
)
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hpp", ".qml"}


def executable_path() -> Path:
    return BUILD_DIR / ("Cutline.exe" if os.name == "nt" else "Cutline")


def newest_source_time() -> float:
    candidates = [ROOT / "CMakeLists.txt", ROOT / "scripts" / "build-app.bat"]
    for directory_name in SOURCE_DIRS:
        directory = ROOT / directory_name
        if not directory.is_dir():
            continue
        candidates.extend(path for path in directory.rglob("*") if path.suffix.lower() in SOURCE_SUFFIXES)
    return max((path.stat().st_mtime for path in candidates if path.is_file()), default=0.0)


def needs_build(app: Path) -> bool:
    return not app.is_file() or newest_source_time() > app.stat().st_mtime


def build() -> None:
    if os.name == "nt":
        script = ROOT / "scripts" / "build-app.bat"
        command = ["cmd.exe", "/d", "/c", str(script), "--target", "Cutline"]
    else:
        cmake = shutil.which("cmake")
        if cmake is None:
            raise RuntimeError("CMake was not found on PATH")
        if not (BUILD_DIR / "CMakeCache.txt").is_file():
            raise RuntimeError(
                "build/app is not configured. Configure it with a Qt desktop toolchain first, "
                "then run this command again."
            )
        command = [cmake, "--build", str(BUILD_DIR), "--target", "Cutline"]

    print("Building Cutline...", flush=True)
    completed = subprocess.run(command, cwd=ROOT)
    if completed.returncode != 0:
        raise SystemExit(completed.returncode)


def runtime_environment() -> dict[str, str]:
    environment = os.environ.copy()
    paths = [str(BUILD_DIR)]
    if os.name == "nt":
        qt_root = Path(
            environment.get(
                "CUTLINE_QT_ROOT",
                str(ROOT / ".tools" / "qt" / "6.8.3" / "msvc2022_64"),
            )
        )
        qt_bin = qt_root / "bin"
        if not (qt_bin / "Qt6Core.dll").is_file():
            raise RuntimeError(
                f"Qt was not found at {qt_root}. Set CUTLINE_QT_ROOT to a Qt 6.5+ MSVC installation."
            )
        paths.insert(0, str(qt_bin))
    environment["PATH"] = os.pathsep.join(paths + [environment.get("PATH", "")])
    return environment


def parse_arguments() -> tuple[argparse.Namespace, list[str]]:
    parser = argparse.ArgumentParser(description="Build and run the Cutline desktop editor.")
    build_group = parser.add_mutually_exclusive_group()
    build_group.add_argument("--build", action="store_true", help="force an incremental build before launching")
    build_group.add_argument("--no-build", action="store_true", help="launch the existing binary without checking sources")
    parser.add_argument("--demo", action="store_true", help="open Cutline's ready-made demonstration project")
    parser.add_argument("--open", metavar="PROJECT", help="open an existing .cutline project folder")
    parser.add_argument("--detach", action="store_true", help="launch Cutline and return immediately")
    options, forwarded = parser.parse_known_args()
    if forwarded and forwarded[0] == "--":
        forwarded = forwarded[1:]
    return options, forwarded


def main() -> int:
    options, forwarded = parse_arguments()
    app = executable_path()
    if options.build or (not options.no_build and needs_build(app)):
        build()
    if not app.is_file():
        raise RuntimeError(f"Cutline was not built at {app}")

    arguments: list[str] = []
    if options.demo:
        arguments.append("--demo")
    if options.open:
        arguments.extend(("--open", options.open))
    arguments.extend(forwarded)

    command = [str(app), *arguments]
    environment = runtime_environment()
    if options.detach:
        creation_flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
        subprocess.Popen(command, cwd=ROOT, env=environment, creationflags=creation_flags)
        print(f"Started {app.name}")
        return 0
    return subprocess.run(command, cwd=ROOT, env=environment).returncode


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"run.py: {error}", file=sys.stderr)
        raise SystemExit(1)
