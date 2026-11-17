# Windows x64 载荷 — dev 包（已入库）

本目录放的是**已经打好并实测通过的 Windows 载荷**，不是占位说明。
`FindOrk.cmake` 只认合并归档，目录里只要有 `ork_static.lib` 就按合并包处理。

```
ork_static.lib      27,819,382 B   sha256 292453d2f8d18e5d7e05bf8d51591e5ebe28615d64c272537975739ee1244d6d
manifest.json       打包来源与安全事实（configure 会读它）
SHA256SUMS.txt      逐文件校验
```

三份 C ABI 头不放这里，在 `cpp/include/ork/sdk/c/`（`ork_abi.h` / `ork_client_abi.h` / `ork_export.h`）。

---

## 为什么这里必须是 dev 包

C ABI 存在一处**能力不对称**：

| ABI 面 | TLS 字段 | 现状 |
| --- | --- | --- |
| Client `OrkClientConfig` v2（96B） | `bootstrap_security_mode` / `tls_ca_file` / `tls_expected_server_name` / `tls_use_system_trust_store` / `tls_handshake_timeout_ms` | 有 |
| Host `OrkHostConfig`（64B） | 无 | **缺口**：桥接层把 Bootstrap 通道硬映射为 `PlaintextDevelopment` |

而本轮 Windows 构建没有编 OpenSSL 通道（`ORK_HAVE_TLS` 未定义，`manifest.security.bootstrap_tls = disabled`）。
两边一叠加：

- **正式包**（`plaintext_bootstrap_compiled = false`）：`ork_host_create` 直接返回 `OrkErr_InvalidArg`，Client 的 TLS 也建不起来 —— 一个无法建立任何 Bootstrap 连接的包。它存在的意义是验证 ABI / 链接 / fail-closed 契约，不是给 demo 跑。
- **dev 包**（本目录，`plaintext_bootstrap_compiled = true`）：Host 能以 `PlaintextDevelopment` 创建，Bootstrap 走明文 TCP 且**强制 bind loopback**，数据面仍走 OrkCipher AEAD。demo 要的正是这个。

所以 demo 仓固定用 dev 包；正式交付包在 `harmonia_ork` 的 `dist/windows`（Release `/MD`）和 `dist/windows-debug`（Debug `/MDd`），两者都已在仓库里。

---

## 两个包的差异（本目录 = 左边）

| 项 | 本目录 dev 包 | `dist/windows` 正式包 |
| --- | --- | --- |
| `plaintext_bootstrap_compiled` | `true` | `false` |
| `bootstrap_tls` | `disabled` | `disabled` |
| Host 能否创建 | 能（loopback 强制） | 不能（fail closed） |
| Client TLS 连接 | 建不起来 | 建不起来 |
| 用途 | demo / 本地联调 / ctest | 集成方链接验证、ABI 契约 |

macOS 的载荷（`../macos-arm64/`）是另一种组合：`plaintext_bootstrap_compiled = true` **且** `bootstrap_tls = enabled`，所以 mac 上 TLS 与明文两条路径都在。

---

## 构建与测试（以下是实跑过的命令）

```powershell
cd cpp
cmake -S . -B build-win -A x64 `
      -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake `
      -DVCPKG_TARGET_TRIPLET=x64-windows-static-md
cmake --build build-win --config Release --parallel 8
cd build-win
ctest -C Release --output-on-failure
```

configure 阶段应该出现这几行（用来确认选对了形态与开关）：

```
--   payload : merged archive (...\cpp\lib\windows-x64\ork_static.lib)
--   package : ork-static-sdk-0.2.0-windows-x64 — built from 66c59c91...
--   plaintext bootstrap: ON (must match the archive; ...)
--   tls channel        : OFF (from manifest; ...)
```

`plaintext bootstrap` 与 `tls channel` 都从 `manifest.json` 自动读取，不用手填；
确实要覆盖时才用 `-DORK_PLAINTEXT_BOOTSTRAP=ON|OFF` / `-DORK_HAS_TLS=ON|OFF`。

Windows 10/11 + VS 2022 BuildTools（MSVC 19.44）+ CMake 4.1 + vcpkg `x64-windows-static-md` 下实测：

| 项 | 结果 |
| --- | --- |
| configure | 通过，`payload=merged archive` / `plaintext=ON` / `tls=OFF` |
| build | 通过，8 个可执行文件（5 demo + 3 test） |
| `ctest -C Release` | 3/3 passed（`c_abi_host` / `c_abi_client` / `c_roundtrip`） |
| `07_tutorial` | PASS |
| `06_encryption` | PASS（Round A `encryption_active=1`、Round B `=0`，两轮 echo OK） |
| `05_broadcast` | PASS（3 客户端，All=3 / Filtered=2 全部符合） |
| `01_echo_server` + `02_echo_client` | PASS（Reliable + Unreliable 双通道回显 OK） |

`test_c_abi_client` 里有三条断言与 TLS 能力有关（v1 72B、`Default`、`Tls`）：
本包 `tls=OFF`，所以它们断言的是「fail closed 且错误信息点名 TLS-capable build」；
在 macOS（`tls=ON`）上同一份代码断言的是「创建成功」。分支由 `ORK_DEMO_HAS_TLS` 控制，
两边都严格，不是放宽。

---

## 常见错误

- `LNK2019` / `LNK4217` 一片 → `ORK_STATIC` 没生效。`FindOrk.cmake` 已自动定义；在别的工程里消费要自己加。
- `LNK2038: RuntimeLibrary 不匹配` → `/MT` 与 `/MD` 混用。triplet 必须是 `x64-windows-static-md`，不是 `x64-windows-static`。
- `Missing Ork static library` → 目录里没有 `ork_static.lib`，或复制时少了一层（是 `lib\` 下面那个）。
- `ork_host_create -> OrkErr_InvalidArg`，错误信息含 `PlaintextDevelopment` → 手里是正式包（`plaintext=false`），不是本目录的 dev 包。
- `ork_client_create -> ... requires a TLS-capable build` → 同上：本包没编 OpenSSL，Client 的 TLS 路径不可用。
- configure 里 `package : no manifest.json` → 只拷了 `.lib` 没拷 `manifest.json`，能力开关就退化成默认值，别这么用。
