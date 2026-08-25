# Csharp NVDEC performance assault

The optimization target is maximum real HEIC decode throughput on the RTX 3050 while preserving exact CPU/NVDEC decoded-plane correctness.

## Current evidence

Measured on the eight-file real HEIC corpus before this update:

| Workers | Images/sec | Avg CPU cores |
|---:|---:|---:|
| 1 | 3.47 | 0.19 |
| 2 | 4.64 | 0.38 |
| 4 | 14.80 | 1.20 |
| 8 | 19.26 | 1.75 |

The 8-worker result is already faster than the earlier CPU-only result, and the scaling strongly suggests the NVDEC engine was underfed rather than saturated.

A subtle but useful property of the existing scheduler is configuration affinity: with eight files and eight workers, each worker repeatedly receives the same file. Keep that behavior deliberate while testing persistent decoder lanes.

## Changes in the deep-performance update

- pool capacity raised to 32 contexts
- decoder slots reserved under the global lock, while expensive `avcodec_open2()` runs outside the lock
- atomic counters for genuinely concurrent benchmarking
- FFmpeg packet storage always includes zeroed `AV_INPUT_BUFFER_PADDING_SIZE`
- four-byte HEVC length prefixes are converted to four-byte Annex-B start codes in place
- the temporary planar YUV420P allocation/copy is removed
- downloaded NV12 is copied/deinterleaved directly into libheif Y/Cb/Cr planes
- non-NV12 fallback uses swscale directly into libheif planes
- per-stage timing: acquire, Annex-B preparation, codec decode, CUDA transfer, output, total callback
- selectable `cuvid`, `native`, and `auto` FFmpeg decoder backends
- parallel benchmark prewarms all decoder lanes/configurations before timing
- worker threads are created before the timed region and released together through a condition-variable gate
- repeatable benchmark runs via `--repeats`

## Correctness gate

Before performance work, all eight known-good HEICs must still pass exact CPU/NVDEC YCbCr plane hashes through `heicprobe --compare-nvdec` using the CUVID backend.

Do not weaken the exact-hash test.

## Benchmark sweep

Use long runs so startup noise does not dominate:

```bash
./build/csharp-bench-parallel --backend cuvid --repeats 3 8 500 \
  /tmp/csharp-heic-corpus-20260821/{02,04,05,06,07,08,10,11}.heic
```

Then test native:

```bash
./build/csharp-bench-parallel --backend native --repeats 3 8 500 \
  /tmp/csharp-heic-corpus-20260821/{02,04,05,06,07,08,10,11}.heic
```

Sweep worker counts:

```text
1 2 3 4 6 8 10 12 16 24 32
```

Stop increasing concurrency when throughput flattens, errors appear, or CPU cost rises without throughput gain.

While benchmarking, observe the decode engine and PCIe behavior where available:

```bash
nvidia-smi dmon -s uct
```

## How to read stage timing

- high `acquire`: pool affinity/reuse is wrong
- high `annexb`: compressed-stream preparation is still too expensive
- high `decode`: FFmpeg/CUVID parser/session work is dominating; compare native NVDEC, then direct NVDECODE
- high `transfer`: GPU-to-host synchronization/copy is the next wall; test independent CUDA contexts/streams and pinned memory
- high `output`: host NV12 deinterleave/copy dominates; consider CUDA UV split or direct GPU-resident downstream output

## Likely next wall: shared CUDA transfer stream

The current plugin shares one FFmpeg CUDA hardware device context. FFmpeg's CUDA hardware context commonly uses the default CUDA stream for transfers, and CPU frame download synchronizes before returning. If decode utilization remains low while transfer time is high, isolate this experimentally:

1. current shared CUDA device context
2. one CUDA AVHWDeviceContext per persistent decoder lane
3. one primary CUDA context with a separate non-default CUstream per lane
4. direct NVDECODE with explicit streams and pinned host buffers

Keep each topology change isolated so benchmark attribution remains meaningful.

## Direct NVDECODE roadmap

Do not jump here until the FFmpeg experiments are measured. If necessary, direct NVDECODE should use persistent producer/consumer lanes around CUVID parser/decoder APIs, `cuvidReconfigureDecoder()` for changing dimensions, explicit CUDA streams, pinned host buffers, and `ulIntraDecodeOnly` only after verifying the HEIC stream is truly intra-only.

For the eventual HEIC -> JPEG path, avoid CPU download entirely where possible: keep the decoded surface GPU-resident and hand GPU-side converted data to nvJPEG.
