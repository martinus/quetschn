# The pull request loop

Part of the `issues` skill (SKILL.md). The same commands every time.

1. State first: `gh pr view <n> --json state` before every push to a PR branch. Martin merges himself
   while you work; a merged PR's follow-up goes on a new branch from `origin/main`.
2. Push: first `git show --stat HEAD`: every file you meant, and nothing else. `git add` the files by
   name, not `-A` (`patch` left `.orig` files and a build script its intermediates, and both got
   committed; a commit after `git reset --soft` took only what was staged and left half the change
   out). Then `git push -u origin <branch>`, with a name no PR had before
   (`gh pr list --state all --head <branch>` is empty): with a reused name, `gh pr view <branch>`
   shows the old, merged PR.
3. Create: `gh pr create --base main --head <branch> --title "<area>: <what>" --body-file <file>`.
   The body in Martin's voice: what, why, the numbers with hardware and compiler (copied from the
   log's `KERNEL CONFIG_CC_VERSION_TEXT` line, not from memory: "gcc 15" stood in two bodies for gcc
   16.2.1), the tests and the mutation that proved each, what was not run and why. No attribution, no
   footer, no session link.
4. Watch: `gh pr checks <n> --watch --interval 30` in the background. `ci-ok` is the required check.
   ClusterFuzzLite's `fuzz (address)` runs on PRs that touch the codec and is not part of `ci-ok`:
   read it too.
5. A red run: `gh run view <run id> --log-failed`. Re-run a job once only when it died before its own
   steps ran (on 9th October `same-bytes` hung 47 minutes in `apt-get`).
6. Merge: `gh pr merge <n> --merge --match-head-commit $(git rev-parse HEAD)`, with the sha the command
   prints, never one from memory. A refused merge means the head is not the one CI checked: read the
   checks on the new head, never force.
7. Close what it closed: `gh issue view <n> --json state` for each issue the PR names. Write
   `Closes #N` only for an issue the PR finishes. A PR that does part of one says "Does item 2 of #N",
   and the issue's body is rewritten to what is left, with why. GitHub finds the keyword before a
   number anywhere in a body or a commit message, quotes included: name an issue you do not close by
   its number alone.
8. Back onto main: `git switch main`, `git fetch -q origin main`, `git merge --ff-only origin/main`,
   then `git branch -d <branch>` and `git push origin --delete <branch>`. A finished worktree, merged
   and clean: `gra -y done <name>`, never the session's own.
