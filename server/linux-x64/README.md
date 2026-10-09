# Linux x64 Server Binary

This directory is reserved for the Linux x64 build of the Ork demo server.

## Current Status

**Not yet available.** The Ork SDK currently only supports macOS arm64.

## Building from Source (Future)

When Linux support is added, you will need:
- Ubuntu 22.04+ / GCC 12.3+
- CMake 3.22+
- OpenSSL 3.x development headers
- libsodium development headers

```bash
# Expected build steps (not yet functional)
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release -DORK_ALLOW_PLAINTEXT_BOOTSTRAP=ON
cmake --build build-linux --target embed_minimal
```

## Tracking

See the [Platform Support Matrix](https://github.com/user/ork-sdk-demo#platform-support) in the main README.
