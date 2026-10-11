# Documentation

| document | what it is | read it if you want to |
| --- | --- | --- |
| [seqlz.md](seqlz.md) | how seqlz works, and why each choice was made, with the numbers | understand the codec. It explains every term where it is first used, no compressor experience needed |
| [seqlz, line by line](https://martin.ankerl.com/quetschn/) | a book that explains every line of `src/`, with animations and step-throughs of the real code; built from [`book/`](book) by CI | understand the code itself, line by line |
| [format.md](format.md) | the bytes of a compressed page, and which pages are valid | write a decoder, or check one |
| [measuring.md](measuring.md) | the commands: corpus, clock, kernel VM, userspace harness, phone | reproduce a number, or measure a change |
| [plan.md](plan.md) | the goal, the score, what zram needs from a codec, the phases, the risks, and where the project stands | know why the project does what it does |
| [explored-designs.md](explored-designs.md) | every design that was measured, kept or not, with its numbers, and an index by topic | try something new without measuring it twice |

The charts in [`plots/`](plots) are made by the scripts in [`tools/`](../tools/README.md) from the logs
of the measurements.

**Where to start.** For the codec: [seqlz.md](seqlz.md), the sections up to "The literals", then the
book, [seqlz, line by line](https://martin.ankerl.com/quetschn/). For the project: [plan.md §1.1](plan.md#11-the-score-memory-against-time-not-bars)
and [§9](plan.md#9-where-the-project-stands-and-the-next-actions). Before changing anything:
[explored-designs.md](explored-designs.md) and [CONTRIBUTING.md](../CONTRIBUTING.md).
