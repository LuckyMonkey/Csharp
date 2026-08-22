# Csharp

**GPU-accelerated HEIC/HEIF decoding and conversion experiments for NVIDIA GPUs. Written in C. Not C#.**

> **Project name:** “Csharp” / “see sharp.”  
> **Implementation language:** C.  
> **C# involvement:** none.

Csharp started with a simple question: **HEIC files commonly contain HEVC-compressed image data, so why spend general-purpose CPU cycles decoding that HEVC when an NVIDIA GPU already has dedicated HEVC decode hardware?**

The project now has a validated libheif decoder-plugin path that can hand real HEIC image payloads to NVIDIA NVDEC and return decoded YCbCr image planes that match the normal CPU decode exactly on the current test corpus.

## 🍗 Status at a glance

🍗 **Validated:** real HEIC → libheif → Csharp plugin → NVIDIA NVDEC → decoded YCbCr image.

🍗 **Validated:** 8/8 current real HEIC test images produce exact CPU/NVDEC YCbCr plane hashes.

🍗 **Validated:** FFmpeg native HEVC + CUDA/NVDEC backend reached **132.99 images/sec** on the current RTX 3050 benchmark corpus.

🍗 **Validated:** legacy `hevc_cuvid` backend reached **37.54 images/sec** at its best measured worker count.

🍗 **Baseline:** normal CPU libheif decoding on the test machine is roughly **11–14 images/sec**, depending on run/corpus mix.

🍗 **Experimental:** direct NVIDIA NVDECODE integration that removes FFmpeg from the GPU decode hot path is under active development.

🍗 **Planned:** keep decoded surfaces GPU-resident and feed them toward GPU JPEG encoding instead of bouncing full images through CPU memory.

## 🍗 What Csharp is doing

The key idea is to let **libheif keep doing HEIF container work** while Csharp replaces the expensive HEVC image decoder.

```text
HEIC / HEIF
    │
    ▼
libheif
(container, items, metadata, transforms)
    │
    ▼
HEVC coded image
    │
    ▼
Csharp decoder plugin
    │
    ├── validated: FFmpeg native HEVC + NVDEC
    ├── reference: hevc_cuvid + NVDEC
    └── experimental: direct NVIDIA NVDECODE
    │
    ▼
NVIDIA NVDEC
    │
    ▼
decoded YUV / CUDA surface
    │
    ▼
libheif image today
GPU JPEG pipeline later
```

## 🍗 Why this exists

🍗 HEIC is a HEIF container; the actual primary image is commonly compressed with HEVC.

🍗 NVIDIA GPUs include dedicated fixed-function HEVC decode hardware through NVDEC.

🍗 libheif exposes a decoder-plugin ABI, so Csharp does **not** need to reimplement the entire HEIF container format.

🍗 Still-image workloads are different from continuous video: startup, parser, decoder lifecycle, scheduling, transfer, allocation, and synchronization overhead can dominate if each photo is treated as an isolated decode session.

🍗 The project therefore treats a **persistent stream of independent HEIC decode jobs** as the unit of optimization, using pooled decoder state and parallel worker lanes.

## 🍗 Current benchmark headline

Current development machine:

🍗 **GPU:** NVIDIA RTX 3050 6 GB (Ampere)

🍗 **CPU:** Intel Core i7-3770 (Ivy Bridge, no AVX2)

🍗 **OS:** Ubuntu 24.04

🍗 **Driver:** NVIDIA 580.173.02

🍗 **CUDA:** 13.0 environment reported during development

🍗 **FFmpeg:** 6.1.1

🍗 **libheif:** 1.17.6 runtime during initial bring-up

Best measured decode-only results on the current small real-HEIC corpus:

| Backend | Best measured throughput | Worker count | Approx. CPU use | Validation |
| --- | ---: | ---: | ---: | --- |
| CPU libheif | ~11–14 img/s | — | ~1 CPU core in measured runs | reference |
| `hevc_cuvid` + NVDEC | **37.54 img/s** | 12 | ~2.95 cores | 8/8 exact hashes |
| Native FFmpeg HEVC + NVDEC | **132.99 img/s** | 32 | ~5.64 cores | 8/8 exact hashes |
| Direct NVDECODE | **not yet validated** | — | — | experimental |

See [BENCHMARKS.md](BENCHMARKS.md) for the full measured worker sweep, stage timings, caveats, and record-to-beat.

## 🍗 What changed the performance picture

🍗 One-at-a-time GPU decoding was initially slower than CPU decoding because decoder/session startup dominated.

🍗 Pooling reusable decoder state helped, but parallel decode lanes were the first major breakthrough.

🍗 Moving from the older `hevc_cuvid` wrapper to FFmpeg's native HEVC decoder with NVIDIA hardware acceleration produced the largest jump so far.

🍗 At 32 native workers, the current benchmark reached **132.99 img/s** while hardware telemetry reportedly showed decoder utilization still well below saturation.

🍗 That suggests the present limit is still substantially influenced by software scheduling/feeding overhead rather than raw HEVC silicon throughput.

## 🍗 Correctness before speed

Csharp does not consider “it produced an image” sufficient validation.

The development comparison path decodes the same HEIC through the normal CPU path and the GPU path, normalizes the result to YCbCr 4:2:0 planes, and checks the decoded bytes.

Current result:

🍗 **CUVID backend:** 8/8 exact YCbCr plane hash matches.

🍗 **Native FFmpeg + NVDEC backend:** 8/8 exact YCbCr plane hash matches.

🍗 Malformed, empty, and mislabeled input samples tested during development fail cleanly instead of being accepted as valid HEIC images.

The current corpus is small and mostly ordinary opaque 8-bit HEIC. This is **not** a claim of complete HEIF compatibility.

## 🍗 What is NOT production-ready yet

🍗 Direct NVDECODE is still experimental and is not the validated default path.

🍗 The tested corpus does not yet represent the full HEIF feature zoo.

🍗 10-bit HEVC needs broader validation.

🍗 HDR behavior needs explicit validation.

🍗 Alpha/auxiliary image handling needs explicit validation.

🍗 HEIF grids/tiles need explicit validation.

🍗 Metadata/orientation/color-profile preservation needs end-to-end conversion tests.

🍗 Long-duration stress, leak, sanitizer, malformed-input, and driver-stability testing still need to be expanded.

🍗 A production HEIC → JPEG CLI with GPU-resident encode is still future work.

## 🍗 Direct NVDECODE: next major experiment

The next performance path removes FFmpeg from the hot decode path while **keeping the validated native FFmpeg backend as the reference implementation**.

Target architecture:

```text
libheif
   │
   ▼
Csharp direct decoder plugin
   │
   ▼
CUVID parser
   │
   ▼
persistent NVDECODE lanes
   │
   ▼
NVDEC
   │
   ▼
CUDA surface
```

The direct backend is being designed around:

🍗 persistent parser/decoder lanes rather than one decoder per image.

🍗 `cuvidReconfigureDecoder()` where compatible instead of destroy/recreate churn.

🍗 producer/consumer submission and output so mapping completed frames does not unnecessarily stop new decode work.

🍗 reusable/pinned host buffers where CPU output is required.

🍗 eventual GPU-resident output for downstream JPEG encoding.

🍗 optional intra-only optimization only after verifying that a coded HEIC image is actually safe for that mode.

## 🍗 Branch layout

🍗 `main` — public project landing page and stable high-level documentation. The main README may describe validated measurements produced on development branches before the implementation is merged.

🍗 `phase1-heicprobe` — active decoder implementation, profiling, benchmarking, and direct-NVDECODE development.

🍗 PR #1 — active development integration work; do not treat it as a stable release yet.

This separation is intentional: **documentation on `main` explains what has been measured; experimental code remains isolated until it earns the merge.**

## 🍗 Build status

The active implementation currently lives on `phase1-heicprobe`. Build requirements and exact commands may change while direct NVDECODE is being integrated.

The validated FFmpeg-backed development path uses CMake and development packages for:

🍗 libheif

🍗 libavcodec

🍗 libavutil

🍗 libswscale

🍗 pthreads

The direct path additionally requires compatible NVIDIA CUDA/NVDECODE development headers/libraries.

Do not copy build commands from this landing page into automation without checking the active branch documentation first.

## 🍗 End goal

The ideal fast path is:

```text
HEIC
  ↓
HEIF parse
  ↓
HEVC payload
  ↓
NVDEC
  ↓
CUDA-resident decoded image
  ↓
GPU colorspace work if needed
  ↓
GPU JPEG encode
  ↓
JPEG
```

The point is not merely to make an old CPU tolerate HEIC. The project is testing whether dedicated media hardware can turn large HEIC ingestion/conversion workloads into a high-throughput GPU pipeline in their own right.

## 🍗 Name

**Csharp** means **“see sharp.”**

🍗 It is written in C.

🍗 It is not written in C#.

🍗 Yes, the name is intentional.

🍗 No, we do not need to discuss Microsoft's naming decisions right now.

Anyway, about those HEIC images…
