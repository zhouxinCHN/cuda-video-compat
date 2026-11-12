#!/usr/bin/env python3
"""Collect CUDA/CUVID/NPP call sites from an FFmpeg source tree."""

from pathlib import Path
import argparse
import hashlib
import re


FILES = [
    "libavcodec/cuviddec.c",
    "libavcodec/nvdec.c",
    "libavcodec/nvdec_h264.c",
    "libavutil/hwcontext_cuda.c",
    "libavfilter/vf_scale_npp.c",
    # These files do not exist in every FFmpeg release.
    "compat/cuda/dynlink_loader.h",
    "libavcodec/dynlink_loader.h",
    "libavutil/hwcontext_cuvid.c",
]

REQUIRED_FILES = set(FILES[:5])

PATTERNS = {
    "NPP calls": re.compile(
        r"\b(npp[ics][A-Z][A-Za-z0-9_]*|nppGetVersion)\s*\("
    ),
    "CUVID calls": re.compile(r"\b(cuvid[A-Z][A-Za-z0-9_]*)\s*\("),
    "CUDA driver calls": re.compile(r"\b(cu[A-Z][A-Za-z0-9_]+)\s*\("),
}

LOADED_SYMBOL = re.compile(
    r"\bLOAD_SYMBOL(?:_OPT)?\s*\(\s*([A-Za-z0-9_]+)"
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ffmpeg_source", type=Path)
    args = parser.parse_args()
    root = args.ffmpeg_source.resolve()
    missing = set()

    print(f"FFmpeg source: {root}")
    for rel in FILES:
        path = root / rel
        print(f"\n===== {rel} =====")
        if not path.is_file():
            print("MISSING" if rel in REQUIRED_FILES else "SKIP: not in this release")
            if rel in REQUIRED_FILES:
                missing.add(rel)
            continue

        data = path.read_bytes()
        text = data.decode("utf-8", errors="replace")
        print("SHA256:", hashlib.sha256(data).hexdigest())

        for title, pattern in PATTERNS.items():
            names = sorted(set(pattern.findall(text)))
            print(f"{title} ({len(names)}):")
            for name in names:
                print(" ", name)

        loaded = sorted(set(LOADED_SYMBOL.findall(text)))
        if loaded:
            print(f"Dynamically loaded symbols ({len(loaded)}):")
            for name in loaded:
                print(" ", name)

    if missing:
        print("\nMissing required files:", ", ".join(sorted(missing)))
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
