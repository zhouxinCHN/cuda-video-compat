# NPP Stage A probe and golden comparison

`npp_stage_a_probe.cpp` is compiled once against NVIDIA NPP and once against the
AMD NPP shim. It exercises:

- `nppiResizeSqrPixel_8u_C1R` with NN, linear and cubic (Stage A), plus the
  Stage B modes `CUBIC2P_BSPLINE`, `CUBIC2P_CATMULLROM`, `CUBIC2P_B05C03`,
  `LANCZOS` and `SUPER` (SUPER only for downscaling cases);
- `nppiYCbCr420_8u_P2P3R`;
- `nppiYCbCr420_8u_P3P2R`;
- full-frame and non-zero ROIs, padded pitches, odd destination dimensions and
  destination ROI offsets.

`--stage a|b|all` selects the resize modes (default `all`). `--stage b` emits
only Stage B resize cases; `--skip LIST` omits named algorithms (for example
`--skip super` on a host that does not implement it yet). Use `--stage a` to
reproduce the archived 32-entry Stage A golden and `--stage b` to generate the
Stage B golden.

The probe never embeds NVIDIA headers or binaries. The same source is used for
golden and candidate output; `manifest.tsv` records geometry and output files.
NPP ABI host code and GPU-runtime code are split into separate translation units
because CUDA/NPP and HIP headers define conflicting vector types.

Current implementation, validation coverage and open items are maintained only
in the [current capability matrix](../../docs/当前能力矩阵.md). This document
covers probe and reproduction workflows; historical outputs are archived under
[evidence](../../docs/evidence/README.md).

The final build-header baseline is CUDA/NPP 12.0.1.104. No rebuild with 11.5
headers is required; NVIDIA NPP 11.5.1.107 remains the runtime/ABI numerical
reference. The end-to-end FFmpeg matrix and the `libnppc` core API probe are
described in sections 4 and 5.

## 1. NVIDIA golden host

Requires authorized CUDA/NPP 11.5 headers and libraries:

```bash
make -C tests/npp clean all \
  GPU_RUNTIME=cuda \
  NPP_INCLUDE_DIR=/usr/local/cuda/include \
  CUDA_INCLUDE_DIR=/usr/local/cuda/include \
  CUDA_LIB_DIR=/usr/local/cuda/lib64 \
  NPP_LIB_DIR=/usr/local/cuda/lib64

mkdir -p /tmp/npp-golden
(cd tests/npp && ./npp_stage_a_probe --output /tmp/npp-golden --stage all)
```

The Stage B golden uses the same command; the manifest simply contains the
additional resize entries. Keep it separate from the Stage A golden directory so
the existing 32-entry comparison remains a fixed baseline.

If the NVIDIA host has a different NPP library root, set `NPP_LIB_DIR`
accordingly. The binary records no host names; archive the NPP version and
hashes with the golden directory.

## 2. AMD shim host

For local build self-testing without licensed NVIDIA headers, do **not** add
stub headers to the repository. Point `NPP_INCLUDE_DIR` at the user-provided
authorized header tree:

```bash
make -C npp_shim NPP_INCLUDE_DIR=/path/to/authorized/cuda/include ROCM_DIR=/opt/rocm

make -C tests/npp clean all \
  GPU_RUNTIME=shim \
  NPP_INCLUDE_DIR=/path/to/authorized/cuda/include \
  NPP_LIB_DIR=../../npp_shim \
  ROCM_DIR=/opt/rocm

mkdir -p /tmp/npp-candidate
(cd tests/npp && ./npp_stage_a_probe --output /tmp/npp-candidate)
```

The runtime needs the shim and ROCm libraries on `LD_LIBRARY_PATH` if rpath is
not enough:

```bash
export LD_LIBRARY_PATH="$PWD/npp_shim:/opt/rocm/lib:${LD_LIBRARY_PATH:-}"
```

## 3. Compare

```bash
python3 tests/npp/compare_stage_a.py /tmp/npp-golden /tmp/npp-candidate --verbose
```

The comparator requires:

- identical manifest keys and geometry metadata;
- identical file set;
- byte-for-byte identical raw planes and padding.

Exit code 0 means all 32 manifest entries (24 resize + 8 color entries) are
identical. A mismatch prints the first differing byte and candidate value.

## Local HIP smoke test (no NVIDIA golden)

A local stub header can be kept outside the repository solely to compile and
run the shim during development. This does not prove NPP ABI or numeric
compatibility and must never be committed. The implemented color conversions
can be checked independently against their deterministic source planes; resize
samples must not be treated as NPP-valid until the NVIDIA comparison passes.

## 4. FFmpeg integration smoke

FFmpeg 7.1 classifies `libnpp` as **nonfree**. Enabling `scale_npp` therefore
requires `--enable-nonfree`; the resulting build is for local validation and is
not redistributable. Disable the other NPP filters unless their symbols are
implemented:

```bash
./configure ... \
  --enable-nonfree --enable-libnpp \
  --disable-filter=scale2ref_npp \
  --disable-filter=transpose_npp \
  --disable-filter=sharpen_npp \
  --extra-cflags=-I/usr/include \
  --extra-ldflags="-L$REPO/npp_shim -Wl,-rpath,$REPO/npp_shim"
make -j"$(nproc)"
```

Verify the shim is actually loaded:

```bash
ldd libavfilter/libavfilter.so.* | grep npp
./ffmpeg -filters | grep scale_npp
```

Color-conversion joint chains (no resize golden needed):

```bash
# Decode -> NPP P2P3 -> download, run once with CUVID_SHIM_BACKEND=vaapi and vk
CUVID_SHIM_BACKEND=vaapi ffmpeg -hwaccel nvdec -hwaccel_output_format cuda \
  -i testdata/h264/test_p_b_5.h264 \
  -vf 'scale_npp=w=320:h=240:format=yuv420p,hwdownload,format=yuv420p' \
  -f rawvideo -pix_fmt yuv420p out.yuv

# Software planar -> hwupload -> NPP P3P2 -> download
ffmpeg -init_hw_device cuda=cu:0 -filter_hw_device cu \
  -f lavfi -i 'testsrc=size=320x240:rate=5:duration=1' \
  -vf 'format=yuv420p,hwupload_cuda,scale_npp=w=320:h=240:format=nv12,hwdownload,format=nv12' \
  -f rawvideo -pix_fmt nv12 out.nv12
```

Both are expected to be byte-identical to the corresponding software conversion.
Rawvideo input plus `hwupload_cuda` used to corrupt later frames because the
shim forwarded a host-involved `cuMemcpy2DAsync` to `hipMemcpyParam2DAsync`,
which could still read the source buffer after FFmpeg recycled it. The shim now
performs host-involved 2D copies synchronously, matching CUDA's pageable-memory
semantics. `verify_hwupload_roundtrip.sh` is the regression test for both
`yuv420p` and `nv12`.

Resize smoke (execution only until golden exists):

```bash
for a in nn linear cubic; do
  ffmpeg -init_hw_device cuda=cu:0 -filter_hw_device cu \
    -f lavfi -i 'testsrc=size=320x240:rate=5:duration=1' \
    -vf "format=nv12,hwupload_cuda,scale_npp=w=160:h=120:interp_algo=$a,hwdownload,format=nv12" \
    -f rawvideo -pix_fmt nv12 "out-$a.nv12"
done
```

## 5. libnppc core API probe

`nppc_core_probe.cpp` is built and run on both hosts. It is not byte-compared:
device limits are machine-specific. It checks the semantics NPP clients rely on:

- `nppGetLibVersion` (optionally `--expect-lib-version 11.5.1`; the real
  11.5.1.107 package reports `{11, 5, 1}`, not `11.5.107`);
- `nppGetGpuNumSMs`, `nppGetMaxThreadsPerBlock`, `nppGetMaxThreadsPerSM`,
  `nppGetGpuDeviceProperties` agree with each other and are non-zero;
- `nppGetGpuName` is non-empty;
- `nppGetStream` starts as the NULL stream, `nppSetStream(NULL)` succeeds;
- `nppGetStreamContext` reports the same device, limits and stream;
- `nppGetStreamNumSMs` / `nppGetStreamMaxThreadsPerSM` agree with the device
  queries.

```bash
make -C tests/npp GPU_RUNTIME=shim \
  NPP_INCLUDE_DIR=/path/to/authorized/cuda/include \
  NPP_LIB_DIR=../../npp_shim ROCM_DIR=/opt/rocm run-core
```

On the NVIDIA host use `GPU_RUNTIME=cuda`, `NPP_LIB_DIR=/usr/local/cuda/lib64`.

The shim's `nppSetStream` only records the handle and flushes the device; the
Stage A kernels synchronize around every operation, so user-stream ordering is
preserved even though kernels run on the default stream. `nppGetStreamContext`
fills `NppStreamContext` from `hipGetDeviceProperties`; compute capability is
the value HIP reports, matching `cuda_shim`'s `cuDeviceComputeCapability`.

## 6. FFmpeg scale_npp end-to-end matrix

`run_ffmpeg_scale_npp.sh` drives FFmpeg's own `scale_npp` filter over 5 target
geometries x 4 interpolation modes (`nn`, `linear`, `cubic`, default) x 2
surface formats (`nv12` through P2P3/P3P2, `yuv420p` per-plane). Each case runs
twice and must hash identically. The input is `testsrc` through `format=yuv420p`
(no decoder involved); a software-only render of the same frames is hashed as
`source.sha256`.

NVIDIA golden host (real NPP, FFmpeg built with `--enable-libnpp`):

```bash
FFMPEG_BUILD_DIR=/path/to/ffmpeg-npp-build \
NPP_RUNTIME=cuda OUT_DIR=/tmp/scale-npp-nvidia \
  tests/npp/run_ffmpeg_scale_npp.sh
```

AMD candidate host (shim):

```bash
FFMPEG_BUILD_DIR=/path/to/ffmpeg-npp-build \
NPP_RUNTIME=shim OUT_DIR=/tmp/scale-npp-amd \
  tests/npp/run_ffmpeg_scale_npp.sh
```

Compare:

```bash
python3 tests/npp/compare_ffmpeg_scale_npp.py \
  /tmp/scale-npp-nvidia /tmp/scale-npp-amd --verbose
```

The comparator requires the same source fixture hash, case list and per-case
metadata, then byte-compares all raw outputs. It exits non-zero on the first
source mismatch or any differing byte, reporting the byte offset. When the
golden host publishes only `manifest.tsv` and hashes (raw outputs are not
archived in git), add `--hashes-only` to compare the recorded per-case SHA256
values instead of re-reading raw files.

`INPUT_MODE=lavfi` (default) feeds the frames inline; `INPUT_MODE=rawvideo`
reads `source-reference.yuv` through the rawvideo demuxer. Both start from the
same YUV420P frames, but FFmpeg's RGB->NV12 and YUV420P->NV12 conversions differ
by a few chroma values, so nv12 cases are only comparable between runs that use
the same mode. The archived NVIDIA golden uses lavfi; generate a rawvideo golden
on the reference host before comparing that mode.

Upload/download alone is covered by `verify_hwupload_roundtrip.sh`, which needs
no golden and requires the CUDA round trip to reproduce a software conversion
byte-for-byte for `yuv420p` and `nv12`:

```bash
FFMPEG_BUILD_DIR=/path/to/ffmpeg tests/npp/verify_hwupload_roundtrip.sh
```

## 7. Stage B golden (Lanczos / Super)

NPP 11.5.1.107 implements `NPPI_INTER_LANCZOS` and `NPPI_INTER_SUPER`, but its
runtime rejects all three `NPPI_INTER_CUBIC2P_*` values with
`NPP_INTERPOLATION_ERROR` (`-22`) for downscale, identity and upscale; the
library also contains no cubic2p kernels. The 11.5 Stage B golden therefore
covers Lanczos and Super only:

```bash
(cd tests/npp && ./npp_stage_a_probe --output /tmp/npp-golden-b --stage b \
  --skip cubic2p_bspline,cubic2p_catmullrom,cubic2p_b05c03)
```

That produces 14 manifest entries (7 resize cases x src/dst). Super runs only
on geometries that downscale both axes; the upscaling case is skipped by the
probe. On the AMD shim use `--stage b --skip super` until supersampling is
implemented.

FFmpeg end-to-end golden generation for the implemented modes:

```bash
GEOMETRIES="160x120 214x118 640x480 320x240 100x100" \
ALGOS="lanczos" NPP_RUNTIME=cuda OUT_DIR=/tmp/scale-npp-b \
  tests/npp/run_ffmpeg_scale_npp.sh

GEOMETRIES="160x120 214x118 100x100" \
ALGOS="super" NPP_RUNTIME=cuda OUT_DIR=/tmp/scale-npp-super \
  tests/npp/run_ffmpeg_scale_npp.sh
```

Archived results: 10/10 Lanczos and 6/6 Super deterministic, plus the
`cubic2p` `-22` failure log:
[NPP Stage B NVIDIA 11.5 golden](../../docs/evidence/NPP_StageB_NVIDIA_11.5_golden/README.md).

## Acceptance status

Current acceptance status and open items are maintained only in the
[current capability matrix](../../docs/当前能力矩阵.md). This section
intentionally retains no second status list.
