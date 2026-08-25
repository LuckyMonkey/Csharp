# Direct NVDECODE stability runbook

This document is for the `phase1-heicprobe` branch while the direct NVDECODE backend is being stabilized under concurrent HEIC prewarm.

## Known-good baseline

- CUDA 13 direct build passes with `-Wall -Wextra -Wpedantic -Werror`.
- Direct decode is exact on the current 8-image 8-bit 4:2:0 HEIC corpus.
- Odd visible dimensions and coded-surface padding are handled by mapping the coded NV12 surface and manually cropping luma/chroma.
- Persistent lanes and `cuvidReconfigureDecoder()` work sequentially.
- Bulk output copy is in place: one 2D Y transfer, one 2D UV transfer, host UV deinterleave.
- 2 and 4 concurrent workers are stable.
- 8-worker concurrent prewarm can nondeterministically fail with `CUVID produced no decoded picture`.
- Native FFmpeg+NVDEC is the validated performance reference and must remain untouched.

## Current failure signature

The benchmark prewarms all lanes concurrently before timing:

```text
8 workers + 8 distinct HEICs
  -> each worker enters libheif decode
  -> direct plugin acquires/initializes a lane
  -> parser sequence callback creates/reconfigures decoder
  -> packet + EOS are submitted
  -> some lanes never observe a display-ready picture
```

Failures vary by image/run. This strongly suggests lifecycle/concurrency/context behavior, not image-specific bitstream corruption.

## Rule zero

Do not optimize until 8-worker prewarm is boringly stable. Do not weaken exact hashes. Do not special-case filenames.

## Trace tooling

Build direct + tracer:

```bash
cmake -S . -B build-direct -DCSHARP_ENABLE_DIRECT_NVDEC=ON
cmake --build build-direct -j"$(nproc)"
```

Run through the interposer:

```bash
CSHARP_NVCUVID_TRACE=1 \
LD_PRELOAD="$PWD/build-direct/libcsharp-nvcuvid-trace.so" \
timeout 120 ./build-direct/csharp-bench-parallel \
  --backend direct --repeats 1 8 8 \
  /tmp/csharp-heic-corpus-20260821/{02,04,05,06,07,08,10,11}.heic \
  2>/tmp/csharp-nvcuvid-trace.log
```

For each failing lane determine whether the trace contains, in order:

1. parser created successfully
2. sequence callback caused decoder create/reconfigure
3. `cuvidDecodePicture()` returned success
4. EOS parse returned success
5. display callback occurred / picture index became available
6. map/unmap occurred

The distinction matters:

- decode succeeds, no display -> parser/EOS/display lifecycle problem
- create/reconfigure fails -> decoder/context lifecycle problem
- decode fails -> bitstream/parser/context state problem
- map fails -> surface/context ownership problem

## Experiment matrix

Run every experiment at least 5 times before calling it stable.

### A. concurrency threshold

Use the eight-file corpus and test workers:

```text
1 2 3 4 5 6 7 8
```

Keep total prewarm work equal to worker count. Record first concurrency at which nondeterminism appears.

### B. same bitstream vs mixed bitstreams

Compare:

```text
8 workers -> same 02.heic
8 workers -> same 10.heic
8 workers -> eight distinct files
```

If same-file succeeds and mixed-file fails, focus on concurrent sequence/reconfigure/configuration changes.

### C. cold lanes vs already-initialized lanes

Separate:

```text
cold: 8 lanes initialized concurrently
warm: initialize lanes safely first, then issue 8 concurrent decodes
```

If only cold fails, lane/parser/decoder construction is implicated. If warm also fails, decode/EOS/display lifecycle is implicated.

### D. decoder construction lifecycle

A/B only for diagnosis:

```text
D1 persistent parser + persistent decoder
D2 persistent parser + fresh decoder per image
D3 fresh parser per image + persistent decoder where legal
D4 fresh parser + fresh decoder per image
```

Do not keep a slower lifecycle merely because it hides the bug. Use it to localize ownership/state.

### E. EOS lifecycle

Current still-image path submits a coded packet and then:

```text
CUVID_PKT_ENDOFSTREAM | CUVID_PKT_NOTIFY_EOS
```

Test whether EOS makes a persistent parser unsuitable for subsequent independent pictures.

Compare:

```text
E1 packet + EOS every image (current)
E2 new parser per image, EOS every image
E3 persistent parser, discontinuity between independent images, EOS only on lane shutdown
E4 persistent parser with documented flush/reset pattern if required by NVDECODE
```

Any no-EOS mode must still deterministically deliver the display callback before returning the image. Do not busy-wait indefinitely.

### F. discontinuity

The direct plugin currently adds `CUVID_PKT_DISCONTINUITY` after the first decoded image in a lane. A/B:

```text
F1 current discontinuity behavior
F2 fresh parser per still, no discontinuity needed
F3 persistent parser with explicit independent-stream reset discipline
```

### G. CUDA context ownership

For every plugin callback and every NVDECODE call, verify which `CUcontext` is current.

The lane's CUDA context must be current whenever operations requiring it execute. Parser callbacks may run synchronously inside `cuvidParseVideoData`; do not assume callback context ownership without tracing it.

If callback current-context mismatches appear, explicitly push/pop the lane context at the narrowest safe boundary.

Never leave a pushed context on a worker thread after decode returns.

### H. decoder surface pressure

Record `min_num_decode_surfaces`, configured decode surfaces, and output surfaces.

Test whether 8 concurrent lanes are creating unnecessarily large surface pools. Do not reduce below NVDEC requirements merely to force success.

### I. one CUDA context vs many

Only if evidence points at independent-context interaction:

```text
I1 current one CUcontext per lane
I2 one process CUDA context shared by lanes, separate parser/decoder objects
```

This is a controlled architecture experiment, not a blind rewrite. Preserve per-lane parser/decoder synchronization.

## Required opt-in direct-plugin diagnostics

Add an environment-controlled diagnostic mode, e.g. `CSHARP_DIRECT_TRACE=1`, which reports:

- lane index
- pthread/thread identifier
- lane CUDA context
- currently active CUDA context
- parser pointer
- decoder pointer
- sequence callback count
- decode callback count
- display callback count
- coded dimensions
- display dimensions/crop
- packet flags, payload size, timestamp
- CUDA/NVCUVID call result name on failure
- display picture index
- map/unmap result

Diagnostics must be off by default and must not pollute timed benchmark output.

## Stability gate

A fix is accepted only after:

1. project builds with warnings as errors
2. all eight exact CPU/direct hashes pass
3. 2-worker smoke passes
4. 4-worker smoke passes
5. 8 workers with same file passes 5/5
6. 8 workers with eight distinct files passes 10/10
7. repeated resolution changes pass
8. no GPU reset, X freeze, kernel issue, or resource leak is observed

## Performance work after stability

Once 8-worker direct decode is stable, resume in this order:

1. 1-32 direct worker sweep
2. NVDEC utilization capture
3. stage timings: parse/decode/wait/map/copy/output
4. producer/consumer submission vs mapping
5. pinned reusable host output buffers if copy is material
6. intra-only decode A/B after verifying picture structure
7. GPU-side NV12 -> planar/colorspace only if host output remains material
8. eventual GPU-resident handoff to nvJPEG

Reference record to beat remains the validated native FFmpeg+NVDEC result: ~132.99 real HEIC images/sec on the current RTX 3050 test host.
