#!/usr/bin/env python3
"""Generate FileCommander's static, announcement-only update site.

The generator is deliberately the only source of version.json, update.html, and
SHA256SUMS.txt. It derives hashes and sizes from the final packages so a manifest
cannot accidentally advertise a package that was not staged for publication.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import html
import json
import re
import shutil
import sys
from pathlib import Path
from string import Template

ORIGIN = "https://fc.aigutta.com"
# The desktop checker refuses manifests at 64 KiB. Reserve 4 KiB so serialized
# metadata has headroom if fields grow slightly in a compatible future release.
MAX_MANIFEST_BYTES = 60 * 1024
MAX_VERSION_COMPONENT = 2_147_483_647
VERSION_RE = re.compile(r"^[0-9]+(?:\.[0-9]+){2}$")
DATE_RE = re.compile(r"^[0-9]{4}-[0-9]{2}-[0-9]{2}$")


def fail(message: str) -> None:
    raise ValueError(message)


def sha256_and_size(path: Path) -> tuple[str, int]:
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as artifact:
        for chunk in iter(lambda: artifact.read(1024 * 1024), b""):
            digest.update(chunk)
            size += len(chunk)
    return digest.hexdigest(), size


def package_specs(version: str) -> list[tuple[str, str, str, str]]:
    return [
        ("windows", "x86_64", f"FileCommander-{version}-windows-x64.zip", "Windows x64 portable ZIP"),
        ("deb", "x86_64", f"FileCommander_{version}_amd64.deb", "Linux x86_64 DEB"),
        ("rpm", "x86_64", f"FileCommander-{version}-1.x86_64.rpm", "Linux x86_64 RPM"),
        ("appimage", "x86_64", f"FileCommander-{version}-x86_64.AppImage", "Linux x86_64 AppImage"),
    ]


def release_model(version: str, date: str, notes: str, artifacts: list[Path]) -> tuple[dict, list[dict]]:
    if not VERSION_RE.fullmatch(version):
        fail("version must be X.Y.Z with numeric components")
    if any(int(component) > MAX_VERSION_COMPONENT for component in version.split(".")):
        fail(f"version components must not exceed {MAX_VERSION_COMPONENT}")
    if not DATE_RE.fullmatch(date):
        fail("date must be ISO YYYY-MM-DD")
    try:
        dt.date.fromisoformat(date)
    except ValueError:
        fail("date must be a real ISO calendar date")

    expected = package_specs(version)
    actual = {path.name: path for path in artifacts}
    if len(actual) != len(artifacts):
        fail("artifact names must be unique")
    expected_names = {name for _, _, name, _ in expected}
    if set(actual) != expected_names:
        missing = sorted(expected_names - set(actual))
        extra = sorted(set(actual) - expected_names)
        fail(f"artifacts must exactly match this release (missing={missing}, extra={extra})")
    if any(not path.is_file() for path in artifacts):
        fail("every artifact must be a regular file")

    packages: dict[str, dict[str, dict[str, object]]] = {}
    rows: list[dict] = []
    for package_type, architecture, name, label in expected:
        digest, size = sha256_and_size(actual[name])
        url = f"{ORIGIN}/{name}"
        entry = {"filename": name, "url": url, "sha256": digest, "size": size}
        packages.setdefault(package_type, {})[architecture] = entry
        rows.append({"label": label, **entry})

    return ({"schema": 2, "version": version, "date": date, "notes": notes, "packages": packages}, rows)


def render_html(template_path: Path, manifest: dict, rows: list[dict]) -> str:
    template = Template(template_path.read_text(encoding="utf-8"))
    rendered_rows = "\n".join(
        "<tr><td>{label}</td><td><a href=\"{url}\">{filename}</a></td>"
        "<td>{size:,} bytes</td><td><code>{sha256}</code></td></tr>".format(
            label=html.escape(str(row["label"])),
            url=html.escape(str(row["url"]), quote=True),
            filename=html.escape(str(row["filename"])),
            size=int(row["size"]),
            sha256=html.escape(str(row["sha256"])),
        )
        for row in rows
    )
    return template.substitute(
        version=html.escape(str(manifest["version"])),
        date=html.escape(str(manifest["date"])),
        notes=html.escape(str(manifest["notes"])),
        package_rows=rendered_rows,
    )


def write_atomically(path: Path, content: bytes) -> None:
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_bytes(content)
    temporary.replace(path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True)
    parser.add_argument("--date", required=True)
    parser.add_argument("--notes-file", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--template", type=Path,
                        default=Path(__file__).with_name("templates") / "update.html.template")
    parser.add_argument("artifacts", nargs=4, type=Path)
    args = parser.parse_args()

    notes = args.notes_file.read_text(encoding="utf-8")
    manifest, rows = release_model(args.version, args.date, notes, args.artifacts)
    manifest_bytes = (json.dumps(manifest, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
    if len(manifest_bytes) > MAX_MANIFEST_BYTES:
        fail(f"generated version.json exceeds the {MAX_MANIFEST_BYTES} byte release limit")
    if not args.template.is_file():
        fail(f"template does not exist: {args.template}")

    args.output.mkdir(parents=True, exist_ok=True)
    for artifact in args.artifacts:
        destination = args.output / artifact.name
        if destination.resolve() != artifact.resolve():
            shutil.copy2(artifact, destination)

    write_atomically(args.output / "version.json", manifest_bytes)
    write_atomically(args.output / "update.html",
                     render_html(args.template, manifest, rows).encode("utf-8"))
    checksums = "".join(f"{row['sha256']}  {row['filename']}\n" for row in rows)
    write_atomically(args.output / "SHA256SUMS.txt", checksums.encode("ascii"))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print(f"generate-update-site: {error}", file=sys.stderr)
        raise SystemExit(2)
