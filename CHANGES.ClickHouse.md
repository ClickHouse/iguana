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

## AVX-512 acceleration and dynamic dispatch

* **`ans32_avx512_core.h` / `ans32_avx512.cpp` — AVX-512 ANS32 decoder.** The 32-way interleaved
  rANS decoder is ported to AVX-512 intrinsics from the Go reference assembly
  (`ans32DecompressAVX512Generic`), using only AVX-512 F + BW + VL (`VPGATHERDD` for the table
  lookup, `VPEXPANDD` for renormalization; no VBMI2). The kernel is a freestanding header so the
  exact shipping code can be compiled into a standalone validation harness.
* **`common.{h,cpp}`, `ans32.{h,cpp}` — dynamic dispatch.** `cpu_has_avx512` reports the F+BW+VL+DQ
  feature set on x86-64; `ans32::decoder::at_process_start` swaps the decode function pointer to the
  AVX-512 kernel when available. On every other host (e.g. AArch64) the portable kernel is used.
* **`output_stream.h` — `claim`.** Returns a writable tail region so the vectorized decoder can write
  a contiguous block instead of byte-at-a-time.

> Validation note: the AVX-512 kernel was developed on an AArch64 host, where it cannot be executed
> (qemu-TCG does not implement AVX-512). It is byte-exact-verifiable against the portable decoder
> with the fixture harness checked in alongside the ClickHouse integration, and is exercised by the
> ClickHouse round-trip test on x86 AVX-512 CI. Run-validate on AVX-512 hardware before relying on it.

## AArch64 NEON acceleration

* **`decoder_neon.cpp` — NEON structural decoder.** A NEON implementation of the Iguana sequence
  decoder: matches are copied with 16-byte NEON loads/stores when the match distance is >= 16
  (the encoder's end-of-buffer clamp keeps such copies in bounds); shorter distances replicate
  byte-wise. Selected at process start on AArch64 (`decoder::at_process_start`). NEON is part of the
  AArch64 baseline, so no runtime feature check is needed.
* **`output_stream.cpp` — bulk literal copy.** `append(p, n)` is now a bulk memcpy (was
  byte-at-a-time), which speeds up literal runs on every architecture.

Unlike the AVX-512 kernel, the NEON kernel was developed, **run, byte-exact-validated and
benchmarked natively** on an AArch64 (Graviton, 128-bit SVE2) host. Measured decode throughput on
4 MiB blocks: highly repetitive 0.44 -> 6.8 GB/s (15x), log-line data 0.41 -> 2.6 GB/s (6.4x),
long-match data 0.44 -> 22.7 GB/s (51x). Entropy-bound data (mostly literals + rANS) is unchanged,
as the structural copy is not its bottleneck there.

`IGUANA_DISABLE_DISPATCH` forces the portable kernels (used for the A/B benchmark above and for
debugging).

## Not covered

Still portable-only: the AVX-512 **ANS32 encoder**, the **structural (Iguana) decoder**, and the
**match finder** (the remaining accelerated kernels in the Go reference). The `c_bindings.{h,cpp}` C
API stub and the `main.cpp` command-line tool were left as upstream.
