# License — TBD

The license for this repository has **not been selected yet**.

Until a license is added, no license is granted for use, reproduction, modification, or
distribution of the source code or documentation in this repository. Do not treat this
repository as open source yet.

Notes for the license decision:

- The compatibility layer links against, but does not vendor, FFmpeg 7.1 objects
  (`libavcodec/*.o`, `libavutil`) that are distributed under LGPL-2.1-or-later.
  Any binary distribution must satisfy the corresponding LGPL obligations.
- `nv-codec-headers` is MIT-licensed and is consumed as an external header dependency.
- NVIDIA CUDA/NPP headers are not distributed by this repository; users must obtain them
  under their own NVIDIA license.
- See [THIRD_PARTY.md](THIRD_PARTY.md) for the current third-party inventory.

When the license is selected, replace this file with a standard `LICENSE` file and update
`README.md` and `THIRD_PARTY.md` accordingly.
