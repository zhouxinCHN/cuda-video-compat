#!/usr/bin/env python3
"""Check relative Markdown links in this repository.

Usage: python3 scripts/check_links.py

External links, anchors, and absolute URLs are ignored. A link that points to a
missing local path fails the check.
"""

from __future__ import annotations

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LINK_RE = re.compile(r"\]\(([^)]+)\)")
SKIP_DIRS = {".git", ".claude", "third_party", "__pycache__"}


def main() -> int:
    missing: list[tuple[str, str]] = []
    for dirpath, dirnames, filenames in os.walk(ROOT):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for name in filenames:
            if not name.endswith(".md"):
                continue
            path = os.path.join(dirpath, name)
            with open(path, encoding="utf-8", errors="ignore") as fh:
                text = fh.read()
            for match in LINK_RE.finditer(text):
                target = match.group(1).strip()
                if target.startswith(("http://", "https://", "mailto:", "#")):
                    continue
                target = target.split("#", 1)[0]
                if not target:
                    continue
                if target.startswith("/"):
                    resolved = target
                else:
                    resolved = os.path.normpath(os.path.join(dirpath, target))
                if not os.path.exists(resolved):
                    missing.append((os.path.relpath(path, ROOT), match.group(1)))

    if missing:
        for source, target in missing:
            print(f"broken link: {source}: {target}")
        return 1
    print("all relative Markdown links resolve")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
