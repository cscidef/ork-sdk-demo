/*
KCP Reliable Channel (替代 TCP 的可靠传输)
- 技能释放
- 道具触发
- 碰撞确认
- 比赛开始/结束
- 结算结果
- 房间内重要事件

UDP Raw Channel (保持现有实现)
- 高频位置同步
- 车辆速度/方向
- 摄像头姿态
- 心跳
*/

#pragma once

#include "ork/transport/transport_kcp/kcp_runtime.hpp"

#include <asio.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <ikcp.h>
#include <vector>

using UdpSocket = asio::ip::udp::socket;
using UdpEndpoint = asio::ip::udp::endpoint;

/**
 * @brief KCP 输出回调类型（Phase 4.2: 替代直接 socket send_to）
 *
 * 由 NetworkTransport 设置，将 KCP 产生的 datagram 入队到
 * UdpTransportServer 的 strand 私有发送队列。
 */
using KcpOutputHandler = std::function<void(std::vector<uint8_t>&& data, const UdpEndpoint& dest)>;

/**
 * @brief KCP会话封装类（Phase 4.2: strand 私有，无 mutex）
 * 
 * 将KCP可靠传输协议封装为独立会话对象，每个客户端连接对应一个KcpSession实例。
 * KCP负责：分片、重传、排序、ACK确认、拥塞控制
 * 
 * Phase 4.2 变更：
 * - 删除内部 mutex_（所有操作在 NetworkPump strand 内串行执行）
 * - OutputCallback 不再直接 send_to，改为调用 KcpOutputHandler 入队
 * 
 * 使用流程：
 * 1. 创建：KcpSession(conv, outputHandler, endpoint, kcpConfig, mtu)
 * 2. 发送：Send(data, len) - 业务层调用，KCP自动处理可靠性
 * 3. 接收：Recv(buffer, maxLen) - 从KCP内部缓冲区取已排序的完整消息
 * 4. 驱动：Update() - 内部使用 MonotonicMs32() 统一时基
 * 5. 输入：Input(raw_udp_data, len) - UDP层收到原始包时喂给KCP
 */
class KcpSession {
private:
    ikcpcb* kcp_;
    uint32_t connection_id_;
    KcpOutputHandler outputHandler_;
    UdpEndpoint remote_endpoint_;
    
public:
    /**
     * @brief 创建KCP会话
     * @param conv 会话ID（类似connectionId，两端必须一致）
     * @param outputHandler 输出回调（入队到 UDP 发送队列）
     * @param remote_endpoint 对端UDP地址（从ConnectionManager获取）
     * @param config KCP协议参数（nodelay/窗口/RTO等）
     * @param mtu 最大传输单元（字节），来自 NetworkConfig::max_packet_size
     */
    KcpSession(uint32_t conv, KcpOutputHandler outputHandler, const UdpEndpoint& remote_endpoint,
               const ork::transport::KcpConfig& config, int mtu);
    
    /**
     * @brief 析构函数：释放KCP控制块
     */
    ~KcpSession();
    
    // 禁止拷贝和移动
    KcpSession(const KcpSession&) = delete;
    KcpSession& operator=(const KcpSession&) = delete;
    KcpSession(KcpSession&&) = delete;
    KcpSession& operator=(KcpSession&&) = delete;
    
    /**
     * @brief 发送数据（业务层调用，strand 内）
     * @param data 数据指针
     * @param len 数据长度
     * @return 0成功，<0失败
     */
    int Send(const uint8_t* data, size_t len);
    
    /**
     * @brief 接收数据（从KCP内部缓冲区取出已排好序的完整消息，strand 内）
     * @param buffer 接收缓冲区
     * @param maxLen 缓冲区最大长度
     * @return 实际接收字节数，<0表示无数据或错误
     */
    int Recv(uint8_t* buffer, size_t maxLen);
    
    /**
     * @brief 定时驱动（使用 MonotonicMs32() 统一时基，strand 内）
     */
    void Update();
    
    /**
     * @brief 收到UDP原始包时喂给KCP（strand 内）
     * @param raw_udp_data UDP原始数据包指针
     * @param len 数据包长度
     */
    void Input(const uint8_t* raw_udp_data, size_t len);
    
    /**
     * @brief KCP底层输出回调（静态函数）
     * 
     * Phase 4.2: 不再直接 send_to，改为调用 outputHandler_ 入队
     */
    static int OutputCallback(const char* buf, int len, ikcpcb* kcp, void* user);
    
    /**
     * @brief 获取连接ID
     */
    uint32_t GetConnectionId() const { return connection_id_; }
    
    /**
     * @brief 获取只读 KCP 诊断采样（strand 内调用，无锁）
     */
    ork::transport::KcpDiagnostics GetDiagnostics() const;
};
