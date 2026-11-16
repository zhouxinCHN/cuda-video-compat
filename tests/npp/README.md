# NPP Stage A probe and golden comparison

`npp_stage_a_probe.cpp` is compiled once against NVIDIA NPP and once against the
AMD NPP shim. It exercises:

- `nppiResizeSqrPixel_8u_C1R` with NN, linear and cubic;
- `nppiYCbCr420_8u_P2P3R`;
- `nppiYCbCr420_8u_P3P2R`;
- full-frame and non-zero ROIs, padded pitches, odd destination dimensions and
  destination ROI offsets.

The probe never embeds NVIDIA headers or binaries. The same source is used for
golden and candidate output; `manifest.tsv` records geometry and output files.
NPP ABI host code and GPU-runtime code are split into separate translation units
because CUDA/NPP and HIP headers define conflicting vector types.

Current status: the final build header baseline is CUDA/NPP 12.0.1.104 and
FFmpeg `scale_npp` loads the shim. No rebuild with 11.5 headers is required; the
NVIDIA NPP 11.5.1.107 golden remains the runtime/ABI numerical reference. The
AMD candidate matches all 32 entries byte-for-byte; P2P3/P3P2 joint chains pass.
See [AMD vs golden evidence](../../docs/evidence/NPP_StageA_AMD_11.5对比_2026-11-13/README.md).
The end-to-end FFmpeg matrix and the `libnppc` core API probe are described in
sections 4 and 5.

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
(cd tests/npp && ./npp_stage_a_probe --output /tmp/npp-golden)
```

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
A rawvideo input plus `hwupload_cuda` on this host is still nondeterministic
(cross-frame corruption independent of `-threads 1`, `-filter_threads 1` or
`-thread_type none`); use the direct lavfi source. This is tracked as a
cuda_shim upload/copy synchronization defect, separate from NPP.

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

- `nppGetLibVersion` (optionally `--expect-lib-version 11.5.107`);
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

The script uses lavfi input on purpose: rawvideo input plus `hwupload_cuda` is
nondeterministic on the AMD shim (see the note in section 4), and that defect
must be fixed before rawvideo can be used in this matrix.

## Stage A acceptance status

1. ✅ NVIDIA 11.5.1.107 golden generated with recorded environment;
2. ✅ comparator passes with zero differing bytes (32/32, 2,490,624 bytes);
3. ✅ FFmpeg `scale_npp` built against the shim; `nn`, `linear`, `cubic` and
   default cubic execute, default equals explicit cubic;
4. ✅ decode → P2P3 and upload → P3P2 are byte-identical on VAAPI and Vulkan;
5. ✅ FFmpeg nonfree build is customer-built; vendor delivery is chip plus
   driver and does not bundle it;
6. ✅ final build headers fixed at CUDA/NPP 12.0.1.104; no 11.5 header rebuild;
7. ✅ `libnppc` core API (version, device limits, name, stream, stream context)
   implemented and probed on the AMD shim;
8. ✅ FFmpeg `scale_npp` end-to-end matrix: AMD side is deterministic (40/40,
   26,307,120 bytes); NVIDIA golden generated with real NPP 11.5.1.107, and all
   40 case SHA256 values match. See
   [NVIDIA evidence](../../docs/evidence/NPP_FFmpeg_scale_npp_NVIDIA_2026-11-16/README.md).
