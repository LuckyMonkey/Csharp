# Direct NVDECODE backend roadmap

Csharp currently has a validated FFmpeg/libavcodec reference backend. The next performance backend removes FFmpeg from the decode hot path and talks directly to NVIDIA's NVDECODE API while keeping libheif responsible for HEIF container semantics.

## Reference result to preserve

The native FFmpeg HEVC + CUDA backend has demonstrated exact CPU/NVDEC YCbCr hashes on the current eight-file corpus and approximately 133 real HEIC images/sec at 32 workers on the development RTX 3050 system. Direct NVDECODE must not replace this backend until it passes the same correctness gate and exceeds or otherwise materially improves on the reference.

## Target architecture

```text
libheif decoder-plugin push_data()
        |
        v
HEVC access unit / parameter sets
        |
        v
persistent direct-NVDECODE lane
        |
        +--> CUVID parser
        +--> CUVID decoder
        +--> reusable output surfaces
        |
        v
completed-picture queue
        |
        v
map/copy/output worker
        |
        v
libheif Y/Cb/Cr image
```

## Phase D0: direct API smoke test

Build with:

```bash
cmake -S . -B build-direct -DCSHARP_ENABLE_DIRECT_NVDEC=ON
cmake --build build-direct -j
./build-direct/csharp-nvdecode-probe
```

This path contains no FFmpeg/libavcodec calls. It verifies CUDA driver initialization and `cuvidGetDecoderCaps()` for HEVC 8-bit 4:2:0.

## Phase D1: one-image direct decode

Implement a separate libheif decoder plugin ID such as `csharp-direct-nvdec`.

Required components:

- persistent CUDA driver context
- `cuvidCreateVideoParser()` with HEVC codec type
- sequence callback that creates or reconfigures the decoder
- decode callback that calls `cuvidDecodePicture()`
- display callback that queues the completed picture
- `cuvidMapVideoFrame()` only from the output side
- exact conversion into a libheif Y/Cb/Cr image

The existing FFmpeg native backend remains the correctness oracle.

## Phase D2: persistent lanes

The unit of reuse is a decoder lane, not an image.

Each lane should own its parser, decoder, output queue and synchronization. Avoid creating or destroying decoder sessions per HEIC. Group or route compatible images to lanes where useful.

Set `ulMaxWidth` and `ulMaxHeight` high enough for the intended corpus and use `cuvidReconfigureDecoder()` when legal instead of destroying a decoder for ordinary resolution changes.

Only set `ulIntraDecodeOnly=1` after inspecting the actual submitted HEVC picture structure and proving the stream is intra-only. It must remain disabled for any stream containing inter pictures.

## Phase D3: producer/consumer scheduling

Do not map each output immediately on the submitting thread.

Use separate submission and output work so decoding can continue while completed frames are mapped/copied. Benchmark lane counts independently from host thread counts.

Measure:

- submit/parser time
- decode callback time
- decoder reconfigure time
- wait-for-picture time
- map time
- device-to-host copy time
- libheif output time
- total images/sec
- process CPU cores
- NVDEC utilization

## Phase D4: host-output optimization

For decode-only libheif compatibility, prefer reusable/pinned host output storage when safe and useful. Avoid per-image malloc/free churn.

Do not introduce a GPU->CPU->GPU round trip into the eventual converter. The long-term HEIC->JPEG path should keep decoded surfaces on-device and feed CUDA/nvJPEG directly when possible.

## Production gate

Before promoting the backend:

- exact hashes against the validated CPU/reference backend
- malformed input fails cleanly
- 8-bit ordinary HEIC supported
- unsupported profiles cleanly fall back
- long-run stress test with no decoder wedges or resource growth
- ASan/UBSan-clean host code where compatible
- valgrind or equivalent leak review for non-CUDA allocations
- deterministic shutdown and CUDA/NVDEC resource destruction
- documented minimum driver/SDK requirements
- benchmark results reproducible over a larger real corpus

## Backend policy

Keep all backends selectable during development:

```text
cpu/reference
ffmpeg-native-nvdec
ffmpeg-cuvid
nvdecode-direct
```

Production `auto` mode should select the fastest validated compatible backend and fall back safely rather than guessing.
