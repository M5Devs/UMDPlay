# UMD ISO → CHD Compression Guide (Safe)

UMDPlay opens disc images through a `DiscReader` abstraction that accepts both
raw **`.iso`** images and MAME **CHD** (Compressed Hunks of Data) files, and
reads them identically. CHD is mainly useful as a **container**: it packs the
disc into a single file with an internal SHA-1 for integrity checking, and it is
**lossless** — decompressing a correctly made CHD gives back a byte-identical
ISO.

One expectation to set up front: a UMD Video ISO is mostly **already-compressed**
H.264 video and ATRAC3+ audio, so CHD usually shrinks it only **modestly**
(single-digit percent — the test disc's video head compressed to ~97 % of its
size). The win is convenience and integrity, not dramatic space savings.

Because CHD only round-trips correctly when it is created in the **right mode**,
this guide explains the UMD sector format, gives the exact command to use, and
shows how to verify and restore a CHD so you never lose an ISO.

---

## 1. UMD disc format (what you are compressing)

A UMD Video disc is a **DVD-style, single-track data** optical disc with a
**UDF 1.02** filesystem. The important properties for compression are:

| Property | Value | Notes |
|---|---|---|
| Logical sector size | **2048 bytes** | "user data" per sector, DVD-style |
| Sector layout | Mode-1 / DVD data sectors | 2048 data bytes per sector; no CD audio/subchannel data |
| Filesystem | UDF 1.02 over a dummy ISO 9660 PVD | see `docs/ARCHITECTURE.md` |
| Tracks | 1 | single data track, no audio tracks |
| Capacity | ~1.8 GB (single layer) | dual-layer UMDs exist but are rare |

A UMD **`.iso` rip is a "raw 2048" image**: it contains *only* the 2048-byte
user-data portion of each sector, back to back, with no per-sector sync/header
or ECC. This is the same convention as a DVD-Video `.iso` — **not** the
2352-byte "raw" layout used by audio CDs and many PS1/PSP *game* rips.

That single fact decides everything below: because UMD is a 2048-byte-sector
DVD-style disc, it must be compressed with **`chdman createdvd`**, never
`createcd`.

---

## 2. The correct command: `chdman createdvd`

`chdman` ships with MAME. On Debian/Ubuntu install it with
`sudo apt-get install mame-tools`; on other platforms it is bundled with MAME.

```bash
# Compress a UMD ISO. Use the DVD mode — NOT the CD mode.
chdman createdvd -i movie.iso -o movie.chd
```

That is the entire workflow. `createdvd` defaults to a 4096-byte hunk with
`lzma,zlib,huff,flac` compression, which is exactly what UMDPlay expects.

The resulting CHD is a **v5 "DVD" CHD** — confirm the two fields that matter:

```console
$ chdman info -i movie.chd
File Version: 5
Logical size: 67,108,864 bytes
Hunk Size:    4,096 bytes          # <-- DVD-style hunk
Unit Size:    2,048 bytes          # <-- DVD-style sector == UMD sector ✅
Compression:  lzma (LZMA), zlib (Deflate), huff (Huffman), flac (FLAC)
Metadata:     Tag='DVD '  Index=0  Length=1 bytes
```

`Unit Size: 2,048` and the `DVD ` metadata tag are the two things to check. If
you see a different unit size, you used the wrong mode (see §3).

### Optional flags

| Flag | Purpose |
|---|---|
| `-f` | overwrite an existing output file |
| `-hs <bytes>` | hunk size (leave at the default unless you have a reason) |
| `-c <codecs>` | compression codecs, e.g. `-c lzma` for smaller / slower, `-c zstd` for faster |
| `-np <n>` | limit CPU threads used during compression |
| `-isb <offset>` / `-ib <bytes>` | compress only a byte range (rarely needed) |

---

## 3. ⚠️ Do NOT use `createcd` (2352-byte CD mode)

The single most common way to break a UMD rip is to reach for `createcd`,
which is correct for CD-based systems (PS1, Sega CD, PC Engine CD) but **wrong
for UMD**. `createcd` assumes a **2352-byte CD sector** and wraps the data in a
CD image with 2448-byte units (2352 data + 96 subchannel) and a `CHT2` track
table:

```console
$ chdman createcd -i movie.iso -o movie.chd     # ❌ WRONG for UMD
$ chdman info -i movie.chd
Logical size: 80,216,064 bytes     # padded up from 67,108,864
Hunk Size:    19,584 bytes         # CD-style hunk
Unit Size:    2,448 bytes          # ❌ CD unit, not 2048
Metadata:     Tag='CHT2' ... TRACK:1 TYPE:MODE1 SUBTYPE:NONE
```

Because the unit size is **2448** instead of **2048**, every sector boundary is
shifted: the image is treated as a CD, sector headers/subchannel padding are
interleaved, and the byte at "sector N" no longer matches the UMD's sector N.
UMDPlay reads *logical byte offsets* from the CHD, so its UDF parser lands in
the wrong place and fails cleanly:

```console
$ umdplay movie.chd --probe
[IsoReader] Opened: movie.chd  [chd backend, 80216064 bytes]
[IsoReader] No .MPS/.PMF files found in STREAM/ directory
[-] Failed to read UMD Video filesystem!        # ← the createcd symptom
```

> **Rule of thumb:** if `chdman info` reports `Unit Size: 2,448`, you compressed
> it with `createcd` — redo it with `createdvd`.

`createdvd` also *fixes* an image that was already padded to a CD logical size:
as long as your `.iso` is a raw 2048-byte-sector image, `createdvd` reads it
correctly regardless of its total length.

---

## 4. Verify a CHD

After compressing (or before deleting any original), verify the CHD's internal
SHA-1 so you know the compressed copy is intact:

```bash
chdman verify -i movie.chd
```

A healthy file prints:

```console
Raw SHA1 verification successful!
Overall SHA1 verification successful!
```

`chdman verify` re-reads and decompresses the whole file, so it is also a good
sanity check that the CHD is not truncated or corrupt. (Add `-f` only if you
want it to repair a mismatched SHA-1, which is rare.)

---

## 5. Restore a CHD back to ISO

A CHD is **lossless** for a `createdvd` image — decompressing gives back a
byte-identical `.iso`. To restore:

```bash
chdman extractdvd -i movie.chd -o movie.iso
```

Then confirm the round-trip is exact (this is what UMDPlay's own CHD test does):

```bash
cmp original.iso restored.iso && echo "byte-identical ✅"
# or: sha256sum original.iso restored.iso
```

Verified in practice on the reference UMD Video disc: a `createdvd` CHD
extracted back with `extractdvd` matched the original ISO **byte-for-byte**
(identical MD5), and UMDPlay produced **byte-identical MP4 clips** from the ISO
and its CHD twin.

---

## 6. Using CHD files with UMDPlay

Nothing special is required — pass the `.chd` anywhere you would pass an `.iso`:

```bash
./build/bin/umdplay movie.chd                 # play
./build/bin/umdplay movie.chd --probe         # inspect streams
./build/bin/umdplay movie.chd --export-clip clip.mp4 --start 3552 --clip-seconds 12
```

The Browse System strip also lists `*.chd` files alongside `*.iso`. UMDPlay
picks the backend from the extension: `.iso` → direct read, `.chd` → libchdr
with a small decompressed-hunk cache. See `docs/ARCHITECTURE.md` for the
`DiscReader` design.

---

## 7. Quick reference

```bash
# 1. Compress (DVD mode — the only correct mode for UMD)
chdman createdvd -i movie.iso -o movie.chd

# 2. Verify the compressed copy
chdman verify -i movie.chd

# 3. (Optional) confirm it is DVD-style: Unit Size must be 2048
chdman info -i movie.chd | grep -E "Unit Size|Hunk Size|Tag"

# 4. Restore at any time (lossless)
chdman extractdvd -i movie.chd -o movie.iso
cmp movie.iso restored.iso   # identical

# ❌ Never do this for a UMD disc:
chdman createcd  -i movie.iso -o movie.chd
```
