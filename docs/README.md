# Documentation

| document | what it is | read it if you want to |
| --- | --- | --- |
| [seqlz.md](seqlz.md) | how seqlz works, and why each choice was made, with the numbers | understand the codec. It explains every term where it is first used, no compressor experience needed |
| [format.md](format.md) | the bytes of a compressed page, and which pages are valid | write a decoder, or check one |
| [measuring.md](measuring.md) | the commands: corpus, clock, kernel VM, userspace harness, phone | reproduce a number, or measure a change |
| [plan.md](plan.md) | the goal, the score, what zram needs from a codec, the phases, the risks, and where the project stands | know why the project does what it does |
| [explored-designs.md](explored-designs.md) | every design that was measured, kept or not, with its numbers, and an index by topic | try something new without measuring it twice |
| [seqlz-bit-by-bit.html](seqlz-bit-by-bit.html) | three real pages decoded one step at a time, every bit coloured by what it means | watch the decoder |
| [seqlz-compressed.html](seqlz-compressed.html) | the same pages compressed, from the first position the matcher tries to the header | watch the compressor |

GitHub shows the two HTML pages only as source. Download one and open it in a browser; each is one file
with everything in it. [`tools/seqlz-viz/`](../tools/seqlz-viz) builds them.

The charts in [`plots/`](plots) are made by the scripts in [`tools/`](../tools/README.md) from the logs
of the measurements.

**Where to start.** For the codec: [seqlz.md](seqlz.md), the sections up to "The literals", then the
two HTML pages. For the project: [plan.md §1.1](plan.md#11-the-score-memory-against-time-not-bars)
and [§9](plan.md#9-where-the-project-stands-and-the-next-actions). Before changing anything:
[explored-designs.md](explored-designs.md) and [CONTRIBUTING.md](../CONTRIBUTING.md).
