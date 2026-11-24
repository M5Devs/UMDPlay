# h264dec

Standalone H.264 decoder and SPS/slice-header parser used by UMDPlay's video
path (`umdplay::VideoDecoderBackend`, see `src/h264_video_decoder.cpp`).

Vendored from [xesf/umd-player](https://github.com/xesf/umd-player)
(`src/core/h264dec.c`, `h264dec_slice.c`, `h264dec_recon.c`,
`h264dec_deblock.c`, `h264dec_int.h`, `h264dec.h`, `h264_cabac_tables.*`,
`h264.c`, `h264.h`), MIT licensed — see `LICENSE`.

Scope: Main profile, 8-bit 4:2:0, CABAC, frame and field pictures (PAFF), as
found on UMD Video. It reports what it does not support (CAVLC, MBAFF,
long-term references, memory management commands, temporal direct) rather than
guessing, and its output is bit-exact with the H.264 decoding process.

Compiled as C with warnings silenced (third-party code never fails the
project's `-Werror`), but with the sanitizers enabled, so a memory bug in it
still fails a sanitizer run.

Local changes, all found by `tests/test_video_decoder.cpp` under
ASan/UBSan and none of them changing the decoded output:

* `h264dec_recon.c` — `h264dec_intra16x16` / `h264dec_intra_chroma` read the
  row above (`dst - stride`) unconditionally.  A valid stream never selects
  vertical/plane prediction without that neighbour, but a truncated or corrupt
  one can, and the read then lands before the plane (ASan: heap-buffer-overflow).
  Both now fall back to DC prediction when the required neighbour is missing,
  and the chroma DC path only accumulates available neighbours.
* `h264dec_slice.c`, `h264dec_deblock.c` — shifts of possibly negative values
  (`x << n`) rewritten as `x * (1 << n)`; identical results, no UBSan noise.
* `h264.c` — `get_ue()` saturates at 32 leading zero bits instead of shifting
  by the type width.
