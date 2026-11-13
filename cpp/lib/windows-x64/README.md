# Windows x64 payload — 放置清单

`FindOrk.cmake` 认两种载荷形态，**合并包优先**（目录里只要出现 `ork_static.lib`
或 `libork_static.a`，就按合并包处理，不再找五个分离库）。放其中一种进本目录即可。

---

## A. 合并包（交付物，推荐）

来源：`harmonia_ork` 里跑 `scripts\package\windows\pack_static.ps1` 产出的
`dist\windows\`。

| 放到本目录的文件 | 从哪拷 | 必需 |
| --- | --- | --- |
| `ork_static.lib` | `dist\windows\lib\ork_static.lib` | **是** |
| `manifest.json` | `dist\windows\manifest.json` | 否，但放了 configure 会打出包版本与来源提交 |

三份 C ABI 头**不放这里**，覆盖到 `cpp\include\ork\sdk\c\`：

    cpp\include\ork\sdk\c\ork_abi.h
    cpp\include\ork\sdk\c\ork_client_abi.h
    cpp\include\ork\sdk\c\ork_export.h

合并包里 OpenSSL、libsodium、kcp、fmt、yaml-cpp 都已并入，**不需要**再放
`libssl.lib` / `libcrypto.lib` / `sodium.lib`，链了反而重复引入。仍然需要的只有
OpenSSL **头文件**（`<asio/ssl.hpp>` 会引用），configure 时用
`-DOPENSSL_ROOT_DIR=<prefix>` 或 vcpkg toolchain 带出来。

---

## B. 分离库（回退形态，历史开发布局）

> 交付与日常联调一律用 A。只有当本目录里**没有** `ork_static.lib` / `libork_static.a`
> 时，`FindOrk.cmake` 才会退到这种形态。

从 `harmonia_ork` 的构建目录导出五个归档：

| 放到本目录的文件 | 构建树里的位置 |
| --- | --- |
| `ork_core.lib` | `source\core\` |
| `ork_platform.lib` | `source\core\platform\` |
| `kcp.lib` | `thirdparty\kcp\` |
| `fmt.lib` | `thirdparty\fmt\` |
| `yaml-cpp.lib` | `thirdparty\yaml-cpp\` |

另外必须提供（不在 SDK payload 里）：

- `sodium.lib` — 直接放本目录即可（`FindOrk.cmake` 会把这里作为查找 hint），
  或传 `-DSODIUM_ROOT=<prefix>`
- OpenSSL ≥ 3 的 `libssl.lib` / `libcrypto.lib` + 头，传 `-DOPENSSL_ROOT_DIR`
  或 vcpkg toolchain

这种形态对应的 plaintext 开关是 ON，与合并包相反，两边必须一致。

---

## 复制命令（PowerShell）

```powershell
# 按实际路径改这三行
$ORK  = 'D:\src\Harmonia_ork'
$DEMO = 'D:\src\ork-sdk-demo'
$VCPKG = 'C:\vcpkg'

# 1) 库 + 元数据（合并包形态）
Copy-Item "$ORK\dist\windows\lib\ork_static.lib" "$DEMO\cpp\lib\windows-x64\" -Force
Copy-Item "$ORK\dist\windows\manifest.json"      "$DEMO\cpp\lib\windows-x64\" -Force

# 2) 三份头
Copy-Item "$ORK\dist\windows\include\ork\sdk\c\*.h" "$DEMO\cpp\include\ork\sdk\c\" -Force

# 3) 确认
Get-ChildItem "$DEMO\cpp\lib\windows-x64\"
Get-FileHash "$DEMO\cpp\lib\windows-x64\ork_static.lib" -Algorithm SHA256
```

`Get-FileHash` 的结果应该等于 `dist\windows\manifest.json` 里
`archives.merged.sha256` 的值——这一步是确认「拷进去的就是打出来的那个」，不是
中途被截断或换成了别的包。

---

## 构建与测试

```powershell
cd $DEMO\cpp
cmake -B build -A x64 `
      -DCMAKE_TOOLCHAIN_FILE="$VCPKG\scripts\buildsystems\vcpkg.cmake" `
      -DVCPKG_TARGET_TRIPLET=x64-windows-static-md
cmake --build build --config Release -j
cd build
ctest -C Release --output-on-failure
```

configure 阶段应该看到这几行（用来确认选对了形态）：

    --   payload : merged archive (...\cpp\lib\windows-x64\ork_static.lib)
    --   package : ork-static-sdk-0.2.0-windows-x64 — built from <sha>
    --   plaintext bootstrap: OFF

---

## 两种开关的组合，别搞混

| 手头的包 | `plaintext bootstrap` | 能跑什么 |
| --- | --- | --- |
| `pack_static.ps1` 默认（OFF） | OFF | 只能验证链接与 fail-closed 契约；`c_abi_host` 会在 `ork_host_create` 处失败，这是**预期** |
| `pack_static.ps1 -AllowPlaintextBootstrap` | ON | 全量测试：ctest 3/3 + pytest 56/56 |

要跑全量测试就打一份 ON 的包给 demo，正式交付那份仍然是 OFF。`FindOrk.cmake`
的判定顺序是：显式 `-DORK_PLAINTEXT_BOOTSTRAP=ON|OFF` → 载荷里的 `manifest.json`
→ 形态推断。所以只要把 `manifest.json` 一起放进来，它就会自动对齐，不用手填。

---

## 后续：第三方头还没进 payload（2026-11-11 记录，Windows 已出包终验）

Windows 静态包已出包并完成终验：合并包里 OpenSSL 与 libsodium 的实现已经并进
`ork_static.lib`，所以 **C ABI 消费者零外部依赖**，不需要 vcpkg、也不需要本地 SSL。
但 C++ 这一层仍差 OpenSSL 的**头文件**（公共头 `#include <asio/ssl.hpp>`），所以本
README 仍然要求 `-DOPENSSL_ROOT_DIR` 或 vcpkg toolchain。

仍待办的一项：把 `thirdparty/include/` 和 `include/ork/` 的 C++ 头一起打进 payload，
打包器从实际参与编译的 include 目录拷并把版本写进 `manifest.json`，头必须与归档内
实现同源同版本。做完之后工程师就彻底不用碰 vcpkg 了。决策记录见
`harmonia_ork/doc/OrkDeliveryFormPlan.md` §7.2。

---

## 常见错误

- `LNK2019` / `LNK4217` 一片 → `ORK_STATIC` 没生效。`FindOrk.cmake` 已经自动加了，
  如果是在别的工程里消费，要自己定义。
- `LNK2038: RuntimeLibrary 不匹配` → `/MT` 与 `/MD` 混用。vcpkg triplet 必须是
  `x64-windows-static-md`，不是 `x64-windows-static`。
- `Missing Ork static library 'ork_core'` → 目录里既没有 `ork_static.lib` 也没有
  五个分离库，八成是复制时路径少了一层（`dist\windows\lib\` 而不是 `dist\windows\`）。
- configure 里 `package : no manifest.json` → 只拷了 `.lib`，没拷 `manifest.json`。
  能跑，但看不出这个库是哪次提交打的。
