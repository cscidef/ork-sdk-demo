# Demo 07 · SDK 教程（`IHostListener` / `ISender` 纯 Mock 走读）

不连任何网络：`DemoFramework` 在本地注入连接 / 载荷 / 统计事件，
让 `IHostListener` 的回调语义（连接生命周期、Reliable/Unreliable 分流、
业务自定义命令解析）可以在一次运行里完整走读一遍。

## 运行

```bash
cmake --build build --target 07_tutorial
./build/bin/07_tutorial
```

## 与其他 demo 的关系

- 想看 `SessionFramework::Create()` 的真实网络最小宿主 → [01_echo_server](../01_echo_server)
- 想看宿主侧业务接入（Join/移动/广播）的完整可运行版本 → [03_cpp_host](../03_cpp_host)
- 本 demo 只回答一个问题：**业务方要实现的接口长什么样、事件以什么顺序到达**。

正式接入时，把 `DemoFramework` 换成 `SessionFramework::Create(config)`，
其余代码（`DemoRoomListener`、命令解析、`GetSender()` 调用）原样可用。

## 来源

迁移自 harmonia_ork `source/examples/tutorial/sdk_demo.cpp`（2026-10，
`cleanup/project-structure` 分支）。
