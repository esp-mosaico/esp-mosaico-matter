#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Run SDK-independent cppcheck on each first-party component separately."""

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx"}
EXCLUDED_DIRS = {"tests", "test", "stubs", "build", "managed_components", "third_party"}


def main() -> int:
    cppcheck = shutil.which("cppcheck")
    if not cppcheck:
        print("cppcheck is required (Ubuntu/Debian: sudo apt install cppcheck).", file=sys.stderr)
        return 1

    # Include new files during initial setup, without traversing submodules,
    # ignored build output or downloaded SDK sources.
    result = subprocess.run(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard", "--", "components"],
        cwd=ROOT, check=True, capture_output=True,
    )
    components: dict[str, list[Path]] = {}
    for name in sorted(set(result.stdout.decode().split("\0")) - {""}):
        path = Path(name)
        if (
            len(path.parts) < 3
            or path.suffix not in SOURCE_SUFFIXES
            or EXCLUDED_DIRS.intersection(path.parts[2:-1])
            or not (ROOT / path).is_file()
        ):
            continue
        components.setdefault(path.parts[1], []).append(path)

    if not components:
        print("No component sources found.", file=sys.stderr)
        return 1

    # Only project headers are supplied. This is a quick baseline; target SDK
    # configuration and headers belong in a compile_commands.json analysis.
    includes = []
    for component in sorted((ROOT / "components").iterdir()):
        for directory in (component, component / "include", component / "private", component / "dmgr"):
            if directory.is_dir():
                includes.extend(["-I", str(directory.relative_to(ROOT))])

    failed = False
    with tempfile.TemporaryDirectory(prefix="esp-claw-cppcheck-") as temporary:
        file_list = Path(temporary) / "sources.txt"
        for component, sources in sorted(components.items()):
            file_list.write_text("".join(f"{path.as_posix()}\n" for path in sources))
            print(f"cppcheck: {component} ({len(sources)} sources)", flush=True)
            command = [
                cppcheck, "--quiet", "--std=c11", "--std=c++17",
                "--enable=warning,performance,portability", "--error-exitcode=1",
                "--inline-suppr", "--library=posix",
                "--suppressions-list=tools/cppcheck/.cppcheck-suppressions",
                *includes, f"--file-list={file_list}",
            ]
            if subprocess.run(command, cwd=ROOT, check=False).returncode != 0:
                failed = True
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
