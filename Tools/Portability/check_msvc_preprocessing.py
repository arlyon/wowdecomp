#!/usr/bin/env python3
"""Compare active MSVC-macro source against a Git baseline, not binary matching.

Uses the local Clang preprocessor and host headers. A VFS overlay gives baseline
files their original paths so __FILE__ and __LINE__ differences remain visible.
"""

import argparse
import difflib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_PATHS = [
    "Engine/Source/Event/Types.h",
    "Engine/Source/Os/Gui.h",
    "Engine/Source/Storm/H/Types.h",
    "Engine/Source/Storm/H/stpl.h",
    "Engine/Source/Tempest/CMath.cpp",
    "Engine/Source/Tempest/CRandom.cpp",
    "Engine/Source/Tempest/CRandom.h",
]


def run(command):
    return subprocess.run(
        command, cwd=ROOT, check=True, text=True, capture_output=True
    ).stdout


def active_lines(text):
    # Ignore only blank lines; preserve string-literal contents and expansions
    # of __FILE__/__LINE__, unlike a whitespace/token split of the output.
    return [line for line in text.splitlines(keepends=True) if line.strip()]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", default="HEAD", help="Git revision to compare against")
    parser.add_argument("--compiler", default="clang++")
    parser.add_argument("paths", nargs="*", default=DEFAULT_PATHS)
    args = parser.parse_args()
    print("Preprocessing-only check: host headers, NOT an MSVC build or binary-match check.")
    failures = 0
    with tempfile.TemporaryDirectory(prefix="wowdecomp-preprocess-") as temp:
        mappings = []
        for index, path in enumerate(args.paths):
            original = run(["git", "show", f"{args.base}:{path}"])
            current = (ROOT / path).read_text()
            metadata = r"^// (?:VERIFY|OFFSET|STATIC):.*$"
            if re.findall(metadata, original, re.MULTILINE) != re.findall(
                metadata, current, re.MULTILINE
            ):
                print(f"FAIL: verification/address metadata changed: {path}")
                failures += 1
            snapshot = Path(temp) / f"{index}.source"
            snapshot.write_text(original)
            mappings.append({
                "type": "file",
                "name": str(ROOT / path),
                "external-contents": str(snapshot),
            })
        overlay = Path(temp) / "baseline.json"
        overlay.write_text(json.dumps({
            "version": 0,
            "use-external-names": False,
            "roots": mappings,
        }))
        for mode, defines in [
            ("Win32", ["-DWOWDECOMP_ARCH_32=1", "-D_USE_32BIT_TIME_T"]),
            ("Win64", ["-D_WIN64=1", "-DWOWDECOMP_ARCH_64=1"]),
        ]:
            for path in args.paths:
                command = [
                    args.compiler, "-std=c++17", "-E", "-P", "-x", "c++",
                    "-D_WIN32=1", "-D_MSC_VER=1930", "-DWIN32", "-D_WINDOWS",
                    "-DNOMINMAX", "-DWIN32_LEAN_AND_MEAN", "-D_CRT_SECURE_NO_WARNINGS",
                    *defines, f"-I{ROOT}", str(ROOT / path),
                ]
                before = active_lines(run(command + ["-ivfsoverlay", str(overlay)]))
                after = active_lines(run(command))
                if before != after:
                    failures += 1
                    print(f"FAIL: {mode} macro-mode: {path}")
                    diff = list(difflib.unified_diff(
                        before, after, fromfile=f"{args.base}/{path}", tofile=path
                    ))
                    sys.stdout.writelines(diff[:60])
                else:
                    print(f"PASS: {mode} macro-mode: {path}")
    return int(failures != 0)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as error:
        print(error.stderr or str(error), file=sys.stderr)
        sys.exit(1)
    except OSError as error:
        print(error, file=sys.stderr)
        sys.exit(1)
