# Reflect, and land what is clear

Part of the `issues` skill (SKILL.md), its last step.

What would make the next session more efficient and more effective? Answer from what happened in
this session, not from opinion. List what cost time or went wrong:

- a command that was refused or retried, and why
- a CI run that went red, and what had to happen before it was green
- a measurement that had to run again, and what spoiled the first one
- a question the owner had to answer twice, or a correction they made
- a fact that `grep` on CLAUDE.md, `.claude/` and `docs/measuring.md` did not find, so a whole file
  was read for it
- a rule that did not load when it was needed (in a `.claude/rules/` file whose `paths:` missed the
  files touched), or loaded everywhere and saved nothing
- a script written from nothing that `tools/` could hold
- a rule that was wrong, stale, or not followed because nothing pointed to it, and a rule that cost
  reading and saved nothing

For each, the change that stops it: add, change, move or remove a bullet in CLAUDE.md, a
`.claude/rules/` file or `docs/measuring.md`, a step in this skill, or a script in `tools/`. Removing
counts as much as adding. Rules go into the repository, not into memory: memory stays on one
machine.

Land the clear ones in one PR, merged on green without asking, through `pr-loop.md`. Clear
means one reading, the evidence is in this session, and it touches only CLAUDE.md, `.claude/`, the
rules of `docs/measuring.md` or a script in `tools/`.

Ask about the rest first, with `AskUserQuestion`: two or three options, their cost, one recommended.
That includes anything that bends the goal, and any change to the codec: a bug found here is an
issue, not a reflection.

If nothing went wrong, say so in one line. A PR made only to have one costs the owner a review and
teaches nothing.
