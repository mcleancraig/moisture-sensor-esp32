# Claude Code — project instructions

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
