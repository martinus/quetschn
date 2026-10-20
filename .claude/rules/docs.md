---
paths:
  - "**/*.md"
  - "docs/**"
  - "tools/plot-*.py"
---

# Editing docs: invariants that break silently

- Code comments, scripts, SVG comments cite `docs/plan.md` sections (§1.1, §3.1, §3.3, §3.5, §4, §5.1,
  §5.2, §5.3, Phase 1, Phase 4). Never renumber.
- Anchors used across files (`plan.md#11-the-score-memory-against-time-not-bars`, `format.md#status`,
  `format.md#example`, many `explored-designs.md#...`). Never rename a heading without fixing its
  links. After Markdown edits: `tools/check-links.py` (checks every tracked `*.md`, `.claude/` too).
- Doc numbers go stale when tables or the matcher change. Verify before quoting:
  `seqlz_ref.load_tables(12)` (from `tools/seqlz-ref/`) gives codes; `tools/seqlz-compress/compress.c`
  (`-Isrc`) compresses a page file. Name the tables used ("tables of 7 October").
- Codec charts: x = time per page, y = memory as % of page, both from 0. Every chart labels every
  axis, categorical ones too. `tools/plot-*.py` take the mean over the boots of a `BOOTS=n` log.
- `docs/explored-designs.md`: never read it whole (~160 000 tokens); its top section says how to
  search it and how to add an entry.
