# AMPR asset container (AMPRPAK4 / AMPRDAT3 / AMPRCRC1 / AMPRCFG1)

AnyPS5 reads this container in `core/libs/prx/libc/src/AmprContainer.cpp`. When a guest `/app0`
path is missing on disk, the path resolver looks it up in the container. A packed file is
extracted once into `ampr_cache/` in the run directory, and the guest is given that host path.
Loose entries are always served from the real filesystem.

Written by the third-party `fakelib/libSceAmpr.sprx` ("ampr_emu", (c) Drakmor, v0.4.2.1).
The same 633094-byte library ships in both Zorro (PPSA06509) and Spirit of the Island
(PPSA13934). Sources for this spec:
- the index validator at VA 0x14219..0x15003 of the unwrapped library;
- the chunk-descriptor builder at 0x13a80..0x13c3b;
- the AMPRDAT3 header check at 0x2a28f;
- the `apr.pack.*` lines in Spirit's `ampr_emu.log`;
- whole-archive tests: every extent in both games passes its CRC, and every packed file
  that also exists loose on disk matches byte for byte.

All integers are little-endian. `[k]` marks a claim confirmed from code or by exhaustive
test. `(?)` marks an uncertain claim.

## Files in the game directory (/app0)

| File | Magic | Role |
|---|---|---|
| `ampr_assets.index` | `AMPRPAK4` | The index: files, extents, pack volumes and a string table. |
| `ampr_assets-<lane>-laneNN-volNN-<packId>.pak` | `AMPRDAT3` | Data volumes. `<lane>` is one of default, bulk, metadata or stream. The trailing number is the pack index. |
| `ampr_assets.index.crc` | `AMPRCRC1` | One CRC-32 per extent, computed over the decoded bytes. Optional, for integrity only. The library does not read it (the library has no "AMPRCRC" string). |
| `ampr_assets.index.runtime` | `AMPRCFG1` | Cache and worker tuning. Not needed for reading. |
| `ampr_emu.index` (`AMPRIDX3`), `ampr_emu.log`, `ampr_commands.bin` | | Emulator-private artifacts: its own lookup cache, its log and its command recording. They are not part of the container and are not needed. |

A single 16-byte GUID ties the set together. It appears in the index header, in every DAT3
header, and in the CRC1 and CFG1 headers `[k]`.

All CRCs in this format are standard CRC-32: zlib `crc32`, reflected polynomial 0xEDB88320,
initial value 0xFFFFFFFF, final XOR. A header CRC is computed over the header with its own
CRC field set to zero `[k]`.

## 1. `ampr_assets.index` (AMPRPAK4)

### Header (0x80 bytes)

| Off | Type | Value / meaning |
|---|---|---|
| 0x00 | char[8] | `AMPRPAK4` [k] |
| 0x08 | u32 | version = 4 [k] |
| 0x0C | u32 | header size = 0x80 [k] |
| 0x10 | u32 | 0 (must be 0) [k] |
| 0x14 | u32 | 0x01020304, the byte-order marker [k] |
| 0x18 | u8[16] | container GUID [k] |
| 0x28 | u64 | file_count. The library caps it at 2,000,000 [k]. |
| 0x30 | u64 | extent_count. The library caps it at 16,000,000 [k]. |
| 0x38 | u32 | pack_count. The library requires fewer than 0x401 (and at most 0xFFFF) [k]. |
| 0x3C | u32 | file entry size = 0x30 [k] |
| 0x40 | u32 | extent entry size = 0x0C [k] |
| 0x44 | u32 | pack entry size = 0x20 [k] |
| 0x48 | u64 | files table offset = 0x80 [k] |
| 0x50 | u64 | extents table offset = 0x80 + 0x30*file_count [k] |
| 0x58 | u64 | packs table offset = extents offset + 0x0C*extent_count [k] |
| 0x60 | u64 | string table offset = packs offset + 0x20*pack_count [k] |
| 0x68 | u64 | string table length. The string table offset plus this length equals the index file size [k]. |
| 0x70 | u32 | CRC-32 of bytes [0x80, EOF) [k] |
| 0x74 | u32 | CRC-32 of header bytes [0, 0x80) with 0x74..0x77 zeroed [k] |
| 0x78 | u64 | 0 (must be 0) [k] |

The tables are packed back to back in that order, with no padding `[k]`.

### Pack entry (0x20 bytes, `pack_count` entries, index = pack id)

| Off | Type | Meaning |
|---|---|---|
| 0x00 | u64 | payload_size. It equals the .pak file size minus the payload start [k]. |
| 0x08 | u64 | .pak file size; this is also payloadEnd [k] |
| 0x10 | u32 | name offset into the string table [k] |
| 0x14 | u32 | name length. It excludes a NUL terminator, which must be present, and the name must contain no NUL bytes [k]. |
| 0x18 | u32 | kind. (kind & ~1) must equal 2, so kind is 2 or 3; every observed pack uses 2. It must equal the kind in the DAT3 header. What 3 means is unknown (?). |
| 0x1C | u32 | page/alignment size: a power of two in 0x1000..0x100000 and a multiple of 64. All observed packs use 0x10000 [k]. |

- `payload_begin` = file_size − payload_size. It is 0x10000 in both games.
- `payload_begin` must be at least 0x40.
- Both file_size and payload_begin must be multiples of the alignment `[k]`.
- The name is a bare file name relative to /app0, for example
  `ampr_assets-bulk-lane01-vol00-000.pak`. It must not start with `/` or `\` and must not
  contain `.` or `..` components `[k]`.

### File entry (0x30 bytes)

| Off | Type | Meaning |
|---|---|---|
| 0x00 | u64 | path hash (see below) [k] |
| 0x08 | u64 | file size in bytes, the decoded size [k] |
| 0x10 | u64 | mtime, Unix seconds. For example, 0x6a5217e1 is 2026-07-11 (?, plausible and matches the dump dates). |
| 0x18 | u32 | first_extent: index into the extent table [k] |
| 0x1C | u32 | extent_count, which is the chunk count [k] |
| 0x20 | u32 | path offset into the string table [k] |
| 0x24 | u32 | path length. At least 7, NUL-terminated, no internal NUL [k]. |
| 0x28 | u32 | flags, at most 0x1F [k]. See below. |
| 0x2C | u8 | chunk_log2. Must be 14..20 (16 KiB..1 MiB) when packed, 0 when loose [k]. Observed values are 16, 17 and 18. |
| 0x2D | u8 | lane class, matching the pack lane of the file's extents: 0 bulk, 1 default, 2 metadata, 3 stream (observed; the library only requires 0 for loose entries). |
| 0x2E | u16 | 0 (must be 0) [k] |

Flags:

| Bit | Mask | Meaning |
|---|---|---|
| 0 | 0x01 | Packed: the data is in the .pak volumes. When clear, the entry is loose: first, count, flags, chunk_log2 and lane class are all 0 `[k]`. |
| 1 | 0x02 | No compression: every extent must use codec 0 `[k]`. Seen on the stream-lane .webm. |
| 2 | 0x04 | Stream (?). It must equal extent bit 23 of every chunk `[k]`, and bit 2 and bit 4 may not both be set `[k]`. Seen on the stream-lane .webm. When this bit is clear, the page-flag rules in the extent section apply `[k]`. |
| 3 | 0x08 | Unknown (?). Set on large Unity files (globalgamemanagers, *.assets, *.resS). Not needed for reading. |
| 4 | 0x10 | Unknown (?). Set on small metadata-lane files (level0, sharedassetsN.assets and similar). Not needed for reading. |

The path must:
- start with `/`;
- have `/` at byte 5, with bytes 1..4 equal to `app0` case-insensitively [k];
- contain no `\`, no NUL, no empty components, and no `.` or `..` components [k].

Stored paths keep their original case, for example `/app0/Media/Resources/unity default resources`.

The path hash is FNV-1a 64 over the whole path, including the leading `/` `[k]`. Each byte
is normalized before hashing: `\` becomes `/` and ASCII `A-Z` becomes `a-z`. The standard
constants apply (offset basis 0xcbf29ce484222325, prime 0x100000001b3). A result of 0 is
stored as 1. Lookups are therefore case-insensitive. The emulator uses the hash for lookup
and resolves collisions by comparing paths. The file table is not sorted by hash. It looks
roughly path-ordered, so a reader should build its own map.

For a packed file:

- `extent_count == ceil(size / 2^chunk_log2)` `[k]`.
- Chunk i covers decoded bytes `[i<<log2, min((i+1)<<log2, size))`. Only the last chunk may
  be shorter than the chunk size `[k]`.
- The decoded chunk sizes must add up to `size` `[k]`.
- A file's extents are the consecutive table entries `first_extent .. first_extent+count-1`,
  in chunk order `[k]`.
- Files do not share extent-table entries. The extent table is covered contiguously, in
  file-table order, with no gaps. This holds in both games; it is not required.
- Physical placement is free. Chunk order in a volume is not always ascending (27 of 130
  Zorro files are not ascending). All chunks of one file happen to be in one pack, but the
  format does not require that.
- Identical chunks are deduplicated: different extents may point at the same
  pack/offset/length (4409 in Zorro, 1198 in Spirit). Partial overlaps never occur.

A loose file (flags bit 0 clear) has no bytes in the container. The emulator serves it from
the real filesystem at the same path (the log reports "loose=55"). The index size of a loose
entry is only a hint and can be stale: for Zorro's `/app0/fakelib/libSceAmpr.sprx` the index
says 254326 bytes while the disk copy is 633094. Some loose entries may be missing on disk:
15 in Spirit, all under `/app0/decrypted/...`. A reader must use the real file for loose
entries.

### Extent entry (0x0C bytes)

```
u64 loc   : bits 0..47  = byte offset inside the .pak (absolute file offset, not payload-relative)
            bits 48..63 = pack id (< pack_count)
u32 word  : bits 0..19  = stored_len - 1        (stored_len = bytes to read from the pak)
            bits 20..21 = codec: 0 = stored (raw), 1 = LZ4 block; 2 and 3 are rejected [k]
            bits 22..25 = 4 chunk flags (below)
            bits 26..31 = 0 (word must be <= 0x3FFFFFFF) [k]
```

Constraints the library checks `[k]`:

- `loc` is a multiple of 64.
- The extent lies inside the payload: `payload_begin <= offset` and
  `offset + stored_len <= pack file size`.
- `stored_len - 1 < chunk size`.
- For codec 0: `stored_len` equals the decoded chunk size.
- For codec 1: the file's flags bit 1 must be clear.

Chunk flags, with a = the pack's alignment (0x10000):

| Bit | Meaning |
|---|---|
| 22 | Unknown (?). Set on about 2% of chunks, both LZ4 and stored, and never checked by the validator. It is not needed for decoding: all such chunks decode and CRC-check with the normal rules. It might be a decode or scheduling hint. The descriptor builder copies it into byte +0x23 of the chunk descriptor. |
| 23 | Mirror of file flags bit 2 (stream) [k]. |
| 24 | The stored range does not cross an a-boundary: `(offset ^ (offset + stored_len - 1)) & ~(a-1) == 0` [k]. |
| 25 | The stored range starts on an a-boundary. Such a chunk is either longer than a or also has bit 24 set [k]. |

For non-stream files, extents shorter than a must have bit 24, and extents of length a or
more must have bit 25 `[k]`. These are placement and I/O hints only; a reader can ignore them.

The "type 1/2/3" in the earlier notes is bits 24 and 25. The "constant 8" is bit 20 (the
LZ4 codec) seen through a mis-sliced field. Bits 17..19 belong to the length field.

The library's chunk descriptor, built at 0x13a80, has this layout:

```
{u32 ?, u32 ?, u32 ?, u32 ?, u64 offset, u32 stored_len, u32 chunk_size, u16 pack, u8 codec, u8 flags}
```

`apr.pack.open ... firstPack=%u firstCodec=%u` prints the pack and codec of chunk 0.

## 2. Reading a file by path

1. Normalize the path to `/app0/...` (lowercase it, turn `\` into `/`) and look it up. Use
   the hash, the path, or both.
2. If flags bit 0 is clear, open the real file `<game dir>/<path without /app0/>`.
3. Otherwise, set `cs = 1 << chunk_log2`. For each i in 0..count-1:
   1. Read extent `first + i`.
   2. Compute `n = min(cs, size - i*cs)`.
   3. Read `stored_len` bytes at `offset` in pack `loc>>48`.
   4. Decode them:
      - codec 0: the bytes are the data, and `stored_len == n`.
      - codec 1: decompress a standard LZ4 *block* (raw block format, no frame, no size
        prefix) of exactly `stored_len` input bytes into exactly `n` output bytes. Python's
        `lz4.block.decompress(src, uncompressed_size=n)` works. The earlier "non-standard
        LZ4" finding was an off-by-one: the length field stores `stored_len - 1`.
   5. Append the result.
4. The concatenation is the file (length = size). Random access to byte X uses chunk `X >> chunk_log2`.
5. Optional check: `crc32(decoded chunk) == crc[first+i]` from the .crc file.

## 3. `.pak` volume (AMPRDAT3)

Each volume has a 0x40-byte header, zero padding up to `payload_begin` (0x10000), and then
the payload. Extents are addressed by absolute file offset.

| Off | Type | Meaning |
|---|---|---|
| 0x00 | char[8] | `AMPRDAT3` [k] |
| 0x08 | u32 | version = 3 [k] |
| 0x0C | u32 | header size = 0x40 [k] |
| 0x10 | u32 | pack id. It must equal this volume's index in the pack table [k]. |
| 0x14 | u32 | kind, at most 3. It must equal the pack entry +0x18 [k]. |
| 0x18 | u8[16] | container GUID. It must equal the index GUID [k]. |
| 0x28 | u64 | payload_begin (0x10000). It must be at least 0x40 and aligned [k]. |
| 0x30 | u64 | payload_size. It must equal the pack entry +0x00, and payload_begin + payload_size must equal the file size [k]. |
| 0x38 | u32 | CRC-32 of the 0x40-byte header with this field zeroed [k] |
| 0x3C | u32 | 0 [k] |

The payload itself carries no checksum; the .crc file covers the decoded chunks. Gaps
between extents are zero padding, because extent offsets are multiples of 64.

## 4. `ampr_assets.index.crc` (AMPRCRC1)

| Off | Type | Meaning |
|---|---|---|
| 0x00 | char[8] | `AMPRCRC1` |
| 0x08 | u32 | version = 1 |
| 0x0C | u32 | header size = 0x30 |
| 0x10 | u8[16] | GUID |
| 0x20 | u64 | count, which equals extent_count |
| 0x28 | u32 | CRC-32 of the body [0x30, EOF) [k] |
| 0x2C | u32 | CRC-32 of the header with 0x2C..0x2F zeroed [k] |
| 0x30 | u32[count] | CRC-32 of each extent's decoded chunk bytes [k] |

A deduplicated extent has the same CRC at each table index that refers to it.

## 5. `ampr_assets.index.runtime` (AMPRCFG1, 64 bytes; tuning only)

| Off | Type | Meaning |
|---|---|---|
| 0x00 | char[8] | `AMPRCFG1` |
| 0x08 | u32 | version = 1 |
| 0x0C | u32 | size = 0x40 |
| 0x10 | u8[16] | GUID |
| 0x20 | u64 | decoded-cache target in bytes: 0x08000000 in Spirit, 0x10000000 in Zorro |
| 0x28 | u64 | physical-cache target in bytes: 0x02000000 |
| 0x30 | u32 | worker count: 4 |
| 0x34 | u32 | reserve: 0 |
| 0x38 | u32 | CRC-32 of the 64 bytes with this field zeroed [k] |
| 0x3C | u32 | 0 |

The Spirit log matches the 0x20..0x34 fields:
`apr.pack.profile.request decoded=134217728 physical=33554432 workers=4 reserve=0`.
The library only uses these values to size its caches. An AnyPS5 reader can ignore this file.

## 6. Observed numbers

| | Zorro | Spirit |
|---|---|---|
| Files | 715: 130 packed, 585 loose | 155: 100 packed, 55 loose |
| Extents | 210761 | 34643 |
| Packs | 7 (bulk lanes 0..2, default, metadata lanes 0..1, stream) | 6 (no stream) |
| Chunk sizes | 64K ×118, 128K ×9, 256K ×3 | 64K ×81, 128K ×12, 256K ×7 |

Codec counts over all extents in both games: 0 stored, 1 LZ4. No other codec occurs.

## 7. Open questions (none block reading)

- The meaning of extent bit 22.
- The meaning of file flags bits 3 (0x08) and 4 (0x10).
- The meaning of pack kind 3.
- The purpose of lane class beyond matching the pack lane. The emulator's I/O class and
  priority may derive from it: the log's `class=` value does not simply equal +0x2D.
- The mtime interpretation of +0x10 is inferred, not checked in code.
