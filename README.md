# quetschn

Fast compression for memory pages, built for Linux zram.

See [PLAN.md](PLAN.md) for the goal, the evidence behind it, and the plan.

## Licensing

quetschn is dual licensed under **`MIT OR GPL-2.0-only`**. You may use it under
either license, at your option.

- [LICENSE-MIT](LICENSE-MIT)
- [LICENSE-GPL-2.0](LICENSE-GPL-2.0)

The GPL-2.0-only option exists so the codec can be merged into the Linux kernel,
which requires GPL-2.0 compatibility. The MIT option exists so userspace and
non-GPL projects can use it too. This is the same approach zstd takes.

Every source file carries an SPDX identifier:

```c
// SPDX-License-Identifier: MIT OR GPL-2.0-only
```

Contributions are accepted under the same dual license. Sign off your commits
with `git commit -s` to certify the [Developer Certificate of
Origin](https://developercertificate.org/), as the Linux kernel requires.
