#pragma once
/**
 * @file endpoint_key.hpp
 * @brief Phase 4.4: UDP endpoint 定长可哈希键（ConnectionManager 反向索引用）
 *
 * 动机：FindByUdpEndpoint 原为 O(N) 遍历连接表并逐个比较 asio endpoint；
 * 高并发下每个 raw UDP 包（KCP 路由）都要遍历全表，成为热路径瓶颈。
 * 本键把 endpoint 压成定长 POD（无 std::string / 无堆分配），配合
 * std::unordered_map 实现 O(1) 反查。
 *
 * 字节布局：
 * - IPv4：addrBytes[0..3] 有效，addrLen=4；
 * - IPv6：addrBytes[0..15] 有效，addrLen=16。
 *
 * port 以主机序存储——EndpointKey 仅为进程内部哈希键，绝不序列化上线，
 * 字节序对相等性/哈希结果无影响（同一 endpoint 必然产生同一键）。
 *
 * 相等语义与 asio::ip::udp::endpoint 的 operator== 对齐（同族 + 同地址字节 +
 * 同端口）。注：asio 的 IPv6 相等还比较 scope_id；当前 UDP 服务器绑定 IPv4
 * （asio::ip::udp::v4()），运行时不会出现 IPv6 link-local scope_id，故本键
 * 不含 scope_id。若将来启用 IPv6 link-local，需在此补充 scope_id 字段。
 */
#include <asio.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ork::connection {

/**
 * @brief UDP endpoint 的定长可哈希键（POD，无堆分配）
 */
struct EndpointKey
{
    std::uint8_t  addrBytes[16]{};  ///< IPv4: [0..3]；IPv6: [0..15]
    std::uint8_t  addrLen{0};       ///< 4（IPv4）或 16（IPv6）
    std::uint16_t port{0};          ///< 端口（主机序；内部键，不上线）

    /**
     * @brief 从 asio UDP endpoint 构造键（无堆分配）
     */
    static EndpointKey From(const asio::ip::udp::endpoint& ep)
    {
        EndpointKey k{};
        const asio::ip::address addr = ep.address();
        if (addr.is_v4())
        {
            const auto bytes = addr.to_v4().to_bytes();  // std::array<uint8_t, 4>
            std::memcpy(k.addrBytes, bytes.data(), bytes.size());
            k.addrLen = static_cast<std::uint8_t>(bytes.size());
        }
        else
        {
            const auto bytes = addr.to_v6().to_bytes();  // std::array<uint8_t, 16>
            std::memcpy(k.addrBytes, bytes.data(), bytes.size());
            k.addrLen = static_cast<std::uint8_t>(bytes.size());
        }
        k.port = ep.port();
        return k;
    }

    bool operator==(const EndpointKey& o) const
    {
        return addrLen == o.addrLen
            && port == o.port
            && std::memcmp(addrBytes, o.addrBytes, addrLen) == 0;
    }
};

/**
 * @brief EndpointKey 的 FNV-1a 哈希（仅覆盖有效字节）
 *
 * 混合顺序：addrLen → port(2B) → addrBytes[0..addrLen)，
 * 保证 IPv4（4B）与 IPv6（16B）地址空间不互相碰撞。
 */
struct EndpointKeyHash
{
    std::size_t operator()(const EndpointKey& k) const
    {
        std::size_t h = 1469598103934665603ull;  // FNV offset basis
        const auto mix = [&h](std::uint8_t b)
        {
            h ^= static_cast<std::size_t>(b);
            h *= 1099511628211ull;               // FNV prime
        };
        mix(k.addrLen);
        mix(static_cast<std::uint8_t>(k.port & 0xFFu));
        mix(static_cast<std::uint8_t>((k.port >> 8) & 0xFFu));
        for (std::uint8_t i = 0; i < k.addrLen; ++i)
            mix(k.addrBytes[i]);
        return h;
    }
};

}  // namespace ork::connection
