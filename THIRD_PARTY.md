# Third-party dependencies

This repository does not vendor the following projects. They are external build/runtime
dependencies and remain under their own licenses.

| Dependency | Role | License / notes |
|---|---|---|
| FFmpeg 7.1 | Parser object files (`h2645_parse`, `h264_ps`, `h2645_vui`, `h2645data`, `h264data`, `h264_parse`, `golomb`, `mathtables`, `refstruct`) and `libavutil` | LGPL-2.1-or-later. Source is not committed. Binary distribution that links these objects must satisfy LGPL obligations (source availability, relinking, notices). |
| nv-codec-headers | CUDA/CUVID API headers (`ffnvcodec/*.h`) | MIT |
| ROCm / HIP | Runtime and compiler | MIT (see ROCm distribution) |
| Mesa / RADV | Vulkan Video implementation and `vk_video/*` headers | MIT (see Mesa distribution) |
| Vulkan-Headers / loader | Vulkan API headers and loader | Apache-2.0 / MIT-style; see upstream |
| libva / libdrm | VAAPI and DRM interfaces | MIT / MIT-style; see upstream |
| NVIDIA CUDA / NPP headers | Build-time compatibility headers used by users | NVIDIA proprietary license; not distributed here |

## FFmpeg usage boundary

The repository contains original shim/test code. The FFmpeg parser is reused as a build-time
dependency to avoid reimplementing H.264 SPS/PPS/slice parsing. The repository does not
include the FFmpeg source tree or prebuilt FFmpeg objects; users build or obtain them
separately.

If you distribute binaries that include FFmpeg-derived objects, review the LGPL-2.1
requirements with your legal team before release.

## Test media

`testdata/h264/*.h264` are small synthetic H.264 streams generated for this project.
Reference YUV files are generated locally and are not committed.
