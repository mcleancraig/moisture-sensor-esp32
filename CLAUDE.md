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
