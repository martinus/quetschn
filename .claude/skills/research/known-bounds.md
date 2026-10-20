# Known bounds

Part of the `research` skill (SKILL.md). Bounds known before, with dates; stale once tables, format
or parse change (`.claude/rules/docs.md`). Check the entry in `docs/explored-designs.md` before
quoting.

- ANS instead of Huffman: at most 0.3% (6 Oct, "How tight the bits are").
- Tables trained on the dump itself: 0.7% on the phone, 1.5% to 2.5% on desktop dumps (7 Oct).
- Another page of the same dump as reference: `zstd` 3 34% smaller; exact copies of an earlier page
  11.6% of pages; 1024 references from another boot 4% to 7% net (17 to 19 Oct, "Reference pages per
  device on the Mi 9T").
- Bytes of a phone page by part, `seqlz-2`'s parse, p3 (19 Oct, "Where the bytes of a phone page go"):
  literals 69%, tokens 18%, offset bits 11%; LZMA with a 4 KiB dictionary 14% under `zstd` 3 per page,
  `zstd` 19 9%.
- Decode, p3, userspace loop with the input prefetched, kernel flags, µs per page, A55 / A76 (19 Oct,
  "The decoder with the page prefetched on the Mi 9T"): seqlz-1 7.35 / 2.84, `lz4` 6.60 / 2.63,
  seqlz's parse in `lz4`'s byte format 5.95 / 2.44. The entropy coding of tokens and offsets buys 266
  bytes per page for 1.4 µs on the A55, 0.4 on the A76. Trained byte tokens (one-byte index into 255
  triples) save 179 of them at 1.0 µs on the A55: no better.
- Which core runs zram (zramlog records of 17 Oct, Mi 9T, scripted sessions; "Write time on the Mi 9T
  by part"): 96% of the writes on the A76 cores, almost all from `kswapd`, and 65% to 69% of the
  reads. A55 write times count little there; weigh a per-core result by this mix before chasing an
  A55-only gap. The Pixel 9a is not known.
- Write time, harness, p3, µs per page, A55 / A76 (19 Oct, same entry): `lz4` 16.9 / 7.6, seqlz-1 23.4
  / 8.2, seqlz-2 27.2 / 8.5, seqlz-3 38.6 / 13.7. Level 1's matcher alone is `lz4`'s whole compress on
  the A55 and faster on the A76; the gap is the encoder's arithmetic per sequence (about 65
  instructions).
- File data (Android 17 emulator image, 19 Oct, "File data of an Android 17 image"): seqlz-3 0.9
  points behind `zstd` 3 on 4 KiB blocks at 2.1 times `lz4`'s decode cycles on the A55; coded literals
  are most of seqlz's decode time there.
