<div align="center">

# Ork SDK Demo

**Encrypted, low-latency network transport with reliable + unreliable channels — runnable end to end.**

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
![Server](https://img.shields.io/badge/server-macOS%20arm64-blue)
![Python](https://img.shields.io/badge/python-56%20tests%20passing-brightgreen)
![C%2FC%2B%2B](https://img.shields.io/badge/c%2Fc%2B%2B-136%20checks%20passing-brightgreen)

<!--
CI badge placeholder — wire these up when the pipeline lands:
[![CI](https://github.com/<org>/ork-sdk-demo/actions/workflows/ci.yml/badge.svg)](https://github.com/<org>/ork-sdk-demo/actions/workflows/ci.yml)
-->

</div>

---

## What is Ork

**Ork** is a pure transport SDK: it moves opaque bytes between a game client and a game
host, and deliberately knows nothing about your game logic. There are no protobuf
schemas to share, no room/session semantics, no gameplay types — just a byte pipe with
strong delivery and security guarantees.

What you get on that pipe:

| Capability | Detail |
|---|---|
| **Bootstrap** | TCP handshake (Bootstrap V2, 109 B) issues the session credential |
| **Reliable channel** | UDP carried over **KCP** — ordered, retransmitting, ARQ-backed |
| **Unreliable channel** | Raw UDP — unordered, no retransmit, lowest latency |
| **Encryption** | **ChaCha20-Poly1305-IETF** AEAD on the data channel, with replay + tamper rejection |
| **Handshake** | UDP Handshake V2 (42 B) authenticates the data channel before any payload flows |
| **Interface** | Stable **C ABI** (22 exported functions) plus native **C++20** and **Python** surfaces |

Both channels share **one port**: the TCP bootstrap listener and the UDP/KCP data socket
bind to the same port number (different protocols, so no conflict).

This repository is the **demo and integration-test companion** to the Ork engine. It ships
a prebuilt, self-contained demo server, complete client suites in Python and C/C++, and
reserved skeletons for Go and C#.

---

## Architecture

```
   CLIENT SIDE                                            SERVER SIDE
  ┌──────────────────────────┐                          ┌──────────────────────────┐
  │      Your Game App       │                          │   Your Game Host Logic   │
  └────────────┬─────────────┘                          └────────────▲─────────────┘
               │  opaque bytes                                       │  opaque bytes
  ┌────────────▼─────────────────────────────┐   ┌───────────────────┴──────────────┐
  │              Ork Client SDK              │   │           Ork Host SDK           │
  │                                          │   │                                  │
  │   Python   │  C ABI  │  C++20  │  C#/Go  │   │   C ABI (13 fn)  │  C++20        │
  │  ctypes    │  9 fn   │ OrkClient│ planned │   │   SessionFramework│  embed_minimal│
  └────────────┬─────────────────────────────┘   └───────────────────▲──────────────┘
               │                                                     │
               │   1. TCP  ── Bootstrap V2 (109 B) ───────────────►  │
               │   2. UDP  ── Handshake V2 (42 B) ────────────────►  │
               │   3. UDP  ══ KCP reliable  ══ ChaCha20-Poly1305 ══► │
               │   4. UDP  ── raw unreliable ─ ChaCha20-Poly1305 ──► │
               │              (same port, default 9400)              │
        ┌──────▼──────────────────────────────────────────────────── ┴──────┐
        │                        N E T W O R K                              │
        └───────────────────────────────────────────────────────────────────┘
```

The demo server in [`server/`](server/) is `embed_minimal` — the smallest real Ork host,
packaged self-contained so it runs on a clean machine with no Homebrew or SDK installs.

---

## Platform Support

| Platform | Server | Python Client | C/C++ Client | Go | C# |
|----------|--------|---------------|--------------|-----|-----|
| **macOS arm64** | ✅ Available | ✅ Available | ✅ Available | 🚧 Planned | 🚧 Planned |
| **Linux x64** | 🚧 Planned | ✅ Ready\* | ✅ Ready\* | 🚧 Planned | 🚧 Planned |
| **Windows x64** | 🚧 Planned | ✅ Ready\* | ✅ Ready\* | 🚧 Planned | 🚧 Planned |

\* The **client** code is fully cross-platform. Running it on Linux/Windows only needs a
server binary for that platform — drop it into `server/linux-x64/` or `server/windows-x64/`
and into `cpp/lib/<os>-<arch>/` for the C++ static libraries. See
[`server/linux-x64/README.md`](server/linux-x64/README.md) and
[`server/windows-x64/README.md`](server/windows-x64/README.md).

Platform detection lives in [`server/start.sh`](server/start.sh): it maps `uname -s` /
`uname -m` onto the matching `server/<platform>/` directory and exits with a clear error
on anything unsupported.

---

## Quick Start (macOS arm64)

### Prerequisites

| Requirement | Version | Needed for |
|---|---|---|
| macOS | 12.0+ | everything |
| Python | 3.10+ | Python demos & tests |
| CMake | 3.20+ | C/C++ demos & tests |
| C/C++ compiler | C11 + C++20 (AppleClang 15+) | C/C++ demos, Python KCP bridge |
| OpenSSL | 3.x (`brew install openssl@3`) | C/C++ demos |
| libsodium | recent (`brew install libsodium`) | C/C++ demos |

### Four commands to a working echo

```bash
git clone <this-repo> ork-sdk-demo && cd ork-sdk-demo

make setup                                    # venv + deps + KCP bridge + offline self-check
make server-start                             # embed_minimal listening on 9400
cd python && .venv/bin/python demos/01_echo/client.py
```

Expected: the client bootstraps over TCP, handshakes over UDP, then echoes a 25-byte
`ORKBENCH1` probe on **both** the reliable (KCP) and unreliable (UDP) channels, and exits `0`.

```bash
cd .. && make server-stop                     # done — shut the server down
```

> The Python test suite manages the server itself (`python/tests/conftest.py` starts it,
> reuses it if the port is already listening, and stops it afterwards), so `make test`
> works without a manual `server-start`.

---

## Repository Structure

```
ork-sdk-demo/
├── README.md                  ← you are here
├── Makefile                   setup / server-start / server-stop / test / demo-echo / clean
├── LICENSE                    MIT
│
├── server/                    prebuilt demo host (embed_minimal)
│   ├── macos-arm64/             bin/ + lib/ + bundle/ — self-contained, no Homebrew needed
│   ├── linux-x64/               placeholder + porting notes
│   ├── windows-x64/             placeholder + porting notes
│   ├── config/                  demo YAML config (bootstrap_security: plaintext_dev)
│   ├── start.sh  stop.sh        background launch / graceful stop (PID + log files)
│   └── README.md
│
├── python/                    Python client suite — 6 demos, 56 tests
│   ├── lib/                     ork_frame / ork_cipher / ork_client_session / kcp bridge
│   ├── demos/01..06/            each with its own README + runnable client.py
│   ├── tests/                   pytest: 2 offline + 5 online (auto start/stop server)
│   ├── testdata/                payload vectors + frozen golden hex (C++-aligned)
│   ├── setup.sh                 one-shot: venv → deps → KCP bridge → offline self-check
│   └── README.md
│
├── cpp/                       C / C++ client suite — 6 demos, 3 CTest binaries (136 checks)
│   ├── include/ork/             public SDK headers (C ABI + C++20 surfaces)
│   ├── lib/macos-arm64/         prebuilt static libs (ork_core, ork_platform, kcp, fmt, yaml-cpp)
│   ├── thirdparty/include/      vendored header-only deps (asio, fmt, spdlog, ikcp, …)
│   ├── cmake/FindOrk.cmake      assembles everything into the Ork::Core imported target
│   ├── demos/01..06/            each with main.c|.cpp + CMakeLists.txt + README.md
│   ├── tests/                   test_c_abi_host / test_c_abi_client / test_cpp_roundtrip
│   └── README.md
│
├── go/                        🚧 reserved — CGo binding plan
├── csharp/                    🚧 reserved — .NET P/Invoke plan (Unity bindings live in main repo)
└── scripts/
    ├── run_all_tests.sh         unified pipeline: Python + C++ (what `make test` runs)
    └── pack_server.sh           repack embed_minimal from a harmonia_ork build tree
```

Everything under `cpp/` is **self-contained**: headers, prebuilt static libraries and
header-only third-party dependencies all ship in the repo, so building the C/C++ demos
never needs the `harmonia_ork` source tree.

---

## Demo Catalog

### Python — [`python/demos/`](python/demos/)

| # | Demo | What it shows | Key flags |
|---|------|---------------|-----------|
| 01 | [`echo`](python/demos/01_echo/) | Minimal full link; dual-channel 25 B `ORKBENCH1` round trip | `--count` |
| 02 | [`reliable_vs_unreliable`](python/demos/02_reliable_vs_unreliable/) | KCP vs raw UDP delivery rate and RTT percentiles on one session | `--iterations` |
| 03 | [`encryption`](python/demos/03_encryption/) | Cipher frame anatomy, seal/open, replay & tamper rejection, encrypted echo | `--rounds` `--offline-only` |
| 04 | [`connection_lifecycle`](python/demos/04_connection_lifecycle/) | State machine: connect → send → idle → disconnect → reconnect | `--idle` `--reconnects` |
| 05 | [`multi_client`](python/demos/05_multi_client/) | N concurrent independent sessions with aggregate success rate | `--clients` `--echoes` |
| 06 | [`latency_measure`](python/demos/06_latency_measure/) | Per-channel RTT min / avg / p50 / p95 / p99 / max | `--pings` `--warmup` |

### C / C++ — [`cpp/demos/`](cpp/demos/)

| # | Demo | Language | Surface | What it shows |
|---|------|----------|---------|---------------|
| 01 | [`echo_server`](cpp/demos/01_echo_server/) | pure C11 | C ABI Host (13 fns) | Minimal server: create → callbacks → start → pump → stop |
| 02 | [`echo_client`](cpp/demos/02_echo_client/) | pure C11 | C ABI Client (9 fns) | Minimal client: 96 B v2 config, caller-driven `pump_once`, dual-channel check |
| 03 | [`cpp_host`](cpp/demos/03_cpp_host/) | C++20 | `SessionFramework` + `IHostListener` | TLS-capable bootstrap and `IConnectionRegistry` governance |
| 04 | [`cpp_client`](cpp/demos/04_cpp_client/) | C++20 | `OrkClient` + `IClientListener` | Full state transitions, typed `SendResult`, per-channel RTT |
| 05 | [`broadcast`](cpp/demos/05_broadcast/) | C++20 | `ISender::Broadcast` | Fan-out via `All()` / `Where()` / `Of()` + self-proving `BroadcastReport` |
| 06 | [`encryption`](cpp/demos/06_encryption/) | C ABI | `ork_host_*` + `ork_client_*` | A/B two-round proof that `encryption_active` follows the server switch |

Demos 05 and 06 are **single-process self-proving**: they run host and client in the same
process on an ephemeral port, so they need no external server.

### Planned

| Language | Directory | Status |
|----------|-----------|--------|
| Go | [`go/`](go/) | 🚧 CGo FFI over the 22-function C ABI — see [`go/README.md`](go/README.md) |
| C# (.NET) | [`csharp/`](csharp/) | 🚧 P/Invoke — see [`csharp/README.md`](csharp/README.md) |
| C# (Unity) | `harmonia_ork/unity/com.hado.ork/` | ✅ Shipped in the main repo (UPM package `com.hado.ork` v0.2.0) |

---

## Running Tests

Everything at once:

```bash
make test                     # or directly: bash scripts/run_all_tests.sh
```

The runner reports `[PASS]` / `[FAIL]` / `[SKIP]` per language and exits non-zero if
anything failed. It skips a language cleanly when its toolchain or venv is missing
(`make setup` first for Python).

Per language:

```bash
make test-python              # pytest, 56 tests (offline + online; auto start/stop server)
make test-cpp                 # cmake configure + build + ctest, 3 suites / 136 checks
```

Or directly:

```bash
cd python && .venv/bin/python -m pytest tests/ -v
cd python && .venv/bin/python -m pytest tests/test_frame_codec.py tests/test_cipher.py -v   # offline only
cd cpp && cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j4 && (cd build && ctest --output-on-failure)
```

| Suite | Count | Needs server? |
|-------|-------|---------------|
| Python `pytest` | **56 tests** | No — `conftest.py` starts/stops `server/` automatically (reuses it if already listening; override with `ORK_TEST_PORT`) |
| C++ `ctest` | **3 suites / 136 checks** | No — fully self-contained, ephemeral OS-assigned ports, safe under `ctest -j8` |

---

## Building From Source

The demo server and the `cpp/lib/` static libraries are **built artifacts** produced from the
Ork engine repository. To rebuild them:

```bash
# 1. build the minimal embedded host in the engine repo
cmake --build /path/to/harmonia_ork/build-demo --target embed_minimal

# 2. repack it here: bundle dylibs, rewrite install names to @executable_path, ad-hoc re-sign
ORK_ROOT=/path/to/harmonia_ork make pack-server

# 3. verify the binary is genuinely self-contained (only system dylibs should remain)
otool -L server/macos-arm64/bin/embed_minimal
```

`scripts/pack_server.sh` needs `ORK_ROOT` (it fails fast with a usage hint when unset);
it copies every non-system dylib into `server/macos-arm64/lib/`,
rewrites install names to `@executable_path/../lib` (binary) and `@loader_path` (sibling
dylibs), then applies an ad-hoc `codesign` — mandatory on arm64 after `install_name_tool`.

The engine itself (transport core, C ABI, C++ SDK, Unity bindings) lives in the separate
**`harmonia_ork`** repository. See [`server/README.md`](server/README.md) for the full
packaging story and [`cpp/README.md`](cpp/README.md) for the link-closure details.

---

## Good To Know

These three bites account for most first-run confusion:

1. **TCP and UDP share one port.** There is no separate UDP port. `embed_minimal` takes a
   single `--gamePort` (default `9400`) and binds both the TCP bootstrap listener and the
   UDP/KCP data socket to it. Change it with `GAME_PORT=9500 bash server/start.sh`.

2. **Client `udp_port` must be set explicitly.** Leaving it `0` falls back to **9401**, which
   nothing listens on — the connect will simply fail. Always set `udp_port = tcp_port`.

3. **The demo server only echoes one exact frame shape.** Its `TransportDemoHandler` echoes
   back, on the same channel, only payloads that are **exactly 25 bytes prefixed with
   `ORKBENCH1`**; anything else is parsed as protobuf and dropped on failure.

   ```
   ORKBENCH1 frame = b"ORKBENCH1" (9 B) + seq (8 B LE) + send_time_ns (8 B LE) = 25 B
   ```

   Every echo demo and test in this repo goes through that single path.

Also worth reading before you integrate:

- **`ork_host_pump_once()` does not drive the network loop.** It is a ~1 ms cooperative yield;
  the host's IO is driven by an internal pump thread. Real caller-driven pumping exists only
  on the client side (`ork_client_pump_once()` → `io.poll()` + `io.restart()`), which a Unity
  or single-threaded client must call every frame.
- **Callbacks fire on Ork's internal strand thread.** Keep them non-blocking and never call
  `stop()` / `destroy()` from inside one (deadlock). `disconnect()` is safe.
- **C ABI Host is pinned to `PlaintextDevelopment` bootstrap**, which force-collapses the bind
  address to loopback. For TLS 1.3 use the C++ SDK (demo 03) with `TlsServerAuthenticated`.
- Full caveats, log-routing table and `BroadcastReport` accounting rules:
  [`cpp/README.md`](cpp/README.md) → *Known Limitations*.

---

## Security Note

The demo server is configured with `bootstrap_security: plaintext_dev` and compiled with
`-DORK_ALLOW_PLAINTEXT_BOOTSTRAP=ON`. The TCP bootstrap listener is **hard-bound to
loopback**, so remote clients cannot reach it. The data-channel cipher
(ChaCha20-Poly1305 AEAD) **remains active regardless** — bootstrap security and payload
encryption are independent.

This configuration is for local demos only. Production must use `bootstrap_security: tls`
with a real certificate chain and private key, and a binary built with the default
`-DORK_ALLOW_PLAINTEXT_BOOTSTRAP=OFF`, which physically removes the plaintext path.
See [`server/README.md`](server/README.md).

---

## Makefile Targets

```bash
make help           # list all targets
make setup          # Python venv + deps + KCP bridge + offline self-check
make pack-server    # repack embed_minimal from a harmonia_ork build (needs ORK_ROOT)
make server-start   # start the demo server in the background
make server-stop    # graceful stop (SIGTERM, then SIGKILL after 5 s)
make test           # Python + C++ via scripts/run_all_tests.sh (pass/fail/skip summary)
make test-python    # Python tests only
make test-cpp       # C++ tests only
make demo-echo      # start server → run Python echo demo → stop server
make clean          # remove venv, build dirs, server runtime files
```

---

## License

MIT — see [`LICENSE`](LICENSE).
