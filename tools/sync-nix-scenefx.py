#!/usr/bin/env python3
"""Refresh the `scenefx` entry in flake.lock so Nix builds the committed submodule revision.

The Nix build never uses the submodule: nix/package.nix copies the `scenefx` flake input into
`subprojects/scenefx` before configuring. Bumping the submodule therefore leaves Nix users pinned to
the previously locked revision, and their build breaks as soon as Umbriel calls a SceneFX API that
only exists in the newer commit. Run this after every submodule bump, in the same commit.

The lock entry is derived from the fork itself (commit metadata plus a NAR hash of the tree), so no
`nix` binary is needed. `--check` verifies the lock without writing, for use before a release.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import io
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

SUBMODULE = "subprojects/scenefx"
INPUT = "scenefx"


def run(args: list[str], cwd: Path | None = None) -> bytes:
    proc = subprocess.run(args, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if proc.returncode != 0:
        detail = proc.stderr.decode(errors="replace").strip()
        raise SystemExit(f"error: {' '.join(args)} failed: {detail}")
    return proc.stdout


def text(args: list[str], cwd: Path | None = None) -> str:
    return run(args, cwd).decode().strip()


# NAR serialization, as used by the Nix store and by every fetcher's `narHash`. Strings are prefixed
# with a 64-bit little-endian length and padded to an 8-byte boundary; directory entries are sorted
# by raw byte order, which is not quite git's tree order.
def _emit(out: io.BytesIO, value: str | bytes) -> None:
    raw = value.encode() if isinstance(value, str) else value
    out.write(struct.pack("<Q", len(raw)))
    out.write(raw)
    out.write(b"\0" * (-len(raw) % 8))


def _tree_entries(repo: Path, tree: str) -> list[tuple[str, str, str, str]]:
    entries = []
    for record in text(["git", "ls-tree", "-z", tree], repo).split("\0"):
        if not record:
            continue
        meta, name = record.split("\t", 1)
        mode, kind, oid = meta.split()
        entries.append((name, mode, kind, oid))
    return sorted(entries, key=lambda entry: entry[0].encode())


def _emit_node(out: io.BytesIO, repo: Path, mode: str, kind: str, oid: str) -> None:
    _emit(out, "(")
    if kind == "tree":
        _emit(out, "type")
        _emit(out, "directory")
        for name, child_mode, child_kind, child_oid in _tree_entries(repo, oid):
            # A gitlink has no content in a `submodules=false` fetch, which is how the flake input is
            # locked, so it is absent from the archive rather than an empty directory.
            if child_kind == "commit":
                continue
            _emit(out, "entry")
            _emit(out, "(")
            _emit(out, "name")
            _emit(out, name)
            _emit(out, "node")
            _emit_node(out, repo, child_mode, child_kind, child_oid)
            _emit(out, ")")
    elif mode == "120000":
        _emit(out, "type")
        _emit(out, "symlink")
        _emit(out, "target")
        _emit(out, run(["git", "cat-file", "blob", oid], repo))
    else:
        _emit(out, "type")
        _emit(out, "regular")
        if mode == "100755":
            _emit(out, "executable")
            _emit(out, "")
        _emit(out, "contents")
        _emit(out, run(["git", "cat-file", "blob", oid], repo))
    _emit(out, ")")


def nar_hash(repo: Path, rev: str) -> str:
    tree = text(["git", "rev-parse", f"{rev}^{{tree}}"], repo)
    out = io.BytesIO()
    _emit(out, "nix-archive-1")
    _emit_node(out, repo, "040000", "tree", tree)
    return "sha256-" + base64.b64encode(hashlib.sha256(out.getvalue()).digest()).decode()


def committed_rev(root: Path) -> str:
    # The gitlink in the index is what a fetch of this working tree resolves to, so the lock tracks
    # it rather than the submodule's checked-out HEAD.
    entry = text(["git", "ls-files", "-s", "--", SUBMODULE], root)
    if not entry:
        raise SystemExit(f"error: {SUBMODULE} is not a tracked submodule")
    return entry.split()[1]


def locked_entry(root: Path, url: str, ref: str, rev: str) -> dict[str, object]:
    with tempfile.TemporaryDirectory(prefix="umbriel-scenefx-") as tmp:
        mirror = Path(tmp) / "scenefx.git"
        run(["git", "clone", "--bare", "--quiet", "--single-branch", "--branch", ref, url, str(mirror)])
        if subprocess.run(
            ["git", "merge-base", "--is-ancestor", rev, "HEAD"],
            cwd=mirror,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        ).returncode != 0:
            raise SystemExit(
                f"error: {rev[:12]} is not on {url} {ref}; push the submodule commit to the fork first"
            )
        return {
            "lastModified": int(text(["git", "log", "-1", "--format=%ct", rev], mirror)),
            "narHash": nar_hash(mirror, rev),
            "ref": ref,
            "rev": rev,
            "revCount": int(text(["git", "rev-list", "--count", rev], mirror)),
            "type": "git",
            "url": url,
        }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="fail instead of writing when flake.lock does not match the submodule",
    )
    args = parser.parse_args()

    root = Path(text(["git", "rev-parse", "--show-toplevel"]))
    lock_path = root / "flake.lock"
    lock = json.loads(lock_path.read_text())
    node = lock["nodes"][INPUT]
    original = node["original"]
    if original.get("type") != "git":
        raise SystemExit(f"error: the {INPUT} input is not a git input; update this script")

    rev = committed_rev(root)
    checked_out = text(["git", "rev-parse", "HEAD"], root / SUBMODULE)
    if checked_out != rev:
        print(
            f"warning: {SUBMODULE} is checked out at {checked_out[:12]} but the index records "
            f"{rev[:12]}; locking the index revision",
            file=sys.stderr,
        )

    entry = locked_entry(root, original["url"], original["ref"], rev)
    if node["locked"] == entry:
        print(f"flake.lock: {INPUT} already at {rev[:12]}")
        return 0
    if args.check:
        print(
            f"error: flake.lock has {INPUT} at {node['locked']['rev'][:12]}, submodule is at "
            f"{rev[:12]}; run tools/sync-nix-scenefx.py",
            file=sys.stderr,
        )
        return 1

    node["locked"] = entry
    lock_path.write_text(json.dumps(lock, indent=2, sort_keys=True) + "\n")
    print(f"flake.lock: {INPUT} -> {rev[:12]} ({entry['narHash']})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
