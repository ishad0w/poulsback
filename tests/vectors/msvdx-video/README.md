<!-- SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors -->
<!-- SPDX-License-Identifier: MIT -->
# msvdx-video black-box vectors

Data only. Each vector records one call's inputs and the observable result of the
reference implementation: the status class and the output bytes or fields. An
implementation passes when it gives the same result for every line. The rules behind
the vectors are the local [video protocol contract](../../../docs/video-protocol.md).

`MANIFEST.sha256` pins the versioned reference files. The CPU checks read these
files directly; neither checking nor consuming them requires a generator.

## Files

| File | Vectors | Operation |
| --- | ---: | --- |
| `message-size.txt` | 256 | envelope size for every ID |
| `message-encode.txt` | 727 | fields → envelope bytes |
| `message-decode.txt` | 1,013 | envelope bytes → fields |
| `stream-validate.txt` | 116 | check a concatenation of envelopes |
| `stream-patch.txt` | 63 | write PTD and fence into every render envelope |
| `ring-grid.txt` | 10,000 | one render envelope at every read/write index pair |
| `ring-plan.txt` | 512 | complete ring plans and every ring refusal |
| `ring-sequence.txt` | 92 | chained submissions with the ring contents after each |
| `mpeg2-picture.txt` | 98 | 24-byte picture descriptor → fields |
| `mpeg2-normalize.txt` | 507 | picture descriptor → register words and sizes |
| `mpeg2-iq.txt` | 84 | 272-byte IQ descriptor applied to a previous state |
| `mpeg2-slice.txt` | 122 | 28-byte slice descriptor checked against a data size |
| `mpeg2-prepare.txt` | 46 | picture + IQ + slice → one prepared record |
| **total** | **13,636** | |

## Line grammar

```text
line    := comment | directive | vector
comment := "#" text
directive := "@packet" SP bytes      (ring-grid.txt only: the envelope for later grid lines)
vector  := op { SP key "=" value } SP "=>" { SP key "=" value }
value   := integer | bytes | word-list | token
integer := decimal digits | "0x" hex digits
bytes   := "x:" { two lowercase hex digits }     (may be empty: "x:")
word-list := integer { "," integer }
```

Keys left of `=>` are inputs, keys right of it are results. The first result is
always `status=`. Integers are unsigned. Byte strings are in memory order; the
length of an input byte string is the length passed to the call.

Common input tokens:

- `dir=host` (host to MTX), `dir=mtx` (MTX to host), `dir=bad` (any other
  direction value).
- `null=<name>`: that pointer argument is absent; the byte string printed for it
  is not passed.

Status classes: `ok`, `argument`, `length`, `unsupported`, `direction`, `range`
(message and MPEG-2 operations), `limit` (stream budgets), `index`, `packet`,
`full` (ring). A refused call writes nothing: output buffers and objects are
unchanged.

## Operations

`size id` → `bytes` (0 when the ID has no layout).

`encode id dir cap [null] <fields>` → `status [out]`. `cap` is the output
capacity. On success exactly `out` is written (its length is the envelope size);
bytes after it are untouched. Field keys by ID:

| ID | Fields |
| --- | --- |
| `0x81` render | `bsize` (low 12 bits of the halfword at byte 2), `bsize_hi` (bits 15:12 of it, kept in place), `ptd`, `lldma`, `ctx`, `fence`, `mode`, `mb_first`, `mb_last`, `flags` |
| `0xc0` completed | `hdr` (halfword at byte 2), `fence`, `flags` |
| `0xc2` deblock required | `hdr`, `ctx` |
| `0xc5` failed, `0xc7` panic | `flags16`, `fence`, `irq` |

`decode dir [null] in` → `status [id <fields>]`.

`validate dir max_bytes max_msgs [null] in` → `status messages renders`
(both counts 0 when refused).

`patch max_bytes max_msgs ptd fence invalidate [null] in` → `status messages renders out`
(`out` equals `in` when refused).

`grid r w` (with the preceding `@packet`) → `status [pad count final]`.

`plan r w [null] in` → `status [pad count final words]`; `words` is always 15
values, unused ones 0.

`step seq n r w in` → `status [pad count final words] ring`. Within one `seq`, lines
are consecutive submissions. The ring starts as 100 words of `0xeeeeeeee`; on `ok`
the plan's first `count` words are stored from index `w`, wrapping at 100. `ring`
lists all 100 words (8 hex digits each, no prefix) after that step. The next step's
`r` and `w` are given explicitly.

`picture [null] in` → `status [picture fields]`.

`normalize progressive max_width max_height [null] in` →
`status [picture fields] [word fields]`.

- Picture fields: `hsize vsize fwd bwd type fcode ext dc_precision structure tff fpfd cmv qst ivlc alt rff progressive first_field`.
- Word fields: `fe_pps0 fe_pps1 be_pps0 be_pps1 be_pps2 coded_width coded_height mb_width mb_height coded_size mb_first mb_last`.

`iq [null] prev_intra prev_non_intra in` → `status [intra non_intra]` (64 bytes
each, raster order).

`slice data [null] in` → `status [size offset flag mb_bit_offset vertical qscale intra_slice]`.

`prepare case progressive max_width max_height data [null] picture iq slice prev_intra prev_non_intra`
→ `status [picture fields] [word fields] [intra non_intra] [slice fields] [sr_setup]`.
`case` is a label only; `sr_setup` is three words.

## Provenance of inputs

Synthetic inputs come from fixed-seed generators. Device inputs cover EMGD 1.8
and PSB MPEG-2 I/P/B pictures, the libva 0.29 `test_12` fixture values,
and this repository's MIT `mpeg2_dc_fixture` stream. Public 32-byte slice descriptors
from the captures appear in their 28-byte form, with the zero horizontal-position
word removed.
