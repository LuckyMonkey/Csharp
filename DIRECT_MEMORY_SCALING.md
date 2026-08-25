# Direct NVDECODE memory-scaling plan

Current direct NVDECODE status is correctness-first and stable:

🍗 8/8 exact CPU/direct YCbCr hashes pass.

🍗 8 logical benchmark workers are stable with a default cap of 4 active direct decoder lanes.

🍗 The 8-lane startup failure was traced to `cuvidCreateDecoder()` returning `CUDA_ERROR_OUT_OF_MEMORY`, not to parser callback context ownership or GPU instability.

🍗 The current decoder creation path provisions every lane with `ulMaxWidth=8192` and `ulMaxHeight=8192`, even when the current coded image is much smaller.

🍗 The stable four-lane path currently produces roughly 51–53 real HEIC images/sec on the RTX 3050.

The next objective is to increase the number of simultaneously resident direct NVDECODE lanes without losing exact correctness or stability.

## Design principle

Do not treat `DIRECT_MAX_WIDTH` / `DIRECT_MAX_HEIGHT` as the decoder allocation size. Keep those values as hard safety limits only.

Each lane should instead have its own configured maximum coded geometry:

```text
hard safety limit: 8192 x 8192

lane 0: max 512 x 512
lane 1: max 512 x 512
lane 2: max 2048 x 2048
lane 3: max 2048 x 2048
...
```

The decoder should only reserve enough geometry for the class of images assigned to that lane. If a later image exceeds the lane's configured maximum, recreate or promote that lane deliberately rather than provisioning every decoder for 8K from birth.

## Phase 1 — measure memory instead of guessing

Add optional direct-backend memory telemetry.

Before and after each of these operations, sample CUDA free/total device memory with `cuMemGetInfo()` when `CSHARP_DIRECT_MEMORY_TRACE=1`:

🍗 CUDA context creation

🍗 parser creation

🍗 `cuvidCreateDecoder()`

🍗 `cuvidReconfigureDecoder()`

🍗 map/unmap

🍗 lane destruction

Log:

```text
lane
coded width/height
configured max width/height
min decode surfaces
actual decode surfaces
output surfaces
free bytes before
free bytes after
memory delta
```

This should remain opt-in and must not contaminate benchmark timing.

Produce a table for the real corpus showing approximate resident memory per decoder configuration.

## Phase 2 — adaptive maximum geometry

Replace the current unconditional:

```c
info.ulMaxWidth = DIRECT_MAX_WIDTH;
info.ulMaxHeight = DIRECT_MAX_HEIGHT;
```

with per-lane maximum geometry.

Start conservatively with size buckets rather than arbitrary exact dimensions.

Suggested first policy:

```text
<= 512 x 512       -> 512 x 512 bucket
<= 1024 x 1024     -> 1024 x 1024 bucket
<= 2048 x 2048     -> 2048 x 2048 bucket
<= 4096 x 4096     -> 4096 x 4096 bucket
otherwise          -> 8192 x 8192 bucket
```

Account for portrait orientation by comparing each coded dimension independently to the bucket limits; do not rotate the coded geometry merely for allocation.

Record on each lane:

```c
unsigned int max_width;
unsigned int max_height;
```

A lane may be reused/reconfigured only when the new coded geometry fits inside those maxima.

If it does not fit:

🍗 finish/unmap any outstanding work

🍗 destroy parser/decoder resources that depend on the old allocation

🍗 recreate the lane in the next suitable bucket

🍗 preserve correctness and lifecycle stats

Do not call `cuvidReconfigureDecoder()` with geometry exceeding the maximum dimensions used at decoder creation.

## Phase 3 — lane affinity by geometry class

The current pool can assign any free lane to any image. That can force small lanes to grow and large lanes to be wasted on tiny files.

Change admission to prefer:

1. an idle initialized lane whose max geometry already fits the image with the least excess area,
2. an idle uninitialized lane,
3. an idle initialized lane that can be safely recreated/promoted,
4. otherwise wait for a suitable lane.

This is a best-fit allocator for decoder sessions.

Track:

```text
exact-fit reuse
bucket-fit reuse
lane promotions
lane recreations
waiting time
```

The goal is to keep small decoder sessions resident instead of allowing one large image to turn every lane into an 8K-capable allocation forever.

## Phase 4 — test active-lane ceiling again

After adaptive geometry is correct, test:

```text
CSHARP_DIRECT_MAX_ACTIVE_LANES=4
5
6
7
8
10
12
16
```

For each value:

🍗 run 8/8 exact hash validation first

🍗 run at least ten mixed-corpus startup attempts

🍗 run 500 decodes when stable

🍗 record peak/free VRAM

🍗 record images/sec

🍗 record CPU cores

🍗 record decoder create/reconfigure/reuse counts

🍗 record CUDA OOMs

Do not simply raise the default because one run succeeds. The default must survive repeated mixed-resolution cold starts.

## Phase 5 — decode-surface pressure

For each `CUVIDEOFORMAT`, preserve `format->min_num_decode_surfaces` as the correctness floor.

Measure the current values across the corpus.

Do not blindly reduce `ulNumDecodeSurfaces` below the parser-provided minimum.

Investigate whether the current upper clamp of 16 is ever relevant for these still-image HEVC streams.

Log:

```text
min_num_decode_surfaces
configured ulNumDecodeSurfaces
```

If all tested still images request a small number, use exactly the required value unless NVIDIA documentation or testing demonstrates a reason for extra surfaces.

## Phase 6 — output surfaces

The synchronous direct path currently uses:

```c
info.ulNumOutputSurfaces = 2;
```

A/B test `1` versus `2` only after decoder geometry is fixed.

Requirements:

🍗 exact hashes remain 8/8

🍗 map/unmap remains stable

🍗 repeated 8+ lane tests remain stable

🍗 no mapped surface is reused before unmap

If one output surface is sufficient for the synchronous still-image path, it may reduce per-lane memory. Do not assume the saving is large until measured.

When producer/consumer pipelining is introduced later, the required output-surface count may need to increase again.

## Phase 7 — intra-only decoder allocation

Inspect the HEVC picture structure of normal HEIC coded items.

If the image streams are truly intra-only / IDR-only, test:

```c
info.ulIntraDecodeOnly = 1;
```

against the validated default `0`.

Measure:

🍗 decoder memory delta

🍗 maximum simultaneous resident lanes

🍗 throughput

🍗 exact hashes

🍗 stability

Never enable this for a stream that contains inter-predicted pictures.

This experiment is especially interesting because lower decoder resource requirements could allow substantially more resident still-image lanes.

## Phase 8 — shared CUDA context A/B

The current architecture gives each lane its own CUDA context.

After per-decoder allocation is right-sized, measure whether CUDA-context overhead itself materially limits lane residency.

Add an experimental mode:

```text
CSHARP_DIRECT_CONTEXT_MODE=per-lane
CSHARP_DIRECT_CONTEXT_MODE=shared
```

Shared mode should use one process-wide CUDA context while retaining separate parser/decoder lane state and synchronization.

Compare:

🍗 VRAM usage

🍗 active decoder count before OOM

🍗 throughput

🍗 CPU cost

🍗 stability

Do not make shared mode default unless exact correctness and stress testing prove it.

## Phase 9 — auto-tune lane count

Once memory costs are measurable, stop hardcoding a universal default of four.

At startup, direct NVDECODE should be able to choose a safe lane budget using:

```text
available VRAM
configured safety reserve
expected geometry bucket(s)
observed decoder allocation cost
hard user override
```

Possible policy:

```text
reserve 512–1024 MB for display/other GPU users
estimate resident decoder cost by bucket
admit lanes while projected free memory remains above reserve
```

A user override such as `CSHARP_DIRECT_MAX_ACTIVE_LANES` remains authoritative.

Never allocate until OOM as the normal control mechanism.

## Phase 10 — benchmark objective

Standing references:

```text
CPU libheif:              roughly 12–14 img/s
Direct NVDECODE, 4 lanes: roughly 51–53 img/s
Native FFmpeg + NVDEC:    132.99 img/s
```

The first goal is not immediately 133 img/s. The progression should be:

```text
4 stable resident lanes
        ↓
6 stable resident lanes
        ↓
8 stable resident lanes
        ↓
find throughput knee
        ↓
then optimize scheduling/output
```

At each step compute per-active-lane throughput. A large fall in per-lane throughput indicates decoder-engine or host scheduling contention even if more sessions fit in memory.

## Production decision tree

```text
Does adaptive geometry let >4 lanes fit reliably?
    |
    +-- no --> inspect surface/context memory next
    |
    +-- yes
          |
          +--> does throughput continue scaling?
                 |
                 +-- no --> GPU/session scheduling is the next wall
                 |
                 +-- yes --> keep raising safe lane count until knee
```

Then:

```text
Is direct near/above native FFmpeg throughput?
    |
    +-- yes --> direct becomes preferred experimental backend
    |
    +-- no
          |
          +--> does direct use substantially less CPU?
                 |
                 +-- yes --> keep as CPU-preserving mode
                 |
                 +-- no --> native remains production speed path
```

## Correctness and safety gates

Every memory optimization must retain:

🍗 `-Wall -Wextra -Wpedantic -Werror`

🍗 8/8 exact YCbCr hashes

🍗 repeated mixed-resolution cold-start stability

🍗 no `CUDA_ERROR_OUT_OF_MEMORY` in supported/default configuration

🍗 no GPU reset/X freeze/kernel instability

🍗 clean parser/decoder/context destruction

🍗 deterministic fallback when a requested geometry/profile cannot be served

## Immediate implementation order

1. Add `cuMemGetInfo()` memory tracing.
2. Add per-lane `max_width` / `max_height` state.
3. Create decoder with the smallest safe geometry bucket.
4. Add best-fit geometry-aware lane selection.
5. Revalidate exact hashes.
6. Measure per-bucket decoder memory.
7. Sweep active lanes 4 -> 8 -> 12+ as memory allows.
8. Test output surfaces 1 vs 2.
9. Test `ulIntraDecodeOnly` when stream structure proves it legal.
10. Test shared CUDA context only if context overhead remains material.
11. Build automatic safe lane-budget selection.
12. Resume producer/consumer and pinned-memory work only after residency is no longer the dominant bottleneck.

The immediate question is now narrow and measurable:

> How many correctly right-sized HEVC still-image decoder sessions can the RTX 3050 keep resident at once, and where does throughput stop scaling?
