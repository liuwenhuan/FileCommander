#!/usr/bin/env python3
"""Merge two macOS app bundles into one Universal2 bundle."""

from __future__ import annotations

import argparse
import filecmp
import os
from pathlib import Path
import shutil
import subprocess
import sys


MACH_O_MAGICS = {
    b"\xfe\xed\xfa\xce",
    b"\xce\xfa\xed\xfe",
    b"\xfe\xed\xfa\xcf",
    b"\xcf\xfa\xed\xfe",
    b"\xca\xfe\xba\xbe",
    b"\xbe\xba\xfe\xca",
}


def is_macho(path: Path) -> bool:
    try:
        with path.open("rb") as handle:
            return handle.read(4) in MACH_O_MAGICS
    except (OSError, IsADirectoryError):
        return False


def run_lipo(x86_path: Path, arm64_path: Path, output_path: Path) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    temporary_path = output_path.with_name(output_path.name + ".universal2.tmp")
    if temporary_path.exists() or temporary_path.is_symlink():
        temporary_path.unlink()
    subprocess.run(
        [
            "/usr/bin/lipo",
            "-create",
            str(x86_path),
            str(arm64_path),
            "-output",
            str(temporary_path),
        ],
        check=True,
    )
    os.replace(temporary_path, output_path)
    shutil.copymode(x86_path, output_path)


def relative_files(root: Path) -> set[Path]:
    return {
        path.relative_to(root)
        for path in root.rglob("*")
        if not path.is_dir()
    }


def copy_entry(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    if source.is_symlink():
        destination.symlink_to(os.readlink(source))
    else:
        shutil.copy2(source, destination)


def merge_bundle(x86_app: Path, arm64_app: Path, output_app: Path) -> None:
    if not x86_app.is_dir() or not arm64_app.is_dir():
        raise SystemExit("both architecture inputs must be existing .app directories")
    if output_app.exists() or output_app.is_symlink():
        shutil.rmtree(output_app)

    shutil.copytree(x86_app, output_app, symlinks=True)
    x86_files = relative_files(x86_app)
    arm64_files = relative_files(arm64_app)

    for relative_path in sorted(arm64_files - x86_files):
        copy_entry(arm64_app / relative_path, output_app / relative_path)

    for relative_path in sorted(x86_files & arm64_files):
        x86_path = x86_app / relative_path
        arm64_path = arm64_app / relative_path
        output_path = output_app / relative_path

        if x86_path.is_symlink() or arm64_path.is_symlink():
            if x86_path.is_symlink() and arm64_path.is_symlink():
                if os.readlink(x86_path) != os.readlink(arm64_path):
                    raise SystemExit(
                        f"architecture bundles disagree on symlink: {relative_path}"
                    )
                continue
            raise SystemExit(f"architecture bundles disagree on file type: {relative_path}")

        if is_macho(x86_path) or is_macho(arm64_path):
            if not is_macho(x86_path) or not is_macho(arm64_path):
                raise SystemExit(
                    f"only one architecture contains a Mach-O file: {relative_path}"
                )
            run_lipo(x86_path, arm64_path, output_path)
        elif not filecmp.cmp(x86_path, arm64_path, shallow=False):
            raise SystemExit(
                f"architecture bundles disagree on non-Mach-O file: {relative_path}"
            )


def verify_bundle(app: Path) -> None:
    failures: list[str] = []
    for path in sorted(relative_files(app)):
        absolute_path = app / path
        if not is_macho(absolute_path):
            continue
        result = subprocess.run(
            ["/usr/bin/lipo", "-info", str(absolute_path)],
            check=False,
            capture_output=True,
            text=True,
        )
        output = f"{result.stdout}\n{result.stderr}"
        if result.returncode != 0 or "x86_64" not in output or "arm64" not in output:
            failures.append(str(path))
    if failures:
        raise SystemExit(
            "Universal2 verification failed for:\n  " + "\n  ".join(failures)
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--x86-app", type=Path)
    parser.add_argument("--arm64-app", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--verify-only", action="store_true")
    args = parser.parse_args()

    if args.verify_only:
        verify_bundle(args.output)
    else:
        if args.x86_app is None or args.arm64_app is None:
            parser.error("--x86-app and --arm64-app are required unless --verify-only is used")
        merge_bundle(args.x86_app, args.arm64_app, args.output)
        verify_bundle(args.output)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as error:
        command = " ".join(error.cmd)
        print(f"error: command failed ({error.returncode}): {command}", file=sys.stderr)
        raise
