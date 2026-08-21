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

Requires CMake, a C compiler, `pkg-config`, and libheif development headers.

```bash
cmake -S . -B build
cmake --build build -j
./build/heicprobe image.heic
```

## Status

Early experiment. Do not use it as the only conversion path for irreplaceable images yet.
