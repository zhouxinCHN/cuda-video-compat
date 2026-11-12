# Contributing

Thanks for your interest. Before opening a PR, please read the project status and
verification boundaries in [README.md](README.md) and [docs/README.md](docs/README.md).

## Development setup

- Linux, ROCm 7.x, libva/libdrm, Vulkan loader.
- FFmpeg 7.1 build artifacts (see [THIRD_PARTY.md](THIRD_PARTY.md)).
- `nv-codec-headers`, and Mesa headers for the Vulkan backend.
- An AMD GPU is required for runtime tests; the CPU-side parser tests can run without one.

## Pull request checklist

- [ ] The change is scoped and described (what problem, what is verified, what is not).
- [ ] New source files follow the existing C++17 style and `extern "C"` symbol conventions.
- [ ] No proprietary NVIDIA headers, binaries, or FFmpeg source trees are added.
- [ ] Build commands in `README.md` / component README still work.
- [ ] Tests are updated; new tests state their pass criteria and limitations.
- [ ] `bash -n` passes for changed shell scripts.
- [ ] `python3 scripts/check_links.py` passes.
- [ ] No large generated files, `.so`, `.o`, `.yuv`, or reference binaries are committed.
- [ ] Commit messages explain the verification evidence, not just “fix”.

## Code style

- C++17; match the surrounding naming and comment density.
- Keep CUDA-facing symbol names exactly matching the NVIDIA headers and `_v2` suffixes.
- Unsupported CUDA/CUVID behavior must return an explicit error, never silently succeed.
- Prefer host-sync baselines until cross-stack fences have a reproducible implementation.

## Developer Certificate of Origin

By contributing, you certify that you have the right to submit the contribution under the
repository's eventual license. Sign off your commits:

```text
Signed-off-by: Your Name <you@example.com>
```

## Security

Please report vulnerabilities according to [SECURITY.md](SECURITY.md), not in a public issue.
