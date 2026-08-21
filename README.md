# Csharp

Experimental GPU-accelerated HEIC/HEIF → JPEG conversion for NVIDIA GPUs.

## Goal

Keep HEIF container handling on the CPU, hand supported HEVC image payloads to NVIDIA NVDEC, keep decoded frames on the GPU where practical, and eventually encode JPEG with nvJPEG.

```text
HEIF container
    ↓
libheif / container handling
    ↓
HEVC coded image
    ↓
NVDEC
    ↓
GPU surface
    ↓
nvJPEG
    ↓
JPEG
```

## Phase 1

`heicprobe` establishes the boring, safe foundation before GPU decode work:

- open HEIC/HEIF through libheif
- locate the primary image
- report dimensions and basic image properties
- report EXIF/XMP presence
- classify whether the file is a candidate for the future GPU fast path
- fail cleanly on malformed/unsupported input

Complex HEIF features should eventually fall back to libheif's normal CPU decode rather than being guessed at.

## Build

Requires CMake, a C compiler, `pkg-config`, libheif development headers, and
FFmpeg development libraries:

```bash
sudo apt install libheif-dev libavcodec-dev libavutil-dev libswscale-dev
```

```bash
cmake -S . -B build
cmake --build build -j
./build/heicprobe image.heic
```

The focused NVDEC proof mode decodes the same disposable image first through
the normal libheif CPU decoder and then through the registered `csharp-nvdec`
decoder plugin:

```bash
./build/heicprobe --compare-nvdec image.heic
```

The plugin accepts libheif's length-prefixed HEVC stream, converts it to
Annex-B NAL units, decodes with FFmpeg's `hevc_cuvid`, downloads the CUDA
NV12 frame, and returns an 8-bit YCbCr 4:2:0 `heif_image`. The current scope
is ordinary opaque 8-bit HEVC images; unsupported formats must use the normal
libheif path.

## Status

Early experiment. Do not use it as the only conversion path for irreplaceable images yet.
