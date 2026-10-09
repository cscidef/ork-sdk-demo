# Ork SDK - C# Bindings

## Status: Planned (Unity bindings exist in main repo)

Standalone C# bindings (outside Unity) are not yet implemented.
However, a complete Unity C# runtime already exists in the Ork main repository at `unity/com.hado.ork/`.

## Existing Unity Bindings

The Ork project includes a full Unity UPM package (`com.hado.ork` v0.2.0) with:
- `OrkSession.cs` — Host (server) wrapper
- `OrkClientSession.cs` — Client wrapper  
- `OrkNative.cs` / `OrkClientNative.cs` — Raw P/Invoke declarations
- `OrkMainThreadDispatcher.cs` — Unity main-thread event bridge
- `OrkPumpDriver.cs` — Frame-driven pump

These use P/Invoke against the native `ork.bundle` (macOS) / `ork.dll` (Windows, planned).

## Planned Approach: .NET P/Invoke

For non-Unity C# (.NET 6+), the same P/Invoke pattern applies:

### Expected API Design

```csharp
namespace Ork.Sdk;

public class OrkHost : IDisposable
{
    public static OrkHost Create(HostConfig config);
    public void Start();
    public void Stop();
    public SendResult Send(uint connectionId, ReadOnlySpan<byte> data, Channel channel);
    public event Action<uint> OnConnected;
    public event Action<uint, DisconnectReason> OnDisconnected;
    public event Action<uint, ReadOnlySpan<byte>, Channel> OnPayload;
}

public class OrkClient : IDisposable
{
    public static OrkClient Create(ClientConfig config);
    public void Connect();
    public void Disconnect();
    public void PumpOnce();
    public SendResult Send(ReadOnlySpan<byte> data, Channel channel);
    public ClientState State { get; }
}
```

### Prerequisites

- .NET 6+ SDK
- Ork native library for your platform:
  - macOS arm64: `ork.bundle` (included in `server/macos-arm64/bundle/`)
  - Windows x64: `ork.dll` (planned)
  - Linux x64: `libork.so` (planned)

### Using the Unity Bindings Directly

If you're building a Unity project, install the UPM package from the Ork main repository:
```json
{
  "dependencies": {
    "com.hado.ork": "file:../../harmonia_ork/unity/com.hado.ork"
  }
}
```

### Contributing

The C ABI headers are in `cpp/include/ork/sdk/c/`. The Unity C# source in the main repo
(`harmonia_ork/unity/com.hado.ork/Runtime/`) serves as the reference implementation for P/Invoke patterns.
