/**
 * @file ork_client_abi.h
 * @brief Ork 客户端 C ABI —— Unity / C# P/Invoke 客户端接入面
 *
 * 设计原则（与 ork_abi.h 保持一致，破坏即破坏 ABI 稳定性）：
 * - 纯 C11：本头文件不依赖任何 C++ 头，可被 C11 编译器独立解析
 *   （门禁：`clang -x c -std=c11 -fsyntax-only ork_client_abi.h` 退出码 0）。
 * - extern "C" 包裹：C++ 侧包含时自动切换 C 链接。
 * - 零 C++ 类型跨界：无 std::string / template / namespace / 异常穿越边界。
 * - 不透明句柄：OrkClientHandle 为前向声明指针，内部实现不外泄。
 * - 版本化结构体：所有跨边界结构体首字段为 `uint32_t struct_size`，
 *   调用前必须置为 `sizeof(该结构体)`。新增字段只追加在尾部，
 *   旧调用方传入较小的 struct_size 时，实现只读取其已知前缀，
 *   从而在不破坏 ABI 的前提下扩展。
 * - 枚举即契约：本头文件中的枚举值是对外稳定 ABI，**不**镜像内部 C++
 *   枚举编号；桥接实现（ork_client_abi.cpp）负责双向映射，并用 static_assert
 *   钉死此处数值 + 验证映射全覆盖。
 * - 基础类型复用 ork_abi.h：OrkError / OrkSendResult / OrkChannel /
 *   OrkDisconnectReason / ork_abi_version()。
 *
 * 明确不含（属业务/编排语义，由宿主在其上自建）：
 *   PlayerId、Join/Leave、房间状态、广播（客户端为单一连接）、统计快照。
 *
 * 线程契约（与宿主 ABI 不同 —— 客户端为「手动驱动单线程」模型）：
 * - 无内部 pump 线程：调用方必须在同一线程周期调用 ork_client_pump_once()
 *   驱动事件循环（如 Unity 主循环每帧一次）；全部 ork_client_* 调用必须
 *   在该同一线程进行（内部 OrkClient 为单线程模型）。
 * - 所有回调（on_connected / on_disconnected / on_payload）在
 *   ork_client_pump_once() 或 ork_client_disconnect() 的调用栈内触发，
 *   实现必须非阻塞。
 * - 禁止在回调内调用 ork_client_destroy（析构自身 → 未定义行为）。
 *   需销毁时先投递到宿主自己的主循环线程。
 * - ork_client_disconnect / ork_client_send 可在回调内安全调用。
 * - on_payload 的 data 为借用指针，仅本次回调期间有效，不得跨调用持有。
 *
 * 错误上报：生命周期/管理类函数返回 OrkError，发送类返回 OrkSendResult；
 * 最近一次客户端 API 失败的错误文本经 ork_client_last_error() 获取
 * （与 ork_last_error() 同语义；两者各自维护独立的线程局部缓冲，
 * 分属不同的实现单元，互不覆盖）。
 */

#ifndef ORK_SDK_C_ORK_CLIENT_ABI_H
#define ORK_SDK_C_ORK_CLIENT_ABI_H

#include <stdint.h>
#include <stddef.h>

#include "ork/sdk/c/ork_abi.h"  /* 复用 OrkError / OrkSendResult / OrkChannel / OrkDisconnectReason */

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/* 枚举 —— 对外稳定 ABI（数值由本头文件权威定义，勿与内部 C++ 枚举混淆）    */
/* ========================================================================== */

/**
 * @brief 客户端连接状态机（对外稳定 ABI）。
 *
 * 内部 C++ ork::client::ClientState 为 7 值（A1.1 起含 TlsHandshaking）；
 * 本枚举为其稳定收敛子集（6 值）：TlsHandshaking 归并入 Connecting
 * （TLS 握手仍属连接建立阶段，宿主无需区分）。桥接层走显式映射 +
 * static_assert 钉死。
 */
typedef enum OrkClientState
{
    OrkClientState_Disconnected  = 0,  ///< 未连接（初始 / 断开后）
    OrkClientState_Connecting    = 1,  ///< TCP 连接中（含 TLS 握手阶段，收敛映射）
    OrkClientState_Bootstrapping = 2,  ///< 等待接收 BootstrapFrame
    OrkClientState_Handshaking   = 3,  ///< UDP 握手 + 等待 KCP ACK
    OrkClientState_Connected     = 4,  ///< 已就绪，可收发
    OrkClientState_Disconnecting = 5   ///< 关闭中
} OrkClientState;

/**
 * @brief Bootstrap 通道安全模式（A1.2，对外稳定 ABI）。
 *
 * 数值由本头文件权威定义，**不**镜像内部 C++ BootstrapSecurityMode 编号
 * （C++：PlaintextDevelopment=0 / TlsServerAuthenticated=1）。桥接层
 * （ork_client_abi.cpp）负责映射并用 static_assert 钉死。
 *
 * 遵循本 ABI「0 = 取默认」约定：0 显式表示默认，即 TLS（生产安全默认）。
 * 因此旧调用方零初始化整个结构体后不填本字段，得到的仍是 TLS 而非明文。
 *
 * - PlaintextDev：明文 TCP。仅 ORK_ALLOW_PLAINTEXT_BOOTSTRAP=ON 构建可用
 *   （否则 ork_client_create 即拒绝，fail closed）；编入后连接目标仍强制
 *   loopback（OrkClient 连接时校验）。仅限本地开发。
 * - Tls（= 默认）：TLS 1.3 服务端认证。证书链/信任锚缺失或握手失败即
 *   断开，无明文回退。
 */
typedef enum OrkBootstrapSecurity
{
    OrkBootstrapSecurity_Default      = 0,  ///< 0 → 默认（= TlsServerAuthenticated）
    OrkBootstrapSecurity_PlaintextDev = 1,  ///< 明文开发模式（须编入 + loopback）
    OrkBootstrapSecurity_Tls          = 2   ///< TLS 1.3 服务端认证（= 默认）
} OrkBootstrapSecurity;

/* ========================================================================== */
/* 结构体 —— 首字段恒为 struct_size，尾部可扩展                              */
/* ========================================================================== */

/**
 * @brief 客户端配置（POD，无 std::string；展平内部
 *        ClientConfig + KcpConfig，v2 起含 Bootstrap TLS 字段）。
 *
 * 语义为 0 / NULL 的字段一律取"默认/禁用"，便于调用方只填关心的项：
 * - host：NULL 或空串 → 默认 "127.0.0.1"（IP 或可解析域名）
 * - tcp_port / udp_port：0 → 默认 9400 / 9401
 * - enable_kcp / enable_encryption：0=disabled, 1=enabled
 * - max_packet_size：0 → 默认 1200（合法区间 [50, 1414]，越界创建即拒绝）
 * - connect_timeout_ms：0 → 默认 5000
 * - kcp_*：0 → 取 KcpConfig 默认值（默认值即与服务端对齐的推荐值）；
 *   非 0 显式覆盖，非法值（越界 / stream_mode≠0）在 ork_client_create
 *   处即被拒绝（返回 OrkErr_InvalidArg，详见 ork_client_last_error()）。
 *
 * A1.2（v2 布局）尾部追加 Bootstrap 通道安全字段（见 OrkBootstrapSecurity）：
 * - bootstrap_security_mode：OrkBootstrapSecurity 值；0 → 默认 TLS；
 *   非法值（非 0/1/2）创建即拒绝。v1 调用方（struct_size =
 *   ORK_CLIENT_CONFIG_V1_SIZE）未填本字段，语义同为默认 TLS。
 * - tls_ca_file：NULL / 空串 → 不加载私有 CA（仅系统 trust store）；
 *   非空时作受信锚加载（PEM 文件路径）。文件本身在连接时才读取。
 * - tls_expected_server_name：NULL / 空串 → 默认空。TLS 模式下必填
 *   （同时用于 SNI 与 SAN/hostname 验证）；留空时连接阶段 fail closed
 *   （工厂内拒绝，无明文回退）。
 * - tls_use_system_trust_store：三态。0 → 默认（启用）；1 → 显式启用；
 *   -1 → 显式禁用（仅 tls_ca_file 作为信任锚，私有 CA 部署场景）；
 *   其他值创建即拒绝。
 * - tls_handshake_timeout_ms：0 → 默认 5000。超时即断开（无回退）。
 *
 * struct_size 版本化（A1.2 起双版本并存，64 位平台 LP64 布局）：
 * - v2（当前）：struct_size = sizeof(OrkClientConfig)（96），TLS 字段
 *   可用。注意 v2 首字段 bootstrap_security_mode 位于 offset 68，落在
 *   v1 布局的尾部 padding 内（v1 末字段止于 68、sizeof=72）——因此实现
 *   按精确版本白名单（72 或 96）判定，而非逐字段位置判定；
 * - v1（A1.2 之前）：struct_size = ORK_CLIENT_CONFIG_V1_SIZE（72），由旧
 *   头文件编译的二进制调用方传入；实现只读取 v1 已知前缀，TLS 字段按
 *   零值处理（= 默认 TLS / 不加载私有 CA）；
 * - 其他值（更小 / 居中 / 更大）一律拒绝（OrkErr_InvalidArg），
 *   杜绝部分填充与未来版本静默截断。
 *
 * @note 客户端 ABI 限制：
 * - enable_kcp / enable_encryption 当前为「意向」字段：内部 OrkClient
 *   暂不据此裁剪行为（KCP 会话始终建立；是否加密以服务端下发的
 *   BootstrapFrame.flags 为权威）。字段已预留，供后续接线。
 * - kcp_nodelay / kcp_fast_resend / kcp_no_congestion 在内部 KcpConfig 中
 *   0/false 属合法值，但本 ABI 统一约定 0 = 取默认（默认即推荐值），
 *   不透传 0；如需这些非默认取值须待后续 ABI 扩展。
 * - kcp_stream_mode 必须为 0（OrkFrame/AEAD 要求消息模式帧定界）。
 * - tls_ca_file / tls_expected_server_name 为借用指针：仅 ork_client_create
 *   调用期间被读取（内部拷贝为 std::string，不依赖调用方缓冲区生命周期），
 *   返回后调用方可立即释放。
 */
typedef struct OrkClientConfig
{
    uint32_t    struct_size;          ///< = sizeof(OrkClientConfig)（v2）或 ORK_CLIENT_CONFIG_V1_SIZE（v1）
    const char* host;                 ///< 服务端主机（NULL → "127.0.0.1"）
    uint16_t    tcp_port;             ///< TCP bootstrap 端口（0 → 9400）
    uint16_t    udp_port;             ///< UDP/KCP 数据端口（0 → 9401）
    int32_t     enable_kcp;           ///< 0=disabled, 1=enabled
    int32_t     enable_encryption;    ///< 0=disabled, 1=enabled
    uint32_t    max_packet_size;      ///< 单包上限 / KCP MTU（0 → 默认 1200）
    uint32_t    connect_timeout_ms;   ///< 连接+握手整体超时（0 → 默认 5000）
    int32_t     kcp_nodelay;          ///< KCP nodelay 模式（0 → 默认 2）
    int32_t     kcp_interval_ms;      ///< KCP flush 间隔 ms（0 → 默认 10）
    int32_t     kcp_fast_resend;      ///< 快速重传阈值（0 → 默认 2）
    int32_t     kcp_no_congestion;    ///< 1=关闭拥塞控制（0 → 默认 1）
    int32_t     kcp_send_window;      ///< 发送窗口（段）（0 → 默认 128）
    int32_t     kcp_receive_window;   ///< 接收窗口（段）（0 → 默认 128）
    int32_t     kcp_stream_mode;      ///< 必须为 0（消息模式）；非 0 拒绝
    int32_t     kcp_rx_min_rto_ms;    ///< 最小 RTO ms（0 → 默认 10）
    /* ---- A1.2（v2）：Bootstrap 通道安全（TLS 1.3 / 明文开发模式） ---- */
    int32_t     bootstrap_security_mode;      ///< OrkBootstrapSecurity（0 → 默认 TLS）
    const char* tls_ca_file;                  ///< TLS 受信 CA PEM 路径（NULL/空 → 仅系统 trust store）
    const char* tls_expected_server_name;     ///< SNI + SAN/hostname 验证名（TLS 模式必填）
    int32_t     tls_use_system_trust_store;   ///< 0=默认(启用) 1=启用 -1=禁用；其他拒绝
    uint32_t    tls_handshake_timeout_ms;     ///< TLS 握手超时 ms（0 → 默认 5000）
} OrkClientConfig;

/* ========================================================================== */
/* 回调函数指针类型                                                          */
/* ========================================================================== */

/// 连接就绪（TCP bootstrap + UDP 握手 + KCP ACK 完成）
typedef void (*OrkClientOnConnected)(void* user, uint32_t connection_id);

/// 连接断开或连接失败。reason 为 OrkDisconnectReason 枚举值（自客户端内部原因映射）
typedef void (*OrkClientOnDisconnected)(void* user, uint32_t reason);

/// 收到 opaque 载荷。data 为借用指针，仅本次调用期间有效；channel 为 OrkChannel
typedef void (*OrkClientOnPayload)(void* user, const uint8_t* data, uint32_t size, uint32_t channel);

/**
 * @brief 客户端回调集合。任一回调可为 NULL（表示不关心该事件）。
 *
 * user 为透传上下文，原样回传给每个回调。
 */
typedef struct OrkClientCallbacks
{
    uint32_t                 struct_size;   ///< = sizeof(OrkClientCallbacks)
    void*                    user;          ///< 透传上下文
    OrkClientOnConnected     on_connected;
    OrkClientOnDisconnected  on_disconnected;
    OrkClientOnPayload       on_payload;
} OrkClientCallbacks;

/* ========================================================================== */
/* 不透明句柄                                                                */
/* ========================================================================== */

/** @brief 客户端实例句柄（不透明）。由 ork_client_create 产出，ork_client_destroy 释放。 */
typedef struct OrkClient_s* OrkClientHandle;

/* ========================================================================== */
/* 便捷初始化宏（C99+ 复合字面量；仅 C 调用方使用）                         */
/* struct_size 已置好，其余字段零初始化，调用方按需覆盖。                   */
/* ========================================================================== */

#define ORK_CLIENT_CONFIG_INIT    ((OrkClientConfig){ .struct_size = sizeof(OrkClientConfig) })
#define ORK_CLIENT_CALLBACKS_INIT ((OrkClientCallbacks){ .struct_size = sizeof(OrkClientCallbacks) })

/**
 * @brief v1（A1.2 之前）OrkClientConfig 布局的字节数（64 位平台）。
 *
 * 由旧头文件编译的调用方会传该 struct_size；实现按 v1 前缀解析
 * （TLS 字段视为零值 = 默认 TLS）。供测试模拟旧调用方使用。
 * 数值由 ork_client_abi.cpp 的 static_assert 钉死（从当前布局推导：
 * v1 末字段 kcp_rx_min_rto_ms 止于 68，向上对齐到结构体对齐 8）。
 */
#define ORK_CLIENT_CONFIG_V1_SIZE 72u

/* ========================================================================== */
/* API 函数                                                                  */
/* ========================================================================== */

/* ---- 生命周期 ---- */

/**
 * @brief 创建客户端实例（不发起连接）。
 * @param config     配置（不可为 NULL；struct_size 必须为 sizeof(OrkClientConfig)
 *                   （v2）或 ORK_CLIENT_CONFIG_V1_SIZE（v1）；KCP 参数与
 *                   TLS 三态字段在此处校验，非法值即拒绝）
 * @param callbacks  回调集合（不可为 NULL；struct_size 必须 = sizeof(OrkClientCallbacks)）
 * @param out_client [out] 成功时写入句柄
 * @return OrkOk 或错误码（失败详情见 ork_client_last_error()）
 *
 * @note TLS 字段（证书文件存在性 / expectedServerName 非空 / 信任锚组合）
 * 在 ork_client_connect 驱动的连接流程中校验（fail closed，无明文回退）；
 * create 仅校验字段值本身（枚举范围 / 三态范围）与构建能力
 * （PlaintextDev 未编入即拒绝）。
 */
OrkError ork_client_create(const OrkClientConfig* config,
                           const OrkClientCallbacks* callbacks,
                           OrkClientHandle* out_client);

/**
 * @brief 发起连接（异步；需周期调用 ork_client_pump_once() 驱动推进）。
 *
 * 重复调用（非 Disconnected 状态，或上一次 connect 尚被 pump 消费前）
 * 返回 OrkErr_AlreadyStarted。断开后可再次 connect（重连）。
 */
OrkError ork_client_connect(OrkClientHandle client);

/**
 * @brief 主动断开（幂等：已断开时为 no-op 成功）。
 *
 * 可在回调内安全调用。手动驱动模式下 OnDisconnected 回调可能在本调用
 * 栈内同步触发（reason 为 OrkDisconnectReason 枚举值）。
 */
OrkError ork_client_disconnect(OrkClientHandle client);

/**
 * @brief 销毁客户端实例（若仍在连接中会先断开；断开引发的 OnDisconnected
 *        不再回调给调用方）。传 NULL 安全无操作。
 *
 * 禁止在回调内调用（会析构自身 → 未定义行为）；请先投递到主循环线程。
 */
void ork_client_destroy(OrkClientHandle client);

/* ---- 驱动 ---- */

/**
 * @brief 驱动一次客户端事件循环（真实执行 io.poll()，非阻塞，非桩）。
 *
 * 必须由调用方在同一线程周期调用（如 Unity 主循环每帧一次），
 * 否则连接流程与收发回调均不会推进。
 *
 * @return OrkOk；client 为空返回 OrkErr_InvalidArg。
 */
OrkError ork_client_pump_once(OrkClientHandle client);

/* ---- 发送 ---- */

/**
 * @brief 发送 opaque 字节。Reliable → KCP，Unreliable → UDP。
 * @param data  size>0 时不可为 NULL；返回后调用方可立即释放
 * @return OrkSendResult（未 Connected → OrkSend_NotConnected）
 */
OrkSendResult ork_client_send(OrkClientHandle client,
                              const uint8_t* data, uint32_t size, OrkChannel channel);

/* ---- 状态查询 ---- */

/** @brief 当前连接状态。client 为空返回 OrkClientState_Disconnected。 */
OrkClientState ork_client_get_state(OrkClientHandle client);

/** @brief 当前连接 ID（Bootstrap 前为 0）。client 为空返回 0。 */
uint32_t ork_client_get_connection_id(OrkClientHandle client);

/* ---- 错误 ---- */

/**
 * @brief 最近一次客户端 API 失败的错误描述（thread_local，NUL 结尾，静态/线程私有存储）。
 *
 * 与 ork_abi.h 的 ork_last_error() 同语义、各自独立（双方分属不同实现
 * 单元，内部缓冲无法合并）；返回指针在**本线程**下一次 ork_client_* 调用前
 * 有效。无错误时返回空串 ""。
 */
const char* ork_client_last_error(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ORK_SDK_C_ORK_CLIENT_ABI_H */
