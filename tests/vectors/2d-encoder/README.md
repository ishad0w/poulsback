# 2D encoder vectors

Black-box vectors for the [2D encoder contract](../../../docs/native-2d.md).
They are black-box outputs of the reference encoder's public functions; every value here is what that
encoder returned, not what a model predicts. They are data, under the root MIT licence
(`LICENSE`). `MANIFEST.json` records each file's vector count and SHA-256 hash.
Replaying the checked-in data does not require its generator.

A new encoder is equivalent when it returns the same status, count and words for every line,
and the same colour words as `rgb565-fill-colour.bin`.

## Files

| File | Calls |
| --- | --- |
| `raster-fill.jsonl` | fill: every format × ROP, pixel packing, geometry and clipping, capacity, accepted limits, every refusal |
| `raster-copy.jsonl` | copy: every format × ROP between two objects and inside one, every displacement sign (the four orders and none), clipping with orders, distinct layouts, sizes of 8, limits, refusals, storage identity and allocation ranges, capacity |
| `raster-precedence.jsonl` | one fault or two faults per call, which pins the order of the checks |
| `raster-random.jsonl` | 2,000 seeded random fills and copies (formats, ROPs, layouts, signed origins, clips, a few faults) |
| `v1.jsonl` | the narrower compatibility interface (ARGB8888 only, no clipping, forward copies) |
| `rgb565-fill-colour.bin` | 65,536 little-endian 32-bit words: entry *p* is the colour word (word 4) of a 1×1 RGB565 fill of pixel *p* |

## Line format

Each `.jsonl` line is one call:

| Key | Meaning |
| --- | --- |
| `id` | a unique name: group, then the case |
| `api` | `fill` or `copy` (the general interface); `v1_fill` or `v1_copy` (the compatibility interface) |
| `src`, `dst` | a surface, or `null` when the call passes a null pointer for it |
| `src_rect`, `dst_rect` (copy), `rect` (fill) | a rectangle, or `null` for a null pointer |
| `clip` | general interface only: a rectangle, or `null` for no clip (a null clip is allowed) |
| `pixel` | fills: the 32-bit pixel argument, hex |
| `rop` | general interface only: the ROP argument as a signed integer (16, −1 and so on are refusal cases) |
| `capacity` | the capacity argument, in 32-bit words |
| `out_null`, `written_null` | whether the output buffer or the count pointer is null |
| `status` | the returned status name |
| `written` | the count after the call on `OK`; `null` on any other status |
| `words` | the words written, hex, `written` of them (empty for `OK` with 0 and for errors) |

A surface is `storage` (general interface only), `offset` and `allocation` (hex strings: they are
64-bit), `width`, `height`, `stride` (bytes) and `format` (the enumeration value: 1 ARGB8888,
2 XRGB8888, 3 RGB565; other values are refusal cases). A rectangle is `x`, `y` (signed in the general
interface, unsigned in v1), `width`, `height`.

## Replay invariants

Fill the output buffer (24 words) and count with canaries before each call.
Reject any breach of these invariants:

- every status other than `OK` left the count and every word unchanged;
- `OK` wrote exactly `written` words from the start of the buffer and no word after them, with
  `written` at most `capacity`;
- `OK` with `written` 0 (nothing left after clipping) changed no word;
- no input object changed.

Use independent canary values when replaying the vectors.
