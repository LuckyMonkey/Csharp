# Csharp HEIC/HEIF Fixture Matrix

This directory defines the fixture contract for v0.1 hardening. Do not commit personal photographs or redistributability-unclear samples to the repository. Local fixture corpora may live outside Git and be described with a manifest.

## Required coverage

🍗 Baseline opaque 8-bit 4:2:0 HEIC expected to take the direct NVDEC path.

🍗 Odd visible dimensions with padded coded dimensions.

🍗 Multiple common phone resolutions, including at least one >2 MP image.

🍗 EXIF metadata with orientation values other than 1.

🍗 XMP metadata.

🍗 Embedded ICC profile.

🍗 10-bit HEIC expected to take CPU fallback until a direct path is implemented.

🍗 HDR/wide-gamut HEIF/HEIC with documented transfer/color properties.

🍗 Grid/tiled HEIF primary image.

🍗 Primary image with alpha; JPEG result should be composited onto white.

🍗 Auxiliary/depth images attached to a normal primary image; sidecars must not replace the primary output.

🍗 Unusual chroma/profile inputs that libheif can normalize.

🍗 Truncated HEIF.

🍗 Zero-byte file.

🍗 Non-HEIF content mislabeled with a `.heic` extension.

🍗 Malformed box lengths / corrupted item data where fixtures can be generated safely.

## Local manifest

For a local corpus, create a TSV file outside Git with columns:

```text
id\tpath\texpected_result\texpected_path\tfeature\tnotes
```

Recommended values:

`expected_result`: `success`, `reject`

`expected_path`: `direct`, `cpu-fallback`, `either`, `n/a`

Example:

```text
odd-379\t/tmp/csharp-fixtures/odd-379.heic\tsuccess\tdirect\todd-dimensions\t379x353 visible image
hdr10\t/tmp/csharp-fixtures/hdr10.heic\tsuccess\tcpu-fallback\t10-bit-hdr\tfixture source/license recorded separately
truncated\t/tmp/csharp-fixtures/truncated.heic\treject\tn/a\tmalformed\tintentionally truncated copy
```

## Acceptance rules

🍗 A successful conversion must produce a structurally valid JPEG and preserve the expected displayed dimensions/orientation.

🍗 A rejected input must return non-zero and must not leave the requested final destination behind.

🍗 Backend-path assertions should be based on explicit machine-readable reporting once available. Until then, verbose logs may be inspected but are not a permanent test API.

🍗 Lossy JPEG pixel output is not compared by compressed-byte hash. Compare dimensions, orientation/metadata semantics, and decoded pixel similarity when needed.

🍗 Every fixture claiming EXIF/XMP/ICC/HDR/alpha/grid behavior should be independently characterized before it becomes a regression oracle.

## Next harness upgrade

The converter should expose a machine-readable result mode such as `--report-json` or `--report-path` containing at least:

```text
requested_backend
decode_path
fallback_reason
width
height
source_bit_depth
source_chroma
has_alpha
metadata_written
output_bytes
```

That reporting should become the basis for asserting that an ostensibly successful `--backend direct` test actually exercised NVDEC rather than silently passing through the CPU fallback.
