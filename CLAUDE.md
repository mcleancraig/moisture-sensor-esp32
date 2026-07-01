# Claude Code — project instructions

## Branching workflow

Two-person team — always use feature branches:

1. Cut a **version branch** from `main` — e.g. `2.5.2`
2. Cut a **feature branch** from the version branch — e.g. `feat/my-feature`
3. Do the work on the feature branch, then open a PR into the version branch and merge promptly
4. Repeat for each feature until the version is ready to ship
5. Open a final PR from the version branch into `main`, merge, tag, release

- `main` is production only — never commit directly to it
- Feature branches should be short-lived; merge as soon as the feature is complete
- One PR per feature into the version branch
- Riskier versions can be tagged as a beta (`vX.X.X-bNN`) straight off the version branch before step 5, and promoted to stable by dropping the suffix — see README.md "Beta releases" for the full procedure

## Git commands

Always use `git -C /Users/craig/git/moisture-sensor-esp32 <subcommand>` rather than
`cd /Users/craig/git/moisture-sensor-esp32 && git <subcommand>`.

The `cd && git` pattern triggers a security prompt on every invocation ("this command
changes directory before running git"). `git -C` runs the command in the target
directory without a shell `cd`, avoiding the prompt entirely.

**Good:**
```
git -C /Users/craig/git/moisture-sensor-esp32 status
git -C /Users/craig/git/moisture-sensor-esp32 add moisture-sensor-esp32.ino
git -C /Users/craig/git/moisture-sensor-esp32 commit -m "..."
git -C /Users/craig/git/moisture-sensor-esp32 push
```

**Avoid:**
```
cd /Users/craig/git/moisture-sensor-esp32 && git status
```

For non-git commands that genuinely require a working directory (e.g. arduino-cli
compile, `gh` commands), `cd` is still fine — the prompt only fires for `cd && git`.

# CLAUDE.md

Behavioral guidelines to reduce common LLM coding mistakes. Merge with project-specific instructions as needed.

**Tradeoff:** These guidelines bias toward caution over speed. For trivial tasks, use judgment.

## 1. Think Before Coding

**Don't assume. Don't hide confusion. Surface tradeoffs.**

Before implementing:
- State your assumptions explicitly. If uncertain, ask.
- If multiple interpretations exist, present them - don't pick silently.
- If a simpler approach exists, say so. Push back when warranted.
- If something is unclear, stop. Name what's confusing. Ask.

## 2. Simplicity First

**Minimum code that solves the problem. Nothing speculative.**

- No features beyond what was asked.
- No abstractions for single-use code.
- No "flexibility" or "configurability" that wasn't requested.
- No error handling for impossible scenarios.
- If you write 200 lines and it could be 50, rewrite it.

Ask yourself: "Would a senior engineer say this is overcomplicated?" If yes, simplify.

## 3. Surgical Changes

**Touch only what you must. Clean up only your own mess.**

When editing existing code:
- Don't "improve" adjacent code, comments, or formatting.
- Don't refactor things that aren't broken.
- Match existing style, even if you'd do it differently.
- If you notice unrelated dead code, mention it - don't delete it.

When your changes create orphans:
- Remove imports/variables/functions that YOUR changes made unused.
- Don't remove pre-existing dead code unless asked.

The test: Every changed line should trace directly to the user's request.

## 4. Goal-Driven Execution

**Define success criteria. Loop until verified.**

Transform tasks into verifiable goals:
- "Add validation" → "Write tests for invalid inputs, then make them pass"
- "Fix the bug" → "Write a test that reproduces it, then make it pass"
- "Refactor X" → "Ensure tests pass before and after"

For multi-step tasks, state a brief plan:
```
1. [Step] → verify: [check]
2. [Step] → verify: [check]
3. [Step] → verify: [check]
```

Strong success criteria let you loop independently. Weak criteria ("make it work") require constant clarification.

---

**These guidelines are working if:** fewer unnecessary changes in diffs, fewer rewrites due to overcomplication, and clarifying questions come before implementation rather than after mistakes.
