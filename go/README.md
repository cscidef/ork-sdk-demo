# Ork SDK - Go Bindings

## Status: Planned

Go bindings for the Ork SDK are not yet implemented. This directory is reserved for future development.

## Planned Approach: CGo FFI

The Ork SDK exposes a stable C ABI (`ork_abi.h` / `ork_client_abi.h`) with 22 exported functions.
Go bindings will use CGo to call these functions directly.

### Expected API Design

```go
package ork

// Host (Server)
type Host struct { handle C.OrkHostHandle }
func NewHost(config HostConfig) (*Host, error)
func (h *Host) Start() error
func (h *Host) Stop()
func (h *Host) Send(connID uint32, data []byte, channel Channel) error
func (h *Host) Broadcast(data []byte, channel Channel) error

// Client
type Client struct { handle C.OrkClientHandle }
func NewClient(config ClientConfig) (*Client, error)
func (c *Client) Connect() error
func (c *Client) Disconnect()
func (c *Client) PumpOnce()
func (c *Client) Send(data []byte, channel Channel) error
```

### Prerequisites

- Go 1.21+
- CGo enabled (`CGO_ENABLED=1`)
- Ork static libraries for your platform (see `cpp/lib/`)
- C compiler (clang/gcc)

### Contributing

If you're interested in implementing Go bindings, the C ABI headers are in `cpp/include/ork/sdk/c/`.
The pure C11 demos in `cpp/demos/01_echo_server/` and `cpp/demos/02_echo_client/` show minimal usage patterns.
