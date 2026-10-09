# Ork Demo Server

A self-contained build of the Ork minimal embedded host (`embed_minimal`),
packaged so it can run on a clean machine with no Homebrew / SDK dependencies.

## Platform Support

| Platform      | Directory              | Status                          |
|---------------|------------------------|---------------------------------|
| macOS arm64   | `macos-arm64/`         | ✅ Available (self-contained)   |
| Linux x64     | `linux-x64/`           | 🚧 Planned — see its README     |
| Windows x64   | `windows-x64/`         | 🚧 Planned — see its README     |

Only macOS arm64 ships a prebuilt binary today. `start.sh` auto-detects the
platform and picks the matching directory.

## Layout

```
server/
├── macos-arm64/
│   ├── bin/embed_minimal      # self-contained host binary
│   ├── lib/*.dylib            # bundled OpenSSL + libsodium (rewritten to @executable_path)
│   └── bundle/ork.bundle      # Ork plugin bundle
├── config/                    # demo configuration (plaintext_dev)
│   ├── network.yaml  server.yaml  room.yaml
│   └── sync.yaml  log.yaml  metrics.yaml
├── start.sh                   # launch in background (writes .server.pid, server.log)
└── stop.sh                    # graceful stop (SIGTERM, then SIGKILL after 5s)
```

## Start / Stop

```bash
# from the repository root
make server-start          # or: bash server/start.sh
make server-stop           # or: bash server/stop.sh

# use a different port
GAME_PORT=9500 bash server/start.sh
```

`start.sh` refuses to start if the port is already in use or a server is
already running (stale PID files are cleaned up automatically). Runtime stdout
goes to `server/server.log`; the engine's own log goes to `server/logs/ork.log`.

## Ports

`embed_minimal` takes a single `--gamePort`. Both the TCP bootstrap listener and
the UDP/KCP data socket bind to **that same port number** (they are different
protocols, so they can share it):

| Channel        | Default        | Binding                       |
|----------------|----------------|-------------------------------|
| TCP bootstrap  | `9400`         | `127.0.0.1` (loopback only)   |
| UDP / KCP data | `9400`         | `0.0.0.0`                     |

> Note: there is **no** separate `9401` UDP port for this binary. The `+1/+2`
> port convention belongs to the gRPC-based `ork_room_server`, not
> `embed_minimal`.

## Security Mode: `plaintext_dev`

This demo runs the bootstrap channel in **plaintext development mode**:

- The binary is compiled with `-DORK_ALLOW_PLAINTEXT_BOOTSTRAP=ON`.
- `config/network.yaml` sets `bootstrap_security: plaintext_dev`.
- The TCP bootstrap listener is **hard-bound to loopback** — remote clients
  cannot reach it. This is intentional and safe for local demos only.
- The KCP/UDP payload cipher (`enable_encryption: true`) is still active; it is
  independent of bootstrap-channel security.

**Production deployments must use `bootstrap_security: tls`** with a certificate
chain and private key, and a binary built with the default
`-DORK_ALLOW_PLAINTEXT_BOOTSTRAP=OFF` (which physically removes the plaintext
path).

## Rebuilding / Repacking from Source

The macOS binary is repacked from the `harmonia_ork` build tree:

```bash
# 1. build embed_minimal in the SDK repo (once)
cmake --build /path/to/harmonia_ork/build-demo --target embed_minimal

# 2. copy + make self-contained (bundle dylibs, rewrite install names, re-sign)
ORK_ROOT=/path/to/harmonia_ork make pack-server
#   or: bash scripts/pack_server.sh

# 3. verify no external dylib paths remain
otool -L server/macos-arm64/bin/embed_minimal
```

`scripts/pack_server.sh` copies every non-system dylib into `lib/`, rewrites
install names to `@executable_path/../lib` (binary) and `@loader_path`
(sibling dylibs), then applies an ad-hoc `codesign` (required on arm64 after
`install_name_tool`).
