#!/usr/bin/env python3
"""Compare two run_ffmpeg_scale_npp.sh matrices byte-for-byte.

Usage: compare_ffmpeg_scale_npp.py GOLDEN_DIR CANDIDATE_DIR [--verbose]

The golden directory is produced on an NVIDIA host with real NPP; the candidate
directory is produced on the AMD host with the NPP shim. Both must use the same
source fixture (source.sha256), case list and per-case metadata. Every raw
output must be byte-identical.

Exit code: 0 = identical, 1 = mismatch, 2 = malformed input.
"""

import hashlib
import sys
from pathlib import Path

COLUMNS = [
    "case", "src_w", "src_h", "dst_w", "dst_h", "pix_fmt", "interp_algo",
    "frames", "bytes", "sha256",
]


def read_manifest(directory):
    path = directory / "manifest.tsv"
    if not path.is_file():
        raise ValueError(f"missing {path}")
    lines = path.read_text().splitlines()
    if not lines:
        raise ValueError(f"empty {path}")
    header = lines[0].split("\t")
    if header != COLUMNS:
        raise ValueError(f"{path}: unexpected header {header}")

    entries = {}
    for line in lines[1:]:
        if not line:
            continue
        fields = line.split("\t")
        if len(fields) != len(COLUMNS):
            raise ValueError(f"{path}: malformed line {line!r}")
        entry = dict(zip(COLUMNS, fields))
        if entry["case"] in entries:
            raise ValueError(f"{path}: duplicate case {entry['case']}")
        entries[entry["case"]] = entry
    if not entries:
        raise ValueError(f"{path}: no cases")
    return entries


def read_source_hash(directory):
    path = directory / "source.sha256"
    if not path.is_file():
        raise ValueError(f"missing {path}")
    return path.read_text().strip()


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main(argv):
    verbose = "--verbose" in argv
    args = [a for a in argv[1:] if not a.startswith("--")]
    if len(args) != 2:
        print(__doc__)
        return 2

    golden, candidate = Path(args[0]), Path(args[1])
    errors = []

    try:
        golden_entries = read_manifest(golden)
        cand_entries = read_manifest(candidate)
        golden_source = read_source_hash(golden)
        cand_source = read_source_hash(candidate)
    except ValueError as exc:
        print(f"FAIL: {exc}")
        return 2

    if golden_source != cand_source:
        print(f"FAIL: source fixture differs: {golden_source} vs {cand_source}")
        return 1
    if verbose:
        print(f"source fixture: {golden_source}")

    missing = sorted(set(golden_entries) - set(cand_entries))
    extra = sorted(set(cand_entries) - set(golden_entries))
    if missing:
        errors.append(f"candidate is missing {len(missing)} case(s): {missing[:5]}")
    if extra:
        errors.append(f"candidate has {len(extra)} unexpected case(s): {extra[:5]}")

    golden_files = {p.name for p in golden.glob("*.yuv")}
    cand_files = {p.name for p in candidate.glob("*.yuv")}
    if golden_files != cand_files:
        errors.append(
            "raw file set differs: missing "
            f"{sorted(golden_files - cand_files)[:5]}, extra "
            f"{sorted(cand_files - golden_files)[:5]}"
        )

    total_bytes = 0
    compared = 0
    for case in sorted(set(golden_entries) & set(cand_entries)):
        g, c = golden_entries[case], cand_entries[case]
        metadata_errors = 0
        for field in COLUMNS[1:-1]:
            if g[field] != c[field]:
                errors.append(f"{case}: {field} {g[field]!r} != {c[field]!r}")
                metadata_errors += 1
        if metadata_errors:
            # Metadata mismatch makes byte comparison of this case meaningless.
            continue

        gfile, cfile = golden / f"{case}.yuv", candidate / f"{case}.yuv"
        if not gfile.is_file() or not cfile.is_file():
            errors.append(f"{case}: missing raw output")
            continue

        expected_size = int(g["bytes"])
        if gfile.stat().st_size != expected_size:
            errors.append(f"{case}: golden size {gfile.stat().st_size} != manifest {expected_size}")
            continue
        if cfile.stat().st_size != expected_size:
            errors.append(f"{case}: candidate size {cfile.stat().st_size} != manifest {expected_size}")
            continue

        ghash, chash = digest(gfile), digest(cfile)
        if ghash != chash:
            offset = first_difference(gfile, cfile)
            detail = f"first difference at byte {offset}" if offset is not None else "length differs"
            errors.append(f"{case}: bytes differ ({detail})")
            continue
        if ghash != g["sha256"]:
            errors.append(f"{case}: golden hash does not match its manifest")
            continue

        total_bytes += expected_size
        compared += 1
        if verbose:
            print(f"PASS: {case} {expected_size} bytes")

    if errors:
        for error in errors[:20]:
            print(f"FAIL: {error}")
        if len(errors) > 20:
            print(f"FAIL: ... and {len(errors) - 20} more")
        return 1

    print(
        f"PASS: {compared} cases and {total_bytes} bytes are bit-identical "
        f"({golden} vs {candidate})"
    )
    return 0


def first_difference(a, b):
    with a.open("rb") as fa, b.open("rb") as fb:
        offset = 0
        while True:
            ca = fa.read(1 << 20)
            cb = fb.read(1 << 20)
            if ca != cb:
                for i, (x, y) in enumerate(zip(ca, cb)):
                    if x != y:
                        return offset + i
                return offset + min(len(ca), len(cb))
            if not ca:
                return None
            offset += len(ca)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
