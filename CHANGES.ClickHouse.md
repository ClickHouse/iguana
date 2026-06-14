# ClickHouse fork — completed C++ implementation

This fork completes the C++ Iguana port, which upstream had left as a work in progress (the
structural compressor and several decode paths were unimplemented and the entropy round-trip was
broken). The implementation was finished by porting the reference Go implementation from
`github.com/SnellerInc/sneller`, package `ion/zion/iguana` (the portable, non-assembly code paths).

All changes are marked in the code with `NOTE (ClickHouse)` comments. The C++ API
(`iguana::encoder` / `iguana::decoder`) now round-trips for every entropy mode and for the full
structural (`iguana`) encoding.

## What was completed / fixed

* **`encoder.cpp` — `encode_iguana` (the LZ structural compressor).** Implemented from scratch from
  the Go reference (`encoder.go`): the 5-byte hash-chain match finder, backward match extension,
  repeat-offset handling, the token/offset/length stream packing (`emit`) and the per-substream
  entropy stage with a verbatim fallback. The 6 substreams and token format match the Go reference
  and the C++ portable sequence decoder. Also fixed the `encode(dst, p, n)` convenience overload,
  which built the input descriptor without setting the data pointer/size.
* **`decoder.cpp` — `decompress` / `decode_iguana` / `wild_copy`.** Removed a stray
  `IGUANA_UNIMPLEMENTED` that aborted before the command dispatch loop; implemented the overlapped
  `wild_copy`; implemented the entropy-coded substream recovery for the `decode_iguana` command;
  completed the scalar `decode_ans_nibble` command.
* **`ans32.cpp` — encoder byte order.** The encoder assembled the stream as
  `bufFwd ++ reverse(bufRev)`; the Go reference (and the decoder) expect `reverse(bufFwd) ++ bufRev`.
  Fixed by accumulating the forward half in a dedicated buffer and reversing it. Also removed an
  incorrect final-state assertion in the decoder that is absent from the Go reference (the 32-way
  interleaved decode legitimately advances some lanes one extra step) and added a length guard
  against malformed input.
* **`ans1.cpp` / `ans32.cpp` / `ans_nibble.cpp` — missing statistics serialization.** All three
  encoders reserved space for the frequency table but never wrote it, leaving the stream
  incompatible with the decoder (which recovers the table from the stream's tail). Added the
  missing `stats.serialize(dst)` call.
* **`error.h` — portability.** The `exception` base passed its message to the non-standard MSVC-only
  `std::exception(const char*)` constructor; it now stores its own message and exposes it via `what`,
  so the code builds with libstdc++/libc++.
* **`common.cpp` — robustness.** `internal::unimplemented` now throws instead of calling
  `std::abort`, so reaching an unimplemented path on corrupt input raises a recoverable error
  instead of crashing the host process.
* **`decoder.h` — allocation.** The decoder no longer eagerly allocates a 1 MiB entropy buffer per
  instance (it grows on demand).

## Not covered

The SIMD/assembly fast paths are not ported; only the portable code paths are implemented. The
`c_bindings.{h,cpp}` C API stub and the `main.cpp` command-line tool were left as upstream.
