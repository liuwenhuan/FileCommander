#!/usr/bin/env python3
"""A stand-in for the release server, for testing the online updater end to end.

It serves exactly what docs/UPDATE_SERVER.md specifies -- a version.json
manifest plus the package files it points at -- over plain HTTP on localhost,
and computes every SHA-256 itself so the manifest can never disagree with what
is actually on disk. That last part matters: a hand-written manifest with a
stale hash is indistinguishable, from the client's side, from a tampered
download, and you would spend the afternoon debugging the wrong half.

Usage
-----
    python tools/mock-update-server.py --version 9.9.9 \
        --package dist/FileCommander-0.2.0-phase2-test-windows-x64.zip

then point a build at it:

    set FILECOMMANDER_UPDATE_MANIFEST_URL=http://127.0.0.1:8765/version.json
    dist\\FileCommander-windows-x64\\FileCommander.exe

The segment key is chosen from the package's suffix (.zip -> windows,
.AppImage -> appimage, .deb -> deb) unless --segment says otherwise.

Fault injection, for the paths that are meant to fail:

    --corrupt-hash     advertise a hash that does not match the package
    --stall-manifest N accept the manifest request, then say nothing for N s
    --fail-download    answer the package request with 500
    --drop-after N     send N bytes of the package, then hang up (once, unless
                       --drop-every), so the client has to resume
    --drop-every       cut every package response short, not just the first
    --no-ranges        ignore Range requests and always send the whole file,
                       the way a server without range support behaves
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SEGMENT_BY_SUFFIX = {
    ".zip": "windows",
    ".appimage": "appimage",
    ".deb": "deb",
}


def sha256_of(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


class Config:
    def __init__(self, args: argparse.Namespace) -> None:
        self.package = os.path.abspath(args.package)
        if not os.path.isfile(self.package):
            raise SystemExit(f"no such package: {self.package}")

        suffix = os.path.splitext(self.package)[1].lower()
        self.segment = args.segment or SEGMENT_BY_SUFFIX.get(suffix)
        if not self.segment:
            raise SystemExit(
                f"cannot infer a manifest segment from '{suffix}'; pass --segment"
            )

        self.version = args.version
        self.date = args.date
        self.notes = args.notes
        self.host = args.host
        self.port = args.port
        self.stall_manifest = args.stall_manifest
        self.fail_download = args.fail_download
        self.drop_after = args.drop_after
        self.drop_every = args.drop_every
        self.no_ranges = args.no_ranges
        self.drops_served = 0

        self.real_sha256 = sha256_of(self.package)
        # A validator the client can send back as If-Range, so the server can
        # tell a resumed download of THIS build from one of an older build.
        self.etag = f'"{self.real_sha256[:16]}"'
        self.advertised_sha256 = (
            "0" * 64 if args.corrupt_hash else self.real_sha256
        )
        self.package_name = os.path.basename(self.package)

    def manifest(self) -> bytes:
        body = {
            "version": self.version,
            "date": self.date,
            "notes": self.notes,
            self.segment: {
                "url": f"http://{self.host}:{self.port}/packages/{self.package_name}",
                "sha256": self.advertised_sha256,
            },
        }
        return json.dumps(body, indent=2, ensure_ascii=False).encode("utf-8")


class Handler(BaseHTTPRequestHandler):
    config: Config = None  # set on the server class before serving

    protocol_version = "HTTP/1.1"

    def log_message(self, fmt: str, *args) -> None:  # noqa: A003 - base class API
        sys.stderr.write(
            "  %s  %s\n" % (time.strftime("%H:%M:%S"), fmt % args)
        )

    def do_GET(self) -> None:  # noqa: N802 - base class API
        cfg = self.config
        path = self.path.split("?", 1)[0]

        if path in ("/version.json", "/FileCommander/version.json"):
            if cfg.stall_manifest:
                self.log_message("stalling manifest for %ss", cfg.stall_manifest)
                time.sleep(cfg.stall_manifest)
            body = cfg.manifest()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-cache, must-revalidate")
            self.end_headers()
            self.wfile.write(body)
            return

        if path == f"/packages/{cfg.package_name}":
            if cfg.fail_download:
                self.send_error(500, "injected download failure")
                return
            self.serve_package(cfg)
            return

        self.send_error(404, "not part of the update protocol")

    def serve_package(self, cfg: "Config") -> None:
        size = os.path.getsize(cfg.package)
        # A real static file server answers Range requests, which is what lets a
        # client continue an interrupted download instead of starting again.
        start = 0
        partial = False
        rng = self.headers.get("Range", "")
        if rng.startswith("bytes=") and not cfg.no_ranges:
            try:
                start = int(rng[len("bytes="):].split("-", 1)[0])
            except ValueError:
                start = 0
            if 0 < start < size:
                partial = True
            else:
                start = 0

        drop = cfg.drop_after > 0 and (cfg.drop_every or cfg.drops_served == 0)
        if drop:
            cfg.drops_served += 1

        remaining = size - start
        self.send_response(206 if partial else 200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(remaining))
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("ETag", cfg.etag)
        if partial:
            self.send_header("Content-Range", f"bytes {start}-{size - 1}/{size}")
        self.end_headers()
        self.log_message("package: from %s%s%s", start,
                         " (206)" if partial else " (200)",
                         f", dropping after {cfg.drop_after}" if drop else "")

        sent = 0
        with open(cfg.package, "rb") as handle:
            handle.seek(start)
            while True:
                budget = 1 << 16
                if drop:
                    budget = min(budget, cfg.drop_after - sent)
                    if budget <= 0:
                        break
                chunk = handle.read(budget)
                if not chunk:
                    break
                self.wfile.write(chunk)
                sent += len(chunk)
        if drop:
            self.wfile.flush()
            self.close_connection = True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--package", required=True,
                        help="the release package to advertise and serve")
    parser.add_argument("--version", default="9.9.9",
                        help="version to advertise (default: 9.9.9, i.e. always newer)")
    parser.add_argument("--date", default=time.strftime("%Y-%m-%d"))
    parser.add_argument("--notes", default="Mock release served by tools/mock-update-server.py")
    parser.add_argument("--segment", choices=sorted(set(SEGMENT_BY_SUFFIX.values())),
                        help="override the manifest segment (inferred from the suffix)")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--corrupt-hash", action="store_true",
                        help="advertise a hash that will not match: the client must refuse to install")
    parser.add_argument("--stall-manifest", type=int, default=0, metavar="SECONDS",
                        help="delay the manifest response, to exercise the client timeout")
    parser.add_argument("--fail-download", action="store_true",
                        help="answer the package request with HTTP 500")
    parser.add_argument("--drop-after", type=int, default=0, metavar="BYTES",
                        help="send this many bytes of the package, then hang up")
    parser.add_argument("--drop-every", action="store_true",
                        help="cut every package response short, not only the first")
    parser.add_argument("--no-ranges", action="store_true",
                        help="ignore Range requests: behave like a server without resume support")
    args = parser.parse_args()

    cfg = Config(args)
    Handler.config = cfg

    server = ThreadingHTTPServer((cfg.host, cfg.port), Handler)
    base = f"http://{cfg.host}:{cfg.port}"
    print(f"serving {cfg.package_name} ({os.path.getsize(cfg.package)} bytes)")
    print(f"  sha256 on disk : {cfg.real_sha256}")
    if cfg.advertised_sha256 != cfg.real_sha256:
        print(f"  sha256 in json : {cfg.advertised_sha256}   <-- deliberately wrong")
    print(f"  manifest       : {base}/version.json  (version {cfg.version}, segment {cfg.segment})")
    print()
    print("point a client at it with:")
    print(f"  FILECOMMANDER_UPDATE_MANIFEST_URL={base}/version.json")
    print()

    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        while True:
            time.sleep(0.5)
    except KeyboardInterrupt:
        print("\nstopping")
        server.shutdown()
    return 0


if __name__ == "__main__":
    sys.exit(main())
