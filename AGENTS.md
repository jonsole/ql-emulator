# AGENTS.md

Guidance for coding agents working in this repository. For what the project is
and why, start with [README.md](README.md) and [docs/plan.md](docs/plan.md);
this file is about how to work on it.

## The short version

- **There is no code yet.** [docs/plan.md](docs/plan.md) is settled: every
  decision in it has been made, and a change of mind goes back through the
  same question and updates that file rather than quietly diverging from it.
  The current stage is the first one, the 68008 as a standalone library.
- **This is a sister project to zx-spectrum-emulator, not a fork of it.** Parts
  of that repository (the Engine, DAP and MCP plumbing, rewind's design, the
  screen and audio streams, `test_main.h`) are copied once and then left to
  drift apart. Nothing is copied in until the 68008 core passes its tests.
  No library is shared, and nothing here should reach into `../zx-spectrum-emulator`.
- **Other sessions may be editing the same working tree.** Never `git stash`,
  `git checkout -- <file>`, `git reset --hard` or `git clean`. Stage the files
  you changed by name; leave everything else as you found it.
- **Windows, PowerShell, MSVC.** C++17 with `/W4 /WX`, built with CMake behind
  `build.ps1`, as in zx-spectrum-emulator.

## Build and test

The build follows zx-spectrum-emulator's: `build.ps1` finds the Visual Studio
Build Tools and imports their environment itself, and the tests are plain
executables built from `core/tests/<name>.cpp` with the three-macro harness in
`test_main.h` (`TEST`, `CHECK`, `CHECK_EQ`). A fast suite runs by default; the
full CPU test set is labelled slow and run separately. Document the actual
`build.ps1` switches here once they exist.

**Expected values in tests are worked out, not captured**: cycle counts from
the 68008's documented timings, results from what the instruction is defined
to do, addresses from the program under test. A number copied from the code's
own output can only confirm the code agrees with itself.

**The CPU gate is SingleStepTests/m68000** (MIT), fetched by a script into a
gitignored location, never committed. It is a 68000 suite: 68008 timings come
from splitting each word access into two byte cycles, and a few instructions
are checked by hand against the 68008 tables in the M68000 User's Manual. The
other SingleStepTests repository, `680x0`, has no licence and is not used.

**Tests that need a Sinclair ROM skip themselves** when it is missing from
`roms/`. The JS ROM and the IPC ROM dump are supplied by whoever runs the
tests; Minerva (GPL v2) is the one ROM that may ship.

## Where things go

| Path | What |
|---|---|
| `core/src/` | The CPU and machine parts (`m68008.cpp`, `zx8301.cpp`, `zx8302.cpp`, `ipc.cpp`, `microdrive.cpp`, `ql.cpp`), and later the Engine, DAP and MCP code |
| `core/tests/` | The test suite |
| `vscode-extension/` | Its own extension: debugger type `ql`, a screen panel, a 68k assembly language mode |
| `scripts/` | Fetching the test suites, `.mdv` tools |
| `docs/` | One file per topic |
| `tools/` | Downloaded vasm and vlink (gitignored) |
| `roms/` | ROM images (gitignored) |

## Things that bite

- **There is no real QL to measure against.** Disputed behaviour is settled by
  the documentation, the ROM disassembly, and other emulators (sQLux, MAME and
  the MiSTer core). Say which source a decision rests on, in a comment beside
  the code.
- **The 68008 has an 8-bit bus.** The core is clocked per bus access: a byte
  access is 4 clocks plus wait states, a word is two byte cycles and a long
  word four, and the ZX8301 can delay any of them. Prefetch and internal cycles
  are counted but not modelled at the pins. Don't describe timing as
  pin-accurate.
- **Addresses are 32-bit inside, 20 bits on the bus.** The 68008 drives
  A0-A19, so the QL's 1 MB wraps. Addresses shown to users are six hex digits.
- **Ports differ from zx_server's** so both can run side by side: DAP 4712,
  MCP 8100, screen 8600, audio 8601; a verifying second instance uses 14712 /
  18100 / 18600 with `--no-audio`.
- **Never commit or bundle a Sinclair ROM or the IPC ROM dump.** Their
  distribution has never been licensed. The same goes for vasm: its licence
  forbids bundling, so it is downloaded into `tools/`, never committed.

## Style

**C++.** C++17, warnings as errors (`/W4 /WX`). Plain structs, `for` loops and
`if`/`else` rather than clever templates or algorithm chains; `///` on
declarations saying what a thing is for; `//` prose inside functions saying
*why*, especially where the obvious alternative would be wrong. Comments
explain the reasoning and the history that forced it, in full sentences.
Constants get names and a comment on where the number comes from (a manual
page, a line in sQLux or MAME, a ROM address).

**JavaScript** (the extension). CommonJS, no build step, no npm dependencies.
Keep logic that can be tested without VS Code out of the files that import
`vscode`, and test it from Node.

**Docs.** A new facility isn't finished until [docs/](docs/) describes it, one
file per topic, linked from the README. Say what is not done as plainly as
what is.

## Commits

Commit only when asked. The history is a linear `master`. A subject is one
sentence saying what the change does, in the project's voice; the body says
what changed and why, what it was checked against, and anything left out. Add
files by name, and say in your summary which changes you left uncommitted.

From the first code commit, a change worth telling users about goes under
**Unreleased** in `CHANGELOG.md`, in the same commit. Releases start no
earlier than the first milestone (SuperBASIC on screen) and are cut only when
asked, since a pushed tag publishes a release.

## Agent skills

### Issue tracker

Issues live in GitHub Issues on jonsole/ql-emulator, managed with the `gh` CLI. See `docs/agents/issue-tracker.md`.

### Triage labels

The five default labels: `needs-triage`, `needs-info`, `ready-for-agent`, `ready-for-human`, `wontfix`. See `docs/agents/triage-labels.md`.

### Domain docs

Single-context: one `GLOSSARY.md` and `docs/adr/` at the repo root. See `docs/agents/domain.md`.
