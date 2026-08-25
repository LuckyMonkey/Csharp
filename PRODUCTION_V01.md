# Csharp v0.1 — End-to-End Completion Gate

Performance research is intentionally plateaued for v0.1. The direct NVDECODE backend is already fast enough to productize; the priority is now correctness, safe JPEG output, metadata behavior, fallback/error handling, and long-run stability.

## v0.1 promise

Csharp should take a supported normal 8-bit HEIC/HEIF still image, decode its HEVC payload with the direct NVIDIA NVDECODE backend, encode a standards-compatible JPEG, preserve supported metadata/orientation correctly, and either complete atomically or fail without damaging the input.

## Scope

🍗 Direct NVDECODE remains the primary experimental fast path; do not redesign it during v0.1 hardening unless a correctness or stability bug requires it.

🍗 Use a mature JPEG encoder first (prefer libjpeg-turbo through its C API). Do not block v0.1 on nvJPEG.

🍗 Preserve the original HEIC. Output should be written to a temporary sibling file and atomically renamed only after encode + metadata work succeeds.

🍗 Implement explicit JPEG quality control with a sane default and range validation.

🍗 Preserve EXIF, XMP, ICC/color information, and orientation where the source and JPEG representation allow it. Never apply orientation twice.

🍗 Unsupported HEIF features must be detected and rejected/fallback cleanly rather than silently producing a wrong JPEG. At minimum audit 10-bit, alpha, grids/tiles, HDR/auxiliary images, unsupported chroma, and malformed/truncated inputs.

🍗 The production CLI must have stable exit codes and useful stderr messages. No benchmark/debug spew by default.

🍗 Keep benchmark/probe executables available for development, but the user-facing converter should not require FFmpeg when built for the direct backend.

## Initial CLI

Target shape:

```text
csharp [options] INPUT.heic OUTPUT.jpg

--quality N       JPEG quality (validated range)
--overwrite       permit replacement of an existing output
--backend direct|cpu
--verbose         useful diagnostics without per-frame benchmark noise
--version
--help
```

Batch/directory traversal can follow after the single-file contract is boringly reliable.

## End-to-end pipeline

```text
HEIC/HEIF
  -> libheif container/metadata handling
  -> direct NVDECODE HEVC decode
  -> validated Y/Cb/Cr image
  -> libjpeg-turbo JPEG encode
  -> metadata/color/orientation handling
  -> fsync/close as appropriate
  -> atomic rename
  -> JPEG
```

## Safety rules

🍗 Inputs are read-only.

🍗 Never truncate or overwrite an existing destination unless `--overwrite` was explicitly supplied.

🍗 A failed conversion must remove its temporary output.

🍗 A process crash should leave, at worst, an identifiable temporary file—not a corrupt file at the requested final path.

🍗 Check all allocation sizes and image-dimension arithmetic for overflow.

🍗 Bound memory use and release CUDA/libheif/JPEG/file resources on every error path.

## Correctness gate

Before calling v0.1 usable:

🍗 Existing 8/8 exact CPU/direct YCbCr validation remains green.

🍗 Decode produced JPEGs with at least libjpeg/libjpeg-turbo and one independent common decoder/tool.

🍗 Verify dimensions and orientation for odd-sized samples as well as larger samples.

🍗 Verify EXIF/XMP/ICC behavior with fixtures that actually contain each metadata class; do not claim preservation without fixtures.

🍗 Re-open every produced JPEG during tests and fail the test on decoder warnings/errors.

🍗 Compare representative JPEG output against a CPU reference semantically (dimensions, orientation, metadata, and reasonable pixel error for lossy JPEG), not by exact compressed-file hash.

## Hardening gate

🍗 Strict `-Wall -Wextra -Wpedantic -Werror` build for production targets.

🍗 ASan + UBSan run over normal and malformed fixture corpora where CUDA/library interaction permits it.

🍗 Repeated conversion stress test: start at 1,000 files/iterations, then target 10,000 without crashes, GPU reset, runaway VRAM/RAM, FD growth, or corrupt output.

🍗 Test zero-byte files, JPEG-with-HEIC-extension, truncated HEIF, bad boxes, unsupported profiles, destination permission failures, full/failed writes where practical, and pre-existing destination behavior.

🍗 Test Ctrl-C/termination cleanup around temporary output.

## Performance policy for v0.1

Do not chase decode-only records during this phase. Measure end-to-end HEIC -> JPEG throughput and CPU usage after the converter works.

The current decode research remains useful as a baseline, but JPEG encoding, metadata rewriting, and filesystem I/O are allowed to become the bottleneck for v0.1. Optimize them only after the complete pipeline is profiled.

## Deferred until after v0.1

🍗 nvJPEG / GPU-resident JPEG encoding.

🍗 Producer/consumer NVDEC redesign.

🍗 Pinned asynchronous output staging unless end-to-end profiling clearly justifies it.

🍗 10-bit/HDR/grid/alpha fast paths.

🍗 PhotoSort integration.

🍗 Automatic directory-scale orchestration.

## Release criterion

v0.1 is ready when Csharp is more boring than impressive: supported files convert correctly, unsupported files fail predictably, originals are safe, metadata behavior is documented and tested, repeated runs do not leak or crash, and the command can be trusted in a real photo pipeline.
