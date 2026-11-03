# Video envelope and MPEG-2 descriptor contracts

This document specifies the CPU interfaces in [msvdx_proto.h](../src/video/msvdx_proto.h)
and [mpeg2_va029.h](../src/video/mpeg2_va029.h). They validate and prepare bytes;
they do not allocate device memory, access registers or execute firmware.
[Video](video.md) owns transport, ownership and device-integration boundaries.

Multi-byte input fields are little-endian. Byte spans may be unaligned.
Operations validate before replacing an output or patching a span; refusal
leaves caller-owned output unchanged. Stream scans return zero counts on refusal.
Checks run in the order described below, so the first failing condition selects
the status. The [reference vectors](../tests/vectors/msvdx-video/README.md)
pin exact results, including refusal precedence.

## Envelope layouts

Byte 0 is the total envelope length in bytes, and byte 1 is its ID.
`msvdx_proto_envelope_size` returns the size below, or zero for another ID.

| ID | Direction | Bytes | Fields after the two-byte header, by byte offset |
| --- | --- | ---: | --- |
| `0x81` render | host → MTX | 32 | 2: u16 buffer-size fields; 4: u32 PTD; 8: u32 LLDMA address; 12: u32 context; 16: u32 fence; 20: u32 operating mode; 24: u16 first macroblock; 26: u16 last macroblock; 28: u32 flags |
| `0xc0` completed | MTX → host | 12 | 2: uninterpreted u16 header; 4: u32 fence; 8: u32 flags |
| `0xc2` deblock required | MTX → host | 8 | 2: uninterpreted u16 header; 4: u32 context |
| `0xc5` failed | MTX → host | 12 | 2: u16 flags; 4: u32 fence; 8: u32 interrupt status |
| `0xc7` panic | MTX → host | 12 | Same field layout as `0xc5` |

The render halfword at byte 2 has a low 12-bit `buffer_size` and preserved
bits 15:12, `buffer_size_high`, kept in place rather than shifted. The codec
does not infer a storage allocation or hardware size unit from this field.
Operating mode, addresses, context and flags are serialized values, not proof
that a device may use them. PTD bit 0 denotes an MMU-invalidate request;
page-directory ownership and physical-address validity belong to the provider.

### Encoding and decoding

`msvdx_message_encode` checks pointers and direction, known ID, matching message
direction, output capacity, then the render size fields. A low-size value with
bits above bit 11, or a high-size value with any low 12 bits, is a range error.
Success writes exactly the envelope size and leaves subsequent bytes untouched.
The output may overlap the message record.

`msvdx_message_decode` checks pointers/direction, a minimum two-byte input,
known ID, matching direction, then exact length and the size byte. It accepts
exactly one whole envelope. Success replaces the complete message record;
input may overlap it. It separates the render size halfword into its low and
high fields and preserves the uninterpreted reply header.

The codec statuses distinguish argument, length, unsupported ID, wrong direction
and range errors. Unknown IDs, including ring padding ID 0, are unsupported
by the envelope codec.

## Streams and patching

`msvdx_proto_scan` accepts consecutive complete envelopes of one direction,
without padding or trailing bytes. Byte and message budgets must be positive
caller limits. Replies may repeat and occur in any order.

Validation checks arguments, nonempty length, then the total byte budget before
reading the span. At each envelope it checks the message budget first, then a
two-byte header and a size of at least two within the remaining span, and
decodes that exact envelope. Success reports message and render counts;
failure reports both counts as zero and writes no bytes.

`msvdx_proto_stamp_render` first scans the entire span as host → MTX. Only a
successful scan permits patching: every render gets the supplied PTD at byte 4
and the same fence at byte 16. `invalidate_first` ORs bit 0 into the first
render's PTD; other envelopes receive the PTD exactly as supplied, including
any bit 0 already set. No other bytes change. This transaction does not prove
that the firmware accepts the batch or that its fence represents final use.

## Host-ring plan

`msvdx_ring_plan_render` plans one 32-byte render envelope in a 100-word ring.
Indices count 32-bit words and must be 0–99. Equal read/write indices mean
empty; one word is reserved so full cannot look empty.

The planner checks pointers, indices, then an exact valid host → MTX render
packet. With read index `r` and write index `w`, it computes:

- padding: `100 - w` when `w + 8 > 100`, otherwise zero;
- words needed: padding plus eight;
- free words: `(r + 100 - w - 1) % 100`;
- final write index: `(w + words needed) % 100`.

Insufficient space is `MSVDX_RING_FULL`. A successful plan contains the input
indices, final index, padding count and at most 15 host-order words. Padding
begins with a word whose low byte is its byte size and whose ID is zero; its
remaining words are zero. The eight little-endian packet words follow, and
unused plan words are zero. A packet starting at 92 ends at zero without
padding; starts at 93–99 require padding. Packet and plan may overlap.

The plan changes neither ring storage nor indices. It describes padding and
packet publication under one final index update. Applying that policy to
physical firmware, including visibility, kick, wrap acceptance and replies,
requires separate device qualification. Index, malformed-packet and full-ring
refusals leave the plan unchanged.

## MPEG-2 descriptor layouts

These are the selected i386 libva 0.29 layouts, not arbitrary modern libva
structures. References are raw surface identifiers; the helpers do not resolve
or own their surfaces.

| Descriptor | Bytes | Layout by byte offset |
| --- | ---: | --- |
| Picture | 24 | 0: u16 width; 2: u16 height; 4: u32 forward reference; 8: u32 backward reference; 12: u32 coding type; 16: u32 f_code; 20: u32 coding extension |
| IQ | 272 | 0/4/8/12: u32 load flags for intra/non-intra/chroma-intra/chroma-non-intra; 16/80/144/208: the corresponding 64-byte matrices in zigzag order |
| Slice | 28 | Seven u32 fields at 0/4/8/12/16/20/24: data size, data byte offset, data flag, macroblock bit offset, vertical position, quantiser scale code, intra-slice flag |

The coding extension contains intra DC precision at bits 1:0, picture structure
at 3:2, then one-bit fields: top-field-first 4, frame-pred-frame-DCT 5,
concealment-motion-vectors 6, q-scale-type 7, intra-VLC-format 8, alternate-scan 9,
repeat-first-field 10, progressive-frame 11 and first-field 12. Bits 31:13 stay
in the raw extension word. The low 16 bits of `f_code` hold four four-bit
motion-code fields, from bits 15:12 down to 3:0.

A 32-byte slice descriptor inserts horizontal position at byte 16 and is
rejected by these helpers. An adapter must validate its source ABI and a
representable horizontal position; removing a word without that check is invalid.

### Picture read and word preparation

`mpeg2_va029_picture_read` checks pointers and exact length, then returns raw
fields and decoded extension bits without value validation.

`mpeg2_va029_picture_words` checks the output and width/height limits before
reading the picture. Limits must be 1–4096. It then reads the descriptor,
requires coding type 1–3 and nonzero dimensions within those limits.
Macroblock columns are width rounded up to 16 pixels. Rows are height rounded
up to 16 in a progressive context, or to a pair of 16-pixel rows otherwise.
The context's progressive flag is separate from the picture's progressive-frame
bit. Coded dimensions are the macroblock counts multiplied by 16 and must
remain at most 4096.

The resulting words have these fields; fields not listed are zero:

| Word | Bit contents |
| --- | --- |
| `fe_pps0` | alternate-scan 9; q-scale-type 8; intra DC precision 7:6; top-field-first 5; concealment motion vectors 4; frame-pred-frame-DCT 3; picture structure 2:1; intra-VLC-format 0 |
| `fe_pps1` | coding type 18:16; low 16 bits of f_code 15:0 |
| `be_pps0` | frame-pred-frame-DCT 9; q-scale-type 8; intra DC precision 7:6; alternate-scan 3 |
| `be_pps1` | low 16 bits of f_code 15:0 |
| `be_pps2` | coding type 6:4; top-field-first 3; concealment motion vectors 2; picture structure 1:0 |
| `coded_picture_size` | coded height minus one 23:12; coded width minus one 11:0 |

The first macroblock is zero. The last is `(rows - 1) << 8 | (columns - 1)`.
The 4096 bound is CPU policy; hardware sequence registers with seven-bit
macroblock counts stop at 2048 pixels per side. Accepting CPU metadata above
that boundary does not establish a device configuration.

### Quantization matrices

`mpeg2_va029_iq_update` checks input/previous/output pointers, exact 272-byte
length, then refuses either nonzero chroma load flag. Each nonzero luma load
flag replaces its matrix; zero retains the supplied previous matrix. No default
matrix is invented. Output is raster order: each raster position takes the
descriptor entry at this H.262 zigzag rank:

```text
 0  1  5  6 14 15 27 28
 2  4  7 13 16 26 29 42
 3  8 12 17 25 30 41 43
 9 11 18 24 31 40 44 53
10 19 23 32 39 45 52 54
20 22 33 38 46 51 55 60
21 34 37 47 50 56 59 61
35 36 48 49 57 58 62 63
```

The alternate-scan picture bit does not change matrix mapping. Input, previous
matrices and output may overlap.

### Slice validation and intra preparation

`mpeg2_va029_slice_check` checks pointers, exact length, then requires data flag
zero (a complete slice). It rejects zero data size, a byte span outside the
supplied data-buffer size, or a macroblock bit offset at least eight times
the slice byte size. The calculation does not overflow a 32-bit product.
It does not read slice data or validate vertical position, quantiser code or
intra-slice flag.

`mpeg2_va029_prepare_intra_slice` applies these stages in order: pointer checks,
picture-word preparation, I-picture/frame-picture scope, slice validation,
slice scope, then matrix update. Slice scope requires vertical position below
both the macroblock-row count and 128, quantiser code 1–31, and intra-slice flag
zero. Success returns all prepared fields and three shift-register setup words:
`0xb0000000`, macroblock bit offset, slice size in bytes. The bitstream address
belongs to an LLDMA record, not a fourth setup word. The caller decides whether
to retain the updated matrices.

## Verification

Run the CPU suites without firmware or a device:

```sh
python3 -I -B tools/build-userspace.py --build-target host \
  --component msvdx-proto --component mpeg2-va029
```

The tests read the versioned reference vectors and independently check bounds,
aliasing and unchanged outputs on refusal. [Vector grammar](../tests/vectors/msvdx-video/README.md#line-grammar)
defines their text format; `MANIFEST.sha256` pins the data. Running the checks
does not require a vector generator. A CPU pass establishes these contracts,
not firmware compatibility, GPU address validity, physical completion or decode quality.
