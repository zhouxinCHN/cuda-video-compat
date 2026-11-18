# npp_shim — NPP compatibility libraries

This directory implements the first NPP compatibility increment for the three
functions used by FFmpeg `scale_npp`:

- `nppiResizeSqrPixel_8u_C1R` with `NPPI_INTER_NN`, `NPPI_INTER_LINEAR`, and
  `NPPI_INTER_CUBIC` (`cubic` is FFmpeg's default);
- `nppiYCbCr420_8u_P2P3R`;
- `nppiYCbCr420_8u_P3P2R`.

The eventual NPP resize scope covers every interpolation mode. Stage A is
deliberately limited to the three FFmpeg core algorithms; unsupported modes
return an explicit error.

## Status

Current implementation, validation coverage and open items are maintained only in
the [current capability matrix](../docs/当前能力矩阵.md). This README covers
build and usage; archived results live under [evidence](../docs/evidence/README.md).

The final build-header baseline is CUDA/NPP 12.0.1.104. NPP 11.5.1.107 remains
the runtime/ABI and numerical reference. No NVIDIA NPP headers or binaries are
stored in this repository. FFmpeg classifies `libnpp` as nonfree, so an NPP-enabled
FFmpeg is customer-built and is not distributed with the driver.

## Build

Route A requires the user's licensed CUDA/NPP headers:

```bash
make -C npp_shim \
  NPP_INCLUDE_DIR=/path/to/authorized/cuda/include \
  ROCM_DIR=/opt/rocm
```

Produces:

```text
libnppc.so.11
libnppig.so.11
libnppicc.so.11
libnppidei.so.11     # placeholder
libnppif.so.11       # placeholder
```

plus unversioned link-name symlinks. `NPP_INCLUDE_DIR` must contain `nppi.h`;
the Makefile refuses to build without it.

## Validation

The shared probe `tests/npp/npp_stage_a_probe.cpp` runs unchanged on NVIDIA NPP
and on these shims. See [tests/npp/README.md](../tests/npp/README.md) for the
golden/candidate workflow.

Acceptance status and remaining numerical work are tracked in the
[current capability matrix](../docs/当前能力矩阵.md).

## Implementation constraints

- The implemented API scope is limited to the functions listed above; do not
  infer full NPP compatibility from these entry points.
- Stage A uses host synchronization for correctness. `nppSetStream` records the
  requested stream and flushes the device on a switch, but kernels still run on
  the default stream; a non-NULL NPP stream is therefore ordering-safe, not
  concurrent.
- Only 8-bit C1 resize and NV12↔YUV420P 4:2:0 conversion are implemented.
- Lanczos uses a sampled kernel table (0.01 distance grid + linear interpolation
  and six normalized taps). `CUBIC2P_*` is gated off by default to mirror the
  NPP 11.5 runtime; `NPP_SHIM_ENABLE_CUBIC2P=1` enables the experimental kernels.
- Unsupported modes return explicit errors. Numerical coverage and remaining
  modes are tracked in the [current capability matrix](../docs/当前能力矩阵.md).
