#!/usr/bin/env python3
"""Compare NPP Stage A NVIDIA golden output with an AMD shim candidate."""

from __future__ import annotations

import argparse
import hashlib
import sys
from pathlib import Path


def read_manifest(root: Path) -> dict[tuple[str, str, str, str], tuple[int, ...]]:
    manifest = root / "manifest.tsv"
    if not manifest.is_file():
        raise ValueError(f"missing manifest: {manifest}")
    entries: dict[tuple[str, str, str, str], tuple[int, ...]] = {}
    for line_no, line in enumerate(manifest.read_text(encoding="utf-8").splitlines(), 1):
        fields = line.split("\t")
        if len(fields) != 11:
            raise ValueError(
                f"{manifest}:{line_no}: expected 11 fields, got {len(fields)}"
            )
        function, case, variant, filename, *numbers = fields
        try:
            metadata = tuple(int(value) for value in numbers)
        except ValueError as exc:
            raise ValueError(f"{manifest}:{line_no}: non-numeric metadata") from exc
        key = (function, case, variant, filename)
        if key in entries:
            raise ValueError(f"{manifest}:{line_no}: duplicate key {key}")
        entries[key] = metadata
    if not entries:
        raise ValueError(f"empty manifest: {manifest}")
    return entries


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def first_difference(reference: bytes, candidate: bytes) -> str:
    limit = min(len(reference), len(candidate))
    for offset in range(limit):
        if reference[offset] != candidate[offset]:
            return (
                f"offset={offset} golden=0x{reference[offset]:02x} "
                f"candidate=0x{candidate[offset]:02x}"
            )
    return f"common prefix={limit} bytes; length differs"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("golden", type=Path, help="NVIDIA NPP reference directory")
    parser.add_argument("candidate", type=Path, help="AMD NPP shim output directory")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    try:
        golden = read_manifest(args.golden)
        candidate = read_manifest(args.candidate)
    except ValueError as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 2

    missing = sorted(set(golden) - set(candidate))
    extra = sorted(set(candidate) - set(golden))
    if missing:
        print("FAIL: candidate is missing cases/files:", file=sys.stderr)
        for key in missing:
            print("  ", key, file=sys.stderr)
        return 1
    if extra:
        print("FAIL: candidate has unexpected cases/files:", file=sys.stderr)
        for key in extra:
            print("  ", key, file=sys.stderr)
        return 1

    failures = 0
    total_bytes = 0
    for key in sorted(golden):
        function, case, variant, filename = key
        if golden[key] != candidate[key]:
            print(
                f"FAIL metadata {function}/{case}/{variant}/{filename}: "
                f"golden={golden[key]} candidate={candidate[key]}",
                file=sys.stderr,
            )
            failures += 1
            continue
        reference_path = args.golden / filename
        candidate_path = args.candidate / filename
        if not reference_path.is_file() or not candidate_path.is_file():
            print(f"FAIL missing payload {filename}", file=sys.stderr)
            failures += 1
            continue
        reference = reference_path.read_bytes()
        actual = candidate_path.read_bytes()
        total_bytes += len(reference)
        if reference != actual:
            failures += 1
            print(
                f"FAIL {function}/{case}/{variant}/{filename}: "
                f"{first_difference(reference, actual)}",
                file=sys.stderr,
            )
        elif args.verbose:
            print(
                f"PASS {function}/{case}/{variant}/{filename} "
                f"{len(reference)} bytes sha256={digest(reference)}"
            )

    if failures:
        print(
            f"FAIL: {failures}/{len(golden)} files or metadata entries differ",
            file=sys.stderr,
        )
        return 1
    print(
        f"PASS: {len(golden)} manifest entries and {total_bytes} bytes "
        "are bit-identical"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
