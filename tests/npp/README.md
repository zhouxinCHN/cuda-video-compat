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

Current status: the shim/probe compile against installed CUDA 12 NPP headers and
FFmpeg `scale_npp` loads the shim. P2P3/P3P2 joint chains pass; resize golden is
still missing. See [local integration evidence](../../docs/evidence/NPP_StageA_本地集成_2026-11-13/README.md).

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
A rawvideo input plus `hwupload_cuda` on this host required `-threads 1`; use the
direct lavfi source for the stable P3P2 smoke test.

Resize smoke (execution only until golden exists):

```bash
for a in nn linear cubic; do
  ffmpeg -init_hw_device cuda=cu:0 -filter_hw_device cu \
    -f lavfi -i 'testsrc=size=320x240:rate=5:duration=1' \
    -vf "format=nv12,hwupload_cuda,scale_npp=w=160:h=120:interp_algo=$a,hwdownload,format=nv12" \
    -f rawvideo -pix_fmt nv12 "out-$a.nv12"
done
```

## Stage A acceptance

Do not mark Stage A complete until:

1. NVIDIA 11.5.1.107 golden has been generated with a recorded environment;
2. comparator passes with zero differing bytes;
3. FFmpeg `scale_npp` is rebuilt against the shim and passes `nn`, `linear`,
   `cubic` and the default cubic path;
4. decode → NPP output is verified on VAAPI and Vulkan (P2P3/P3P2 already pass;
   resize remains pending);
5. the FFmpeg nonfree distribution path has a legal decision.
