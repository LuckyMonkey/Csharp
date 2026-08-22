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

Csharp has two performance goals, and they are deliberately separate:

1. maximize images/second when GPU acceleration actually wins;
2. provide a low-CPU decode path when preserving CPU headroom is more valuable than minimum wall time.

A GPU path may therefore still be useful when it is slower in elapsed time if it materially reduces CPU-seconds consumed while PhotoSort or other workloads need the CPU.

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

The persistent decode-only benchmark loads its inputs into memory once and
round-robins them in one process. It reports cold startup separately from
warm repeated decodes:

```bash
./build/heicprobe --benchmark 100 sample1.heic sample2.heic
```

Benchmark output includes wall time, process CPU time, images/sec,
CPU-ms/image, and average CPU cores consumed. This lets Csharp distinguish a
throughput win from a CPU-headroom win.

The current NVDEC implementation shares one CUDA device and pools
`AVCodecContext`/NVDEC instances by HEVC parameter-set signature. This avoids
reinitializing compatible decoders for every image while keeping incompatible
streams isolated.

The plugin accepts libheif's length-prefixed HEVC stream, converts it to
Annex-B NAL units, decodes with FFmpeg's `hevc_cuvid`, downloads the CUDA
NV12 frame, and returns an 8-bit YCbCr 4:2:0 `heif_image`. The current scope
is ordinary opaque 8-bit HEVC images; unsupported formats must use the normal
libheif path.

## Next decoder experiments

The current `hevc_cuvid` route is the reference GPU backend. Two experiments
are intentionally kept separate from it so the working path remains available:

- FFmpeg native HEVC decoder with CUDA/NVDEC hardware frames, avoiding the
  legacy CUVID decoder wrapper where possible.
- Direct NVIDIA Video Codec SDK/NVDECODE integration using a long-lived decoder
  and decoder reconfiguration for changing still-image dimensions.

Both must preserve the existing exact CPU/NVDEC YCbCr validation before they
can replace the reference backend.

## Status

Early experiment. Do not use it as the only conversion path for irreplaceable images yet.
