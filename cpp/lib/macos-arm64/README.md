# macOS arm64 payload — 放置清单

`FindOrk.cmake` 认两种载荷形态，**合并包优先**（目录里只要出现 `libork_static.a`
或 `ork_static.lib`，就按合并包处理，不再找五个分离库）。放其中一种进本目录即可。

---

## A. 合并包（交付物，推荐）

来源：`harmonia_ork` 里跑 `scripts/package/macos/pack_static.sh` 产出的
`dist/macos/`。

| 放到本目录的文件 | 从哪拷 | 必需 |
| --- | --- | --- |
| `libork_static.a` | `dist/macos/lib/libork_static.a` | **是** |
| `manifest.json` | `dist/macos/manifest.json` | 否，但放了 configure 会打出包版本与来源提交 |
| `SHA256SUMS.txt` | `dist/macos/SHA256SUMS.txt` | 否，用来核对拷进来的是不是同一个包 |

三份 C ABI 头**不放这里**，覆盖到 `cpp/include/ork/sdk/c/`：

    cpp/include/ork/sdk/c/ork_abi.h
    cpp/include/ork/sdk/c/ork_client_abi.h
    cpp/include/ork/sdk/c/ork_export.h

合并包里 OpenSSL 3.6.3、libsodium、kcp、fmt、yaml-cpp 都已并入，**不需要**再放
`libssl.a` / `libcrypto.a` / `libsodium.a`，链了反而重复引入。仍然需要的只有
OpenSSL **头文件**（`<asio/ssl.hpp>` 会引用），Homebrew 装好即可，找不到时传
`-DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3`。

---

## B. 分离库（开发构建树导出）

从 `harmonia_ork` 的构建目录导出五个归档：

| 放到本目录的文件 | 构建树里的位置 |
| --- | --- |
| `libork_core.a` | `build/source/core/` |
| `libork_platform.a` | `build/source/core/platform/` |
| `libkcp.a` | `build/thirdparty/kcp/` |
| `libfmt.a` | `build/thirdparty/fmt/` |
| `libyaml-cpp.a` | `build/thirdparty/yaml-cpp/` |

另外必须提供（不在 SDK payload 里）：OpenSSL ≥ 3 与 libsodium，`FindOrk.cmake`
会通过 pkg-config 与 Homebrew 路径自动发现，缺失时传 `-DOPENSSL_ROOT_DIR` /
`-DSODIUM_ROOT`。

这种形态对应的 plaintext 开关是 ON，两边必须一致。

---

## 复制命令

```bash
ORK=/Users/<you>/hadoWorld/HADO-PROJ/harmonia_ork
DEMO=/Users/<you>/hadoWorld/HADO-PROJ/ork-sdk-demo

# 1) 库 + 元数据（合并包形态）
cp "$ORK/dist/macos/lib/libork_static.a" "$DEMO/cpp/lib/macos-arm64/"
cp "$ORK/dist/macos/manifest.json"       "$DEMO/cpp/lib/macos-arm64/"
cp "$ORK/dist/macos/SHA256SUMS.txt"      "$DEMO/cpp/lib/macos-arm64/"

# 2) 三份头
cp "$ORK/dist/macos/include/ork/sdk/c/"*.h "$DEMO/cpp/include/ork/sdk/c/"

# 3) 确认拷进来的是打出来的那个包
cd "$DEMO/cpp/lib/macos-arm64"
shasum -a 256 libork_static.a | cut -d' ' -f1
python3 -c "import json;print(json.load(open('manifest.json'))['archives']['merged']['sha256'])"
```

两行输出必须相同。

---

## 构建与测试

```bash
cd "$DEMO/cpp"
cmake -B build-merged-macos -S . \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_ARCHITECTURES=arm64 \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 \
      -DORK_PLATFORM=macos-arm64
cmake --build build-merged-macos -j 8
ctest --test-dir build-merged-macos --output-on-failure
```

configure 阶段应该看到这几行（用来确认选对了形态）：

    --   payload : merged archive (.../cpp/lib/macos-arm64/libork_static.a)
    --   package : ork-static-sdk-0.2.0-darwin-arm64 — built from 2f831a16...
    --   plaintext bootstrap: OFF

---

## 两种开关的组合，别搞混

| 手头的包 | `plaintext bootstrap` | 能跑什么 |
| --- | --- | --- |
| `pack_static.sh` 默认（OFF） | OFF | 只能验证链接与 fail-closed 契约；`ctest` 的三条明文用例会在 `ork_host_create` / `ork_client_create` 处 fail closed，这是**预期**，不是缺陷 |
| 打一份 `-AllowPlaintextBootstrap` 的开发包 | ON | 全量测试：ctest 3/3 |

`FindOrk.cmake` 的判定顺序是：显式 `-DORK_PLAINTEXT_BOOTSTRAP=ON|OFF` → 载荷里的
`manifest.json` → 形态推断。所以只要把 `manifest.json` 一起放进来，它就会自动对齐，
不用手填。

---

## 常见错误

- 宿主链接报 `SSL_*` / `sodium_*` 未定义 → 拿到的是分离库，不是交付包；改用
  `libork_static.a`。
- `-DORK_PLAINTEXT_BOOTSTRAP=ON` 但包是 OFF → 编译期定义与归档不一致，运行期仍
  fail closed；开关注释里写了必须以归档为准。
- configure 里 `package : no manifest.json` → 只拷了 `.a`，没拷 `manifest.json`。
  能跑，但看不出这个库是哪次提交打的。
- `missing required architecture arm64` → 拷错了平台的包；macOS 交付包只有 arm64，
  没有 universal。
