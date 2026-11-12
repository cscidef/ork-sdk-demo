# Linux x64 Server Binary

This directory is reserved for the Linux x64 build of the Ork demo server.

## Current Status

**Not yet available.** Only the macOS arm64 demo server is prebuilt and published here.

The engine itself does build on Linux and defines `ORK_HAVE_TLS` there, and the Ork main
repository has a Linux packaging entry point (`scripts/package/linux/pack_static.sh`) — but
that package has not yet been built and validated on an Ubuntu 22.04 machine, and no demo
server binary is published for this platform.

## Building from Source

Build it from the **Ork main repository** — this demo repo only consumes prebuilt artifacts:

- Ubuntu 22.04+ / GCC 12.3+, CMake 3.16+
- OpenSSL 3.x from the system packages (TLS bootstrap is compiled in on Linux)
- libsodium, fmt, yaml-cpp and KCP come from the Ork source tree

```bash
cd <path>/harmonia_ork
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target embed_minimal -j4
```

Copy the resulting `embed_minimal` (or `ork_room_server`) into `server/linux-x64/bin/` so
`server/start.sh` can find it. See the [Platform Support Matrix](../../README.md#platform-support).
