---
name: kernel-patches
description: Rules for the seqlz patch series to upstream Linux (Phase 7), for preparing a version, changelogs, the cover letter, git format-patch, resends, bot findings and replies to reviewers. Use when preparing, sending or revising the kernel patch series, drafting a changelog or cover letter, or handling a reply from a maintainer or a review bot such as Sashiko.
---

# Kernel patch series

Source: kernel Maintainers Summit, Oct 2026. Never quote, cite or link its report.

## Who writes what

- Changelogs and cover letter: Martin writes them. Supply code, tests, numbers, bullet points.
  Changelog = problem, change, measured result; short (padded text reads as tool output).
- Tool use: stated in the cover letter only (tools, which step, Martin reviewed and tested all).
  Before sending, check `Documentation/process/coding-assistants.rst` in the base tree for
  `Assisted-by:` (the summit agreed to drop the requirement; the file decides). Martin picks the
  wording. The "no Claude attribution" rule covers this repo and GitHub, not the cover letter.
- Humans review why, where and taste: the cover letter answers "why a new codec" and "why this place
  in the tree" up front.

## Before each version

Copy and check off:

```text
- [ ] .claude/rules/kernel-copy.md holds for the kernel copy
- [ ] cleanup pass over the kernel files: comments match code; no dead code, unused option or
      unneeded function; less code wins; backend API follows the other zram backends; Martin can
      explain every line
- [ ] KUnit error-path tests updated for changed code (seqlz_test_damaged)
- [ ] tools/kernel-port/kunit.sh passes on the exact base
- [ ] builds with the newest gcc and the newest clang
- [ ] git format-patch --base=<commit> on the tree MAINTAINERS names for zram at send time; that tree
      named in the cover letter
```

## After sending

- Resend policy: never one version per bot (Sashiko) finding or per mail. Wait for human design
  comments, answer all in one version. Wrong bot finding: Martin explains why. Bot finding in code the
  series does not touch: separate fix if real, else ignore.
- Replies to reviewers: Martin, in person, soon, plain words; never pasted tool output. Fix small
  nits (include order, names) without discussion; maintainers use them to test for a human.
- After "applied": check linux-next yourself, do not ask.
