# Windows x64 Server Binary

This directory is reserved for the Windows x64 build of the Ork demo server.

## Current Status

**Not yet available.** Only the macOS arm64 demo server is prebuilt and published here.

Two separate blockers, worth keeping apart:

- **TLS backend** — `ORK_HAVE_TLS` is not defined on Windows (`source/CMakeLists.txt` in the
  Ork main repo skips it for `WIN32`), so the desktop static SDK ships no TLS bootstrap on this
  platform. Without TLS the bootstrap channel would have to run plaintext, and plaintext is
  hard-bound to loopback — a Windows host could serve local clients only, never a remote one.
- **Demo server packaging** — even once TLS lands, the demo server binary still has to be built
  and published for this platform.

## Prerequisites for Windows Support

- MSVC 2022+ (the static SDK is built and validated with VS2022 BuildTools, `/MD`)
- vcpkg `x64-windows-static-md` with OpenSSL and libsodium
- A Windows TLS backend — **not implemented** in the current desktop static SDK line

## Tracking

See the [Platform Support Matrix](../../README.md#platform-support) in the main README.
