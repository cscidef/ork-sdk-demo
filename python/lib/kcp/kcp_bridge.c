#include "ikcp.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// 单进程 Python 压测客户端并发 KCP 会话上限（须 >= --clients）
#define KCP_BRIDGE_MAX_SESSIONS 256
#define KCP_BRIDGE_OUT_CAP 2048

typedef struct {
    ikcpcb* kcp;
    uint8_t out[KCP_BRIDGE_OUT_CAP];
    int out_len;
    int in_use;
} KcpBridgeSession;

static KcpBridgeSession g_sessions[KCP_BRIDGE_MAX_SESSIONS];

// 统一的会话获取：sid 越界或槽位未占用（in_use==0）时返回 NULL。
// 所有对外数据面 API 复用此三重校验，避免重复展开同样的 guard。
static KcpBridgeSession* get_session(int sid)
{
    if (sid < 0 || sid >= KCP_BRIDGE_MAX_SESSIONS || !g_sessions[sid].in_use)
        return NULL;
    return &g_sessions[sid];
}

static int bridge_output(const char* buf, int len, ikcpcb* kcp, void* user)
{
    const int sid = (int)(intptr_t)user;
    // 仅做 sid 越界校验，不检查 in_use：本回调由 KCP 内部在 ikcp_flush 期间
    // 触发，此时会话必然处于占用状态；且 user 是创建时冻结的 sid，检查
    // in_use 反而会在释放竞态下漏掉正在 flush 的合法输出。
    if (sid < 0 || sid >= KCP_BRIDGE_MAX_SESSIONS)
        return -1;

    KcpBridgeSession* session = &g_sessions[sid];
    if (len <= 0 || len > KCP_BRIDGE_OUT_CAP)
        return -1;

    memcpy(session->out, buf, (size_t)len);
    session->out_len = len;
    return 0;
}

static int alloc_session_id(void)
{
    for (int i = 0; i < KCP_BRIDGE_MAX_SESSIONS; ++i)
    {
        if (!g_sessions[i].in_use)
            return i;
    }
    return -1;
}

int kcp_bridge_create(uint32_t conv)
{
    const int sid = alloc_session_id();
    if (sid < 0)
        return -1;

    KcpBridgeSession* session = &g_sessions[sid];
    memset(session, 0, sizeof(*session));

    session->kcp = ikcp_create(conv, (void*)(intptr_t)sid);
    if (!session->kcp)
        return -1;

    ikcp_setoutput(session->kcp, bridge_output);
    ikcp_nodelay(session->kcp, 2, 10, 2, 1);
    ikcp_wndsize(session->kcp, 128, 128);
    ikcp_setmtu(session->kcp, 1200);
    session->kcp->stream = 0;
    session->kcp->rx_minrto = 10;
    session->in_use = 1;
    return sid;
}

void kcp_bridge_release(int sid)
{
    // 仅做 sid 越界校验，不检查 in_use：release 是拆销路径，必须能清理一个
    // 无论当前 in_use 为何值的槽位（幂等释放）；若在此检查 in_use，重复释放
    // 或释放一个已被 memset 归零的槽位将变成静默 no-op，反而掩盖调用方错误。
    if (sid < 0 || sid >= KCP_BRIDGE_MAX_SESSIONS)
        return;

    KcpBridgeSession* session = &g_sessions[sid];
    if (session->kcp)
    {
        ikcp_release(session->kcp);
        session->kcp = NULL;
    }
    memset(session, 0, sizeof(*session));
}

int kcp_bridge_send(int sid, const char* data, int len)
{
    KcpBridgeSession* session = get_session(sid);
    if (!session)
        return -1;

    const int sent = ikcp_send(session->kcp, data, len);
    if (sent >= 0)
        ikcp_flush(session->kcp);
    return sent;
}

void kcp_bridge_input(int sid, const char* data, int len)
{
    KcpBridgeSession* session = get_session(sid);
    if (!session)
        return;

    ikcp_input(session->kcp, data, len);
}

void kcp_bridge_update(int sid, uint32_t current_ms)
{
    KcpBridgeSession* session = get_session(sid);
    if (!session)
        return;

    ikcp_update(session->kcp, current_ms);
}

int kcp_bridge_recv(int sid, char* buffer, int max_len)
{
    KcpBridgeSession* session = get_session(sid);
    if (!session)
        return -1;

    return ikcp_recv(session->kcp, buffer, max_len);
}

int kcp_bridge_peeksize(int sid)
{
    KcpBridgeSession* session = get_session(sid);
    if (!session)
        return -1;

    return ikcp_peeksize(session->kcp);
}

int kcp_bridge_poll_output(int sid, char* buffer, int max_len)
{
    KcpBridgeSession* session = get_session(sid);
    if (!session)
        return -1;

    if (session->out_len <= 0)
        return 0;

    const int n = session->out_len > max_len ? max_len : session->out_len;
    memcpy(buffer, session->out, (size_t)n);
    session->out_len = 0;
    return n;
}
