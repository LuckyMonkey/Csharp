# Csharp Benchmarks

This document records **measured development results**, not marketing estimates.

The implementation that produced most of these numbers currently lives on `phase1-heicprobe`; `main` carries the public benchmark record so the project landing page reflects what has actually been demonstrated.

## 🍗 Test system

🍗 **CPU:** Intel Core i7-3770 (Ivy Bridge, 4 cores / 8 threads, no AVX2)

🍗 **GPU:** NVIDIA RTX 3050 6 GB (Ampere)

🍗 **OS:** Ubuntu 24.04

🍗 **NVIDIA driver:** 580.173.02 during the reported benchmark session

🍗 **CUDA environment:** 13.0 reported during development

🍗 **FFmpeg:** 6.1.1

🍗 **libheif runtime:** 1.17.6 during initial bring-up

## 🍗 Corpus

The current correctness/performance corpus is intentionally small and real rather than synthetic.

🍗 8 known-good HEIC files were used for exact CPU/GPU decode validation.

🍗 Dimensions range from roughly 375×353 through 1536×2048.

🍗 Current validated files are 8-bit HEVC.

🍗 Current validated files do not represent broad coverage of HDR, alpha, auxiliary images, grids, or 10-bit content.

🍗 Malformed, zero-byte, and mislabeled `.heic` candidates encountered during bring-up fail cleanly.

## 🍗 Correctness gate

Before throughput numbers are accepted, the same HEIC is decoded through the normal CPU reference path and the GPU backend.

The decoded Y, Cb, and Cr planes are hashed and compared after normalization to the same YCbCr 4:2:0 representation.

🍗 **CUVID backend:** 8/8 exact CPU/NVDEC plane hash matches.

🍗 **Native FFmpeg HEVC + CUDA/NVDEC backend:** 8/8 exact CPU/NVDEC plane hash matches.

A backend that fails this gate does not get to claim a benchmark result.

## 🍗 Historical progression

| Stage | Approx. throughput | What changed |
| --- | ---: | --- |
| CPU libheif baseline | ~11–14 img/s | CPU HEVC decode reference |
| Early one-at-a-time NVDEC | ~3.4 img/s | GPU worked, but startup/lifecycle dominated |
| Parallel NVDEC | ~19.3 img/s | Multiple independent image decodes in flight |
| Tuned `hevc_cuvid` | **37.54 img/s** | pooled contexts + concurrency |
| Native FFmpeg HEVC + NVDEC | **132.99 img/s** | native HEVC decoder with CUDA hardware acceleration |

The useful lesson is that **the NVDEC silicon was not the original problem; feeding many tiny independent HEVC still images efficiently was.**

## 🍗 CUVID worker sweep

Measured with true-warm pooled decoder state on the current corpus:

| Workers | Images/sec | Avg CPU cores |
| ---: | ---: | ---: |
| 1 | timeout / wedge in that run | — |
| 2 | 5.22 | 0.38 |
| 3 | 11.99 | 0.87 |
| 4 | 16.01 | 1.20 |
| 6 | 30.19 | 2.36 |
| 8 | 20.32 | 1.53 |
| 10 | 32.54 | 2.55 |
| 12 | **37.54** | 2.95 |
| 16 | 31.46 | 2.45 |
| 24 | 32.18 | 2.58 |
| 32 | 31.41 | 2.56 |

🍗 **Best measured CUVID throughput:** 37.54 img/s at 12 workers.

🍗 CUVID decoder utilization reportedly peaked around 12% in the observed telemetry.

🍗 The CUVID path flattened well below full NVDEC utilization and is retained primarily as a reference backend.

## 🍗 CUVID steady-state repeat

At 8 workers × 500 decodes:

| Repeat | Images/sec | Avg CPU cores |
| ---: | ---: | ---: |
| 1 | 21.54 | 1.58 |
| 2 | 21.02 | 1.56 |
| 3 | 20.50 | 1.51 |

Each timed run reported:

🍗 **AVCodecContext initializations during timed region:** 0

🍗 **Decoder reuses:** 500

This was important because earlier “warm” numbers still included context construction inside the measured interval.

## 🍗 Native FFmpeg HEVC + CUDA/NVDEC sweep

| Workers | Images/sec | Avg CPU cores |
| ---: | ---: | ---: |
| 4 | 65.88 | 2.44 |
| 8 | 66.39 | 2.51 |
| 12 | 103.32 | 4.47 |
| 16 | 112.07 | 4.39 |
| 24 | 121.39 | 5.09 |
| 32 | **132.99** | 5.64 |

🍗 **Current record:** 132.99 real HEIC decode operations/sec.

🍗 **Record worker count:** 32.

🍗 **CPU consumption at record:** about 5.64 CPU cores on average.

🍗 **Observed NVDEC utilization at record-class runs:** roughly 20% peak in the reported telemetry.

🍗 The native path had not yet reached an obvious NVDEC-hardware saturation point at the reported record.

## 🍗 Stage timing at the 132.99 img/s native result

Average measured plugin-stage time per image:

| Stage | ms/image |
| --- | ---: |
| Acquire decoder | 0.008 |
| Annex-B preparation | 0.001 |
| Decode path | **61.387** |
| GPU → CPU transfer | 9.098 |
| Output into libheif planes | 4.054 |
| Total plugin callback | 74.549 |

These are **per-image latency components under concurrent execution**, not serial throughput predictions. Multiple images are in flight simultaneously.

The measurements indicate:

🍗 decoder acquisition is effectively negligible in the warmed native path.

🍗 Annex-B preparation is effectively negligible after the in-place conversion work.

🍗 GPU-to-host transfer is meaningful but is not the dominant measured stage.

🍗 host output is also not the dominant measured stage.

🍗 the largest measured component remains the decode/scheduling path around feeding many independent still-image jobs.

## 🍗 CPU-headroom mode versus raw-speed mode

Earlier one-worker NVDEC tests were slow in wall-clock time but consumed only about 0.19 CPU cores. Parallelism changed that tradeoff.

Csharp therefore has two useful optimization targets:

🍗 **raw-speed mode:** feed the GPU aggressively and maximize images/sec.

🍗 **CPU-headroom mode:** accept lower wall-clock throughput in exchange for leaving host CPU capacity available for indexing, hashing, OCR, face detection, or other ingestion work.

The current project focus is **raw GPU throughput**, but the CPU-light mode remains useful for eventual integration into larger pipelines.

## 🍗 Direct NVDECODE record-to-beat

Direct NVIDIA NVDECODE development is the next major experiment.

The validated reference record is:

> **132.99 images/sec — native FFmpeg HEVC + CUDA/NVDEC — 32 workers**

A direct backend is considered interesting if it does one or more of the following:

🍗 exceeds 132.99 img/s on the same corpus and machine.

🍗 reaches similar throughput with materially lower CPU consumption.

🍗 drives substantially higher NVDEC utilization.

🍗 reduces per-image scheduling/decode overhead.

🍗 enables a cleaner GPU-resident path toward nvJPEG.

## 🍗 Benchmark caveats

🍗 The current corpus is only eight known-good ordinary HEIC images.

🍗 Small-image throughput is not equivalent to throughput on full-resolution modern phone-photo libraries.

🍗 The CPU baseline comes from an i7-3770 and should not be presented as representative of modern AVX2/AVX-512 CPUs.

🍗 Decoder utilization telemetry is observational and should be confirmed with deeper profiling when making architectural claims.

🍗 Decode-only benchmarks do not include the final cost of JPEG encoding, metadata preservation, filesystem I/O, or complete production conversion.

🍗 Exact-hash validation on eight files proves correctness for those files, not complete HEIF/HEVC compatibility.

## 🍗 Production benchmark checklist

Before a release-quality performance claim, expand the benchmark matrix to include:

🍗 hundreds or thousands of distinct HEIC files.

🍗 multiple camera/device generations.

🍗 full-resolution 12 MP / 24 MP / 48 MP photos where available.

🍗 8-bit and 10-bit HEVC.

🍗 grids/tiles.

🍗 HDR/color-profile variants.

🍗 alpha/auxiliary images.

🍗 repeated long-duration stress runs.

🍗 sanitizer and leak checks.

🍗 a modern AVX2 CPU reference implementation.

🍗 full HEIC → JPEG end-to-end throughput once nvJPEG or another encoder path is integrated.
