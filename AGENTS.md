# AGENTS.md

Guidance for coding agents working in this repository. For what the project is
and why, start with [README.md](README.md) and [docs/plan.md](docs/plan.md);
this file is about how to work on it.

## The short version

- **Stage 1 is under way: the 68008 as a standalone library.** Its tickets
  are GitHub issues #1-#12. [docs/plan.md](docs/plan.md) is settled: every
  decision in it has been made, and a change of mind goes back through the
  same question and updates that file rather than quietly diverging from it.
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

```powershell
.\scripts\fetch-m68000-tests.ps1     # once: SingleStepTests/m68000 into .test-suites\ (gitignored)
cd core
.\build.ps1                          # Debug, into build\Debug
.\build.ps1 -Release                 # RelWithDebInfo, into build\RelWithDebInfo
.\build.ps1 -Test                    # build, then the fast CTest suite (label "slow" excluded)
.\build.ps1 -Release -Slow           # the whole SingleStepTests set
.\build.ps1 -Target ql_tests         # the tests alone; any one target by name
.\build.ps1 -BuildDir <dir>          # somewhere else entirely
```

`build.ps1` finds the Visual Studio Build Tools and imports their environment
itself, as zx-spectrum-emulator's does. Run it from the PowerShell tool, not
from bash: a fresh `powershell` started from bash refuses the script under the
default execution policy.

Each test is a plain executable built from `core/tests/<name>.cpp` with the
three-macro harness in `test_main.h` (`TEST`, `CHECK`, `CHECK_EQ`; an
exception escaping a test fails it), registered in `core/tests/CMakeLists.txt`
with `ql_add_test`. The CPU library, `ql_m68008`, depends on nothing but the
`Bus` interface in `m68008.h`.

`m68000_suite` is the CPU gate. CTest runs it twice: `m68000_sample`, every
tenth test of each file, in the fast suite, and `m68000_full`, every test,
labelled slow. Run by hand, it takes file names (`m68000_suite.exe MOVE.b`) to
run just those, and `--full`. The files it runs are the `FILES` list at its
top, which names every file in the suite. Which instruction each opcode is
comes from one table, `m68008_decode.cpp`; the routines are in a source file
per group, which `m68008_internal.h` lists. Every one of the suite's 317,500
tests runs, address errors included; a change to the core is finished when the
slow run still passes them all. Without the fetched suite it exits 77, which
CTest reports as skipped.

**Address errors are where the 68000's microcode shows most.** The stacked PC
is `fault_pc_`, which each instruction moves on at its own points, and some
instructions make their (An)+ and -(An) changes, and set their flags, only after
the access that would fault. Each such rule was read off SingleStepTests and
says so in a comment where it is applied; a change to an instruction's order of
work has to keep them.

**Expected values in tests are worked out, not captured**: cycle counts from
the 68008's documented timings, results from what the instruction is defined
to do, addresses from the program under test. A number copied from the code's
own output can only confirm the code agrees with itself.

**The CPU gate is SingleStepTests/m68000** (MIT), fetched at a pinned commit
into `.test-suites/`, never committed. Its `.json.bin` files are read
directly (`m68000_file.h`); `decode.py` is never run. It is a 68000 suite:
`m68008_timing.h` derives the 68008's bus cycles by splitting each word cycle
into two byte cycles, even byte first, 4 clocks more, and a few instructions
are checked by hand against the 68008 tables in the M68000 User's Manual
(#12). The other SingleStepTests repository, `680x0`, has no licence and is
not used.

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
- **Addresses are 32-bit inside, 24 bits out of the CPU, 20 on the QL.** The
  core drives the 68000's 24 bits, since SingleStepTests uses all 16 MB; the
  QL's 48-pin 68008 has only A0-A19, so its 1 MB wraps, and dropping the top
  bits is the machine's bus's job. Addresses shown to users are six hex digits.
- **PC is the next prefetch address**, not the instruction's: with the queue
  full it is 4 past the opcode in IR. That is how the 68000 and the suite
  both see it.
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
