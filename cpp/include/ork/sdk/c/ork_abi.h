/**
 * @file ork_abi.h
 * @brief Ork 宿主 C ABI（Phase 5H.1）—— Unity / C# P/Invoke 接入面
 *
 * 设计原则（务必遵守，破坏即破坏 ABI 稳定性）：
 * - 纯 C11：本头文件不依赖任何 C++ 头，可被 C11 编译器独立解析
 *   （门禁：`clang -x c -std=c11 -fsyntax-only ork_abi.h` 退出码 0）。
 * - extern "C" 包裹：C++ 侧包含时自动切换 C 链接。
 * - 零 C++ 类型跨界：无 std::string / template / namespace / 异常穿越边界。
 * - 不透明句柄：OrkHost 为前向声明指针，内部实现不外泄。
 * - 版本化结构体：所有跨边界结构体首字段为 `uint32_t struct_size`，
 *   调用前必须置为 `sizeof(该结构体)`。新增字段只追加在尾部，
 *   旧调用方传入较小的 struct_size 时，实现只读取其已知前缀，
 *   从而在不破坏 ABI 的前提下扩展。
 * - 枚举即契约：本头文件中的枚举值是对外稳定 ABI，**不**镜像内部 C++
 *   枚举编号；桥接实现（ork_abi.cpp）负责双向映射，并用 static_assert
 *   钉死此处数值 + 验证映射全覆盖。
 *
 * 明确不含（属业务/编排语义，由宿主在其上自建）：
 *   PlayerId、Join/Leave、房间状态、gRPC 控制面。
 *
 * 线程契约（与 ork/sdk/host_listener.hpp 一致）：
 * - 所有回调均在 Ork 内部网络 pump 线程触发，实现必须非阻塞。
 * - 禁止在回调内调用 ork_host_stop / ork_host_destroy（会等待 pump 线程
 *   退出，而回调正在该线程上 → 死锁）。需关停时先投递到宿主自己的线程。
 * - ork_host_disconnect 可在回调内安全调用（内部为异步 close，非阻塞）。
 * - on_payload 的 data 为借用指针，仅本次回调期间有效，不得跨调用持有。
 */

#ifndef ORK_SDK_C_ORK_ABI_H
#define ORK_SDK_C_ORK_ABI_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief ABI 版本号（单调递增）。
 *
 * ork_abi_version() 返回此值。调用方可在加载动态库后核对，
 * 确保头文件与库二进制兼容。
 */
#define ORK_ABI_VERSION 1u

/* ========================================================================== */
/* 枚举 —— 对外稳定 ABI（数值由本头文件权威定义，勿与内部 C++ 枚举混淆）    */
/* ========================================================================== */

/**
 * @brief 日志级别。
 *
 * 映射到内部 C++ LogLevel（Trace/Debug/Info/Warn/Error 数值一致；
 * C 侧无 Critical，OrkLog_Off 映射到 C++ Off）。
 */
typedef enum OrkLogLevel
{
    OrkLog_Trace = 0,
    OrkLog_Debug = 1,
    OrkLog_Info  = 2,
    OrkLog_Warn  = 3,
    OrkLog_Error = 4,
    OrkLog_Off   = 5
} OrkLogLevel;

/**
 * @brief 连接链路类型（OrkConnectionInfo.link）。
 *
 * 由内部链路状态 + 是否启用 KCP 推导：
 * Established+KCP→OrkLink_Kcp，Established(无KCP)→OrkLink_Udp，
 * TcpOnly/Disconnected→OrkLink_Tcp。
 */
typedef enum OrkLinkType
{
    OrkLink_Tcp = 0,
    OrkLink_Udp = 1,
    OrkLink_Kcp = 2
} OrkLinkType;

/**
 * @brief 发送通道。数值与内部 C++ ork::sdk::Channel 严格一致。
 */
typedef enum OrkChannel
{
    OrkChannel_Reliable   = 0,  ///< 可靠通道（KCP / TCP fallback）
    OrkChannel_Unreliable = 1   ///< 不可靠通道（UDP）
} OrkChannel;

/**
 * @brief 连接断开原因（对外稳定 ABI）。
 *
 * 与内部 C++ DisconnectReason 编号不同，由桥接层双向映射：
 * - OnDisconnected 回调 / OrkStats.disconnects_by_reason 索引用本枚举；
 * - ork_host_disconnect 入参用本枚举，桥接映射回 C++ DisconnectReason。
 * 内部 CryptoFailed 归并到 OrkDisconnect_ProtocolError；
 * OrkDisconnect_Internal / MaxConnections / Unknown 无内部对应，
 * 主动断开时统一映射为内部 Kicked（宿主发起）。
 */
typedef enum OrkDisconnectReason
{
    OrkDisconnect_PeerClosed       = 0,
    OrkDisconnect_Idle             = 1,
    OrkDisconnect_Kicked           = 2,
    OrkDisconnect_ProtocolError    = 3,
    OrkDisconnect_Internal         = 4,
    OrkDisconnect_BootstrapTimeout = 5,
    OrkDisconnect_MaxConnections   = 6,
    OrkDisconnect_Unknown          = 7
} OrkDisconnectReason;

/**
 * @brief 单次发送 / 广播结果（对外稳定 ABI，内部 SendResult 的收敛子集）。
 *
 * 内部 InvalidArg / NoRoute / CipherFailed / BadState 统一归并为
 * OrkSend_BadState（C ABI 不细分底层失败原因）。
 */
typedef enum OrkSendResult
{
    OrkSend_Ok           = 0,
    OrkSend_TooLarge     = 1,
    OrkSend_NotConnected = 2,
    OrkSend_BadState     = 3,
    OrkSend_QueueFull    = 4
} OrkSendResult;

/**
 * @brief API 调用错误码（生命周期 / 管理 / 查询类函数返回值）。
 */
typedef enum OrkError
{
    OrkOk             = 0,  ///< 成功
    OrkErr_InvalidArg = 1,  ///< 空指针 / struct_size 不符 / 参数非法
    OrkErr_AlreadyStarted = 2,  ///< 重复 start
    OrkErr_NotStarted = 3,  ///< 未 start 即调用运行期 API
    OrkErr_Internal   = 4,  ///< 内部异常（详见 ork_last_error()）
    OrkErr_BadState   = 5   ///< 状态不允许（如连接不存在）
} OrkError;

/* ========================================================================== */
/* 结构体 —— 首字段恒为 struct_size，尾部可扩展                              */
/* ========================================================================== */

/**
 * @brief 宿主配置（POD，无 std::string）。
 *
 * 语义为 0 的字段一律取"默认/禁用"，便于调用方只填关心的项：
 * - pump_rate_hz / kcp_update_rate_hz：0 → 默认 100
 * - idle_timeout_sec：0 → 禁用空闲检测
 * - max_packet_size：0 → 默认 1200（合法区间 [50, 1414]）
 * - bootstrap_ttl_sec：0 → 默认 300
 * - log_path：NULL → 不写日志文件
 *
 * @note Phase 5H.1 限制：
 * - tcp_port / udp_port：内部 TransportConfig 单一 gamePort（TCP bootstrap
 *   与 UDP 游戏流量共用同一端口号）。桥接取 udp_port（为 0 时取 tcp_port）
 *   作为 gamePort；如需 TCP/UDP 异端口，须待后续 Phase 扩展内部配置。
 * - outbound_queue_capacity / inbound_queue_capacity：内部暂无对应可配置项，
 *   当前接受但不生效（保留字段，供后续 ABI 兼容扩展）。
 * - drive_mode：0=内部 pump 线程自驱；1=调用方 ork_host_pump_once 驱动。
 *   Phase 5H.1 恒为内部线程驱动，pump_once 为兼容桩（协作式让出），
 *   完整外部自驱延后到独立 Phase。
 */
typedef struct OrkHostConfig
{
    uint32_t    struct_size;              ///< = sizeof(OrkHostConfig)
    uint16_t    tcp_port;                 ///< TCP bootstrap 端口
    uint16_t    udp_port;                 ///< UDP 游戏端口
    int32_t     enable_kcp;               ///< 0=disabled, 1=enabled
    int32_t     enable_encryption;        ///< 0=disabled, 1=enabled
    uint32_t    bootstrap_ttl_sec;        ///< bootstrap token TTL（0→默认 300）
    uint32_t    pump_rate_hz;             ///< 宿主 pump 频率（0→默认 100）
    uint32_t    kcp_update_rate_hz;       ///< KCP 更新频率（0→默认 100）
    uint32_t    idle_timeout_sec;         ///< 空闲超时秒（0→禁用）
    uint32_t    max_packet_size;          ///< 单包上限（0→默认 1200）
    uint32_t    outbound_queue_capacity;  ///< 出站队列容量（0→默认；5H.1 保留）
    uint32_t    inbound_queue_capacity;   ///< 入站队列容量（0→默认；5H.1 保留）
    uint32_t    drive_mode;               ///< 0=内部pump线程, 1=调用方自驱
    const char* log_path;                 ///< 日志文件路径（NULL=不写文件）
    uint32_t    log_level;                ///< OrkLogLevel 枚举值
} OrkHostConfig;

/**
 * @brief 单条连接只读信息快照。
 *
 * @note Phase 5H.1：内部 IConnectionRegistry 仅提供
 * {id, linkState, lastActiveUs, encryptionActive}。故本结构体中
 * rtt_us / retransmits / bytes_* / packets_* / lost_ppm / send_queue_len /
 * remote_endpoint 暂无数据来源，统一填 0 / 空串；字段已预留，
 * 待内部提供每连接诊断后填充（不破坏 ABI）。
 */
typedef struct OrkConnectionInfo
{
    uint32_t struct_size;          ///< = sizeof(OrkConnectionInfo)
    uint32_t id;                   ///< ConnectionId
    uint32_t link;                 ///< OrkLinkType 枚举值
    int64_t  last_active_us;       ///< 最后活跃时间（微秒，单调时钟）
    uint32_t rtt_us;               ///< 往返时延（微秒）—— 5H.1 恒 0
    uint32_t retransmits;          ///< KCP 重传计数 —— 5H.1 恒 0
    int32_t  encryption_active;    ///< 0/1
    uint64_t bytes_in;             ///< —— 5H.1 恒 0
    uint64_t bytes_out;            ///< —— 5H.1 恒 0
    uint64_t packets_in;           ///< —— 5H.1 恒 0
    uint64_t packets_out;          ///< —— 5H.1 恒 0
    uint32_t lost_ppm;             ///< 丢包率（百万分之）—— 5H.1 恒 0
    uint32_t send_queue_len;       ///< 发送队列长度 —— 5H.1 恒 0
    char     remote_endpoint[64];  ///< 定长 "ip:port"，避免分配 —— 5H.1 空串
} OrkConnectionInfo;

/**
 * @brief 网络统计快照。
 *
 * 数据来源：内部以 ~1Hz 经 OnStats 回调上报，桥接层缓存最近一次快照；
 * ork_host_get_stats() 返回该缓存（最多约 1s 陈旧）。active_connections
 * 额外以注册表实时值刷新。各字段独立采样，非事务一致。
 *
 * disconnects_by_reason 按 OrkDisconnectReason 枚举索引（8 槽）；
 * 内部 6 值经映射归并写入（CryptoFailed→ProtocolError 槽）。
 */
typedef struct OrkStats
{
    uint32_t struct_size;          ///< = sizeof(OrkStats)
    uint64_t pump_tick_count;
    uint64_t pump_overrun_count;
    uint64_t decode_failures;
    uint64_t crypto_fail_aead;
    uint64_t crypto_fail_replay;
    uint64_t crypto_fail_expired;
    uint64_t crypto_fail_malformed;
    uint64_t dropped_too_large;
    uint64_t dropped_queue_full;
    uint64_t inbound_queue_high_water;
    uint64_t outbound_queue_high_water;
    uint32_t active_connections;
    uint32_t disconnects_by_reason[8];  ///< 按 OrkDisconnectReason 索引
    /* KCP 聚合 */
    uint32_t kcp_waitsnd_sum;
    uint32_t kcp_srtt_avg_ms;
    uint64_t kcp_retransmits_total;
    /* 字节 / 包（wire 级累计） */
    uint64_t bytes_in;
    uint64_t bytes_out;
    uint64_t packets_in;
    uint64_t packets_out;
} OrkStats;

/* ========================================================================== */
/* 回调函数指针类型                                                          */
/* ========================================================================== */

/// 连接就绪（TCP bootstrap + UDP 握手完成）
typedef void (*OrkOnConnected)(void* user, uint32_t connection_id);

/// 连接断开。reason 为 OrkDisconnectReason 枚举值
typedef void (*OrkOnDisconnected)(void* user, uint32_t connection_id, uint32_t reason);

/// 收到 opaque 载荷。data 为借用指针，仅本次调用期间有效；channel 为 OrkChannel
typedef void (*OrkOnPayload)(void* user, uint32_t connection_id,
                             const uint8_t* data, uint32_t size, uint32_t channel);

/// 周期性网络统计上报（stats 为借用指针，仅本次调用期间有效）
typedef void (*OrkOnStats)(void* user, const OrkStats* stats);

/**
 * @brief 宿主回调集合。任一回调可为 NULL（表示不关心该事件）。
 *
 * user 为透传上下文，原样回传给每个回调。
 */
typedef struct OrkHostCallbacks
{
    uint32_t         struct_size;      ///< = sizeof(OrkHostCallbacks)
    void*            user;             ///< 透传上下文
    OrkOnConnected   on_connected;
    OrkOnDisconnected on_disconnected;
    OrkOnPayload     on_payload;
    OrkOnStats       on_stats;
} OrkHostCallbacks;

/* ========================================================================== */
/* 不透明句柄                                                                */
/* ========================================================================== */

/** @brief 宿主实例句柄（不透明）。由 ork_host_create 产出，ork_host_destroy 释放。 */
typedef struct OrkHost_s* OrkHost;

/* ========================================================================== */
/* 便捷初始化宏（C99+ 复合字面量；仅 C 调用方使用）                         */
/* struct_size 已置好，其余字段零初始化，调用方按需覆盖。                   */
/* ========================================================================== */

#define ORK_HOST_CONFIG_INIT     ((OrkHostConfig){ .struct_size = sizeof(OrkHostConfig) })
#define ORK_HOST_CALLBACKS_INIT  ((OrkHostCallbacks){ .struct_size = sizeof(OrkHostCallbacks) })
#define ORK_CONNECTION_INFO_INIT ((OrkConnectionInfo){ .struct_size = sizeof(OrkConnectionInfo) })
#define ORK_STATS_INIT           ((OrkStats){ .struct_size = sizeof(OrkStats) })

/* ========================================================================== */
/* API 函数                                                                  */
/* ========================================================================== */

/* ---- 生命周期 ---- */

/**
 * @brief 创建宿主实例。
 * @param config     配置（不可为 NULL；struct_size 必须 = sizeof(OrkHostConfig)）
 * @param callbacks  回调集合（不可为 NULL；struct_size 必须 = sizeof(OrkHostCallbacks)）
 * @param out_host   [out] 成功时写入句柄
 * @return OrkOk 或错误码（失败详情见 ork_last_error()）
 */
OrkError ork_host_create(const OrkHostConfig* config,
                         const OrkHostCallbacks* callbacks,
                         OrkHost* out_host);

/** @brief 启动宿主（开始监听 + 内部 pump 线程）。重复启动返回 OrkErr_AlreadyStarted。 */
OrkError ork_host_start(OrkHost host);

/** @brief 停止宿主（阻塞等待内部 pump 线程退出）。未启动返回 OrkErr_NotStarted。 */
OrkError ork_host_stop(OrkHost host);

/** @brief 销毁宿主实例（若仍运行会先 Stop）。传 NULL 安全无操作。 */
void ork_host_destroy(OrkHost host);

/* ---- 驱动（drive_mode=1 时由调用方周期调用） ---- */

/**
 * @brief 驱动一次宿主 pump。
 *
 * Phase 5H.1：内部恒为独立线程驱动，本函数为兼容桩——做一次协作式让出
 * （约 1ms），使调用方"循环 pump_once 等待事件"的写法可用；回调仍由内部
 * 线程触发。完整"调用方独占驱动"（不起内部线程）延后到独立 Phase。
 *
 * @return OrkOk；host 为空返回 OrkErr_InvalidArg；未启动返回 OrkErr_NotStarted。
 */
OrkError ork_host_pump_once(OrkHost host);

/* ---- 发送 ---- */

/**
 * @brief 向单个连接发送 opaque 字节。
 * @param data  size>0 时不可为 NULL；返回后调用方可立即释放
 * @return OrkSendResult
 */
OrkSendResult ork_host_send(OrkHost host, uint32_t connection_id,
                            const uint8_t* data, uint32_t size, OrkChannel channel);

/**
 * @brief 向所有活跃连接广播 opaque 字节。
 * @return OrkSendResult（无连接时视为 vacuous 成功 OrkSend_Ok）
 */
OrkSendResult ork_host_broadcast(OrkHost host,
                                 const uint8_t* data, uint32_t size, OrkChannel channel);

/* ---- 连接管理 ---- */

/** @brief 主动断开指定连接（可在回调内调用）。连接不存在/已断开返回 OrkErr_BadState。 */
OrkError ork_host_disconnect(OrkHost host, uint32_t connection_id, OrkDisconnectReason reason);

/** @brief 当前活跃连接数。host 为空返回 0。 */
uint32_t ork_host_connection_count(OrkHost host);

/** @brief 查询单条连接信息。连接不存在返回 OrkErr_BadState。 */
OrkError ork_host_get_connection_info(OrkHost host, uint32_t connection_id,
                                      OrkConnectionInfo* out_info);

/* ---- 统计 ---- */

/** @brief 获取最近一次统计快照（~1Hz 缓存，最多约 1s 陈旧）。 */
OrkError ork_host_get_stats(OrkHost host, OrkStats* out_stats);

/* ---- 错误 ---- */

/**
 * @brief 最近一次失败的错误描述（thread_local，NUL 结尾，静态/线程私有存储）。
 *
 * 返回指针在**本线程**下一次 ork_* 调用前有效。无错误时返回空串 ""。
 */
const char* ork_last_error(void);

/* ---- ABI 版本 ---- */

/** @brief 返回 ABI 版本号（= ORK_ABI_VERSION，当前为 1）。 */
uint32_t ork_abi_version(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ORK_SDK_C_ORK_ABI_H */
