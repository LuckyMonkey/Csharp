# Csharp Project Status

This file is the deliberately boring truth table for the project.

The public README explains the idea. `BENCHMARKS.md` records measured performance. This file separates **what works now**, **what is experimental**, and **what is still planned**.

## 🍗 VALIDATED NOW

🍗 HEIC/HEIF files can be opened through libheif and their HEVC-coded primary image can be handed to a custom Csharp decoder plugin.

🍗 NVIDIA NVDEC can decode the tested real HEIC payloads on the RTX 3050.

🍗 The `hevc_cuvid` reference backend produces exact CPU/NVDEC YCbCr plane hashes on all 8 current known-good test files.

🍗 The native FFmpeg HEVC decoder with CUDA/NVDEC hardware acceleration also produces exact CPU/NVDEC YCbCr plane hashes on all 8 current known-good test files.

🍗 Parallel decoding materially increases throughput for independent still-image HEVC payloads.

🍗 Native FFmpeg HEVC + NVDEC currently holds the measured record at **132.99 images/sec** on the current development corpus.

🍗 Decoder pooling/prewarming works well enough that measured warm runs can avoid creating AVCodecContext instances inside the timed interval.

🍗 The benchmark infrastructure reports throughput, process CPU use, worker count, decoder/context reuse, and internal stage timing.

## 🍗 EXPERIMENTAL NOW

🍗 Direct NVIDIA NVDECODE integration that removes FFmpeg from the decode hot path.

🍗 Direct capability probing through CUDA/NVCUVID.

🍗 Persistent direct-NVDECODE lane design.

🍗 `cuvidReconfigureDecoder()` for resolution/configuration changes without destroy/recreate churn.

🍗 Producer/consumer submission versus blocking map/output behavior.

🍗 Reusable pinned host buffers for direct output.

🍗 Intra-only NVDECODE optimization after validating actual picture structure.

🍗 Worker/lane scaling beyond the current FFmpeg-native record.

## 🍗 NOT YET CLAIMED

🍗 Complete HEIF compatibility.

🍗 Production-quality support for arbitrary Apple HEIC libraries.

🍗 Validated 10-bit HEVC coverage.

🍗 Validated HDR coverage.

🍗 Validated alpha/auxiliary-image coverage.

🍗 Validated HEIF grid/tile coverage.

🍗 Complete orientation/color-profile/metadata preservation across HEIC → JPEG conversion.

🍗 A stable public CLI interface.

🍗 A stable library ABI/API.

🍗 GPU-resident end-to-end HEIC → JPEG conversion.

🍗 A performance claim versus modern AVX2/AVX-512 CPUs.

## 🍗 NEXT PERFORMANCE MILESTONE

The direct NVDECODE backend must first match the validated native FFmpeg backend's correctness.

Then it must attack the current record:

> **132.99 images/sec**

Interesting outcomes include:

🍗 higher throughput.

🍗 similar throughput with substantially less CPU.

🍗 higher observed NVDEC utilization.

🍗 lower decode/scheduling latency.

🍗 cleaner access to GPU-resident decoded surfaces.

If the direct path loses badly to the native FFmpeg path, it remains an experiment rather than becoming the default just because it is lower-level.

## 🍗 PRODUCTIONIZATION GATE

Before Csharp should be described as production-ready, require at minimum:

🍗 broad real-world test corpus.

🍗 deterministic fallback behavior for unsupported HEIF variants.

🍗 explicit input limits and malformed-input handling.

🍗 sanitizer runs.

🍗 leak/resource-lifetime testing.

🍗 long stress runs across many thousands of decodes.

🍗 GPU/driver recovery behavior documented.

🍗 thread-safe lane/pool ownership.

🍗 clean shutdown paths.

🍗 build/install documentation.

🍗 versioned CLI behavior.

🍗 end-to-end HEIC → JPEG correctness checks including metadata.

## 🍗 EVENTUAL FAST PATH

```text
HEIC
  ↓
libheif container handling
  ↓
HEVC coded image
  ↓
direct NVDECODE
  ↓
CUDA-resident image surface
  ↓
GPU colorspace processing if required
  ↓
GPU JPEG encoder
  ↓
JPEG
```

The important production goal is to avoid unnecessary full-resolution image transfers between CPU and GPU memory.

## 🍗 BRANCH POLICY

🍗 `main` is allowed to document **validated development results** before the implementation producing those results is merged.

🍗 `phase1-heicprobe` is the active engineering branch.

🍗 Experimental direct-NVDECODE code stays isolated until correctness and stability are demonstrated.

🍗 Documentation should always label results as validated, experimental, or planned.

🍗 Do not merge an implementation solely because it wins a benchmark.

## 🍗 THE NAME

🍗 Csharp = “see sharp.”

🍗 Written in C.

🍗 Not C#.

🍗 The ambiguity is a feature.
