# npp_shim — Stage A NPP compatibility libraries

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

- HIP kernels and the five required library SONAMEs are present.
- Built successfully against real CUDA/NPP 12.0.1.104 headers after splitting the
  NPP ABI host layer from HIP code (CUDA and HIP vector types conflict).
- A separate FFmpeg 7.1 nonfree build links `libnppig.so.11` / `libnppicc.so.11`
  from this directory and exposes `scale_npp`.
- P2P3 and P3P2 joint chains are byte-identical to software conversion on both
  VAAPI and Vulkan decode backends.
- **NPP 11.5.1.107 golden comparison passes**: 32/32 manifest entries and
  2,490,624 bytes are byte-identical. The implementation reproduces NPP's
  absolute-coordinate/ROI semantics, float32 rounding, and its non-Catmull
  fast-cubic four-tap weights.
- **FFmpeg treats `libnpp` as nonfree**: `--enable-libnpp` requires
  `--enable-nonfree` and the result is marked unredistributable. The current
  integration is for internal validation only.
- `libnppc` currently exposes only `nppGetLibVersion`; stream/version APIs are a
  later increment.
- No NVIDIA NPP headers or binaries are stored in this repository.

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

Stage A acceptance status:

1. ✅ NVIDIA 11.5.1.107 reference outputs for all probe cases;
2. ✅ byte-identical AMD shim outputs: 32/32 entries, 2,490,624 bytes;
3. ✅ FFmpeg `scale_npp` with `interp_algo=nn/linear/cubic`; default equals
   explicit cubic;
4. ✅ decode → P2P3 and upload → P3P2 are byte-identical on VAAPI/Vulkan;
5. ⏳ legal/build decision for the FFmpeg nonfree linkage, plus a final rebuild
   against authorized NPP 11.5 headers.

## Known limitations

- Resize formula is an initial implementation pending NVIDIA golden
  correction. Do not describe it as NPP bit-exact yet.
- Stage A uses host synchronization for correctness.
- Only 8-bit C1 resize and NV12↔YUV420P 4:2:0 conversion.
- Stage B (remaining FFmpeg interpolation options) and Stage C (all NPP
  interpolation flags) are not implemented.
