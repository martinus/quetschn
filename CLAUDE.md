# quetschn

## The goal

Get as close as possible to `lz4`'s speed and to `zstd`'s compression, without more memory than `lz4`
needs and without recompression. It might be unreachable, so every design is judged by how much closer
it gets, not by whether it arrives.

- Speed and compression are the two axes of the score in `PLAN.md` §1.1: mean time per page written,
  and zsmalloc memory per stored page. `zstd` means zram's default, level 3.
- Memory: per-CPU workspace at most `lz4`'s 16416 bytes (C5).
- No recompression: the page stays in the format it was written in. zram's recompression of idle pages
  is not part of any design, `PLAN.md` §1.1 says why.

## Guards, not gates

C2 and C3, the p99 of reads and writes against `lz4`, are guards. A design is not dropped because its
p99 misses the bar, and none is kept just because it meets it. A p99 that gets clearly worse is a reason
to find out which pages cause it. C5 and C6 stay hard limits.
