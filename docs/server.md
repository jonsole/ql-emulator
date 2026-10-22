# The server

`ql_server` runs one QL and lets tools drive it over MCP. A debugger (DAP), the screen stream and the VS Code extension come later in Stage 2 and in Stage 3.

## Running it

```
ql_server --rom <image> [--ram 128|640] [--mcp-host <host>] [--mcp-port <port>] [--uncapped]
```

| Option | Default | |
|---|---|---|
| `--rom <image>` | required | The system ROM, up to 48K: the JS ROM, or Minerva. Never bundled; see the README. |
| `--ram 128\|640` | 128 | The standard 128K, or 640K with the expansion area filled. |
| `--mcp-host <host>` | `127.0.0.1` | |
| `--mcp-port <port>` | 8100 | zx_server uses 8000, so the two run side by side; a verifying second instance uses 18100. |
| `--uncapped` | off | Run as fast as possible. Otherwise the QL runs in realtime, 7.5 MHz against the wall clock. |

Everything is bound before anything is served, and a failure -- a bad option, a ROM that cannot be read or is over 48K, a port already in use -- stops the server with a message rather than leaving it half up. The server logs each connection and tool call, one timestamped line each, on stdout.

## MCP

JSON-RPC 2.0 over Streamable HTTP, at `http://<host>:<port>/mcp`, protocol revision 2025-06-18: POST a message (or a batch), and the reply comes back in the response. A notification is answered 202 with no body; GET and anything else but POST and DELETE is refused with 405. Tools only -- no resources or prompts.

Tool names are zx_server's wherever the meaning is the same. A tool's result is text content carrying JSON; a failure is a result with `isError` set, so the model sees what went wrong. Addresses are written as six hex digits, and taken as numbers or as hex strings such as `"$30000"` or `"0x30000"`.

| Tool | Arguments | Does |
|---|---|---|
| `run` | `wait` (default true) | Runs the machine. With `wait: false`, returns at once and leaves it running; otherwise returns when paused. |
| `pause` | | Stops a run. |
| `step` | `instructions` (default 1) | Runs whole instructions; refused while running. |
| `reset` | | The reset button: the 68008's reset sequence. RAM is left as it was. |
| `get_state` | | `pc`, `running`, `stopped` (by STOP), `clock` (CPU clocks since power-on), and the registers: `d0`-`d7`, `a0`-`a7` (A7 being whichever stack pointer SR selects), `usp`, `ssp`, `sr`. |
| `read_memory` | `address`, `length` (default 1, up to 65536) | The bytes as the CPU would read them, as hex. |
| `get_screen` | | The 512×256 screen as a PNG image. |
| `server_info` | | The server's name and version. |

`pc` is the address of the next instruction. The core itself keeps PC as the next prefetch address, 4 further on, as the 68000 does (see `Registers::pc`).

## The Engine

The machine runs on its own thread, and every tool call goes through a queue to it, so several clients share one machine without locking it against each other. A run is itself a queued job that lasts until it is paused; every 2,000 instructions it serves the queue's other jobs, so reading memory or the state reaches a running machine, and it publishes the screen at most every 20 ms. Pause and the screen bypass the queue. This is zx-spectrum-emulator's design, cut down: watchpoints, logpoints, trace, the profiler, rewind, audio and video are not here yet.

## What is copied from zx-spectrum-emulator

Copied once and left to drift, as the plan says: the socket, HTTP, base64, logging and file helpers (each marked at its top), the Engine's queue and run-loop design, and MCP's message and HTTP handling. Vendored: nlohmann/json and stb_image_write; see [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).
