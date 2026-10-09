/**
 * @file network_pump.hpp
 * @brief Phase 4.3: asio strand 单线程化调度器（kcpUpdateRate 驱动）
 *
 * NetworkPump 把 transport 层所有状态变更收归到单一 asio::strand，
 * 消除 mutex，实现无锁单线程热路径。
 *
 * 核心能力：
 * - strand 私有 timer 以 kcpUpdateRate（默认 100Hz=10ms）驱动 tickFn_（KCP Update/Recv）；
 *   宿主层（pumpRate）分频由 NetworkTransport 的累加器在 tickFn_ 内完成
 * - Dispatch(fn): fire-and-forget post 到 strand（strand 内直接执行）
 * - Invoke(fn): 带返回值的同步桥接（外部线程阻塞等结果）
 * - RunningInThisThread(): 判断当前是否已在 strand 内
 */

#pragma once

#include <asio.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <utility>

class NetworkPump
{
public:
    /**
     * @brief 构造 NetworkPump
     * @param io       asio io_context（由外部持有）
     * @param updateIntervalMs  timer tick 间隔（ms）= 1000/kcpUpdateRate，驱动 KCP Update/Recv 等周期任务
     */
    NetworkPump(asio::io_context& io, uint32_t updateIntervalMs);

    ~NetworkPump();

    // 禁止拷贝/移动
    NetworkPump(const NetworkPump&) = delete;
    NetworkPump& operator=(const NetworkPump&) = delete;

    /// 启动 strand timer（在 io_.run() 之前或之后均可调用）
    void Start();

    /// 停止：设 stopped 标志，在 strand 内取消 timer
    void Stop();

    /**
     * @brief 提交工作到 strand（fire-and-forget）
     *
     * 如果当前已在 strand 内则直接执行 fn()；
     * 否则 asio::post(strand_, fn) 异步投递。
     */
    template <typename F>
    void Dispatch(F&& fn)
    {
        if (RunningInThisThread())
        {
            fn();
        }
        else
        {
            asio::post(strand_, std::forward<F>(fn));
        }
    }

    /**
     * @brief 带返回值的同步桥接（外部线程调用，阻塞等结果）
     *
     * 如果当前已在 strand 内则直接调用返回结果；
     * 否则创建 packaged_task + future，post 到 strand，等待完成。
     *
     * 超时保护：等待期间循环检查 stopped_ 标志。一旦 pump 被 Stop（io 可能
     * 随即 io_.stop()，导致已 post 但尚未执行的任务被丢弃），立即返回 R{}，
     * 绝不无限阻塞。这保证 Stop 与并发 Invoke 不会死锁。
     *
     * @tparam F  可调用对象，返回 R
     * @return R  fn 的返回值；stopped 时返回 R{}
     */
    template <typename F>
    auto Invoke(F&& fn) -> decltype(fn())
    {
        using R = decltype(fn());

        if (RunningInThisThread())
        {
            return fn();
        }

        if (stopped_.load(std::memory_order_acquire))
        {
            return R{};
        }

        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(fn));
        auto future = task->get_future();

        asio::post(strand_, [task]() { (*task)(); });

        // 循环等待：任务就绪则立即返回（无额外延迟）；
        // 若 stopped_ 在等待期间被置位且任务被 io_.stop() 丢弃，则及时退出，避免死锁。
        while (future.wait_for(std::chrono::milliseconds(20)) != std::future_status::ready)
        {
            if (stopped_.load(std::memory_order_acquire))
            {
                return R{};
            }
        }
        return future.get();
    }

    /// 查询当前线程是否在 strand 内执行
    bool RunningInThisThread() const;

    /// 是否已停止（拒绝新命令）
    bool IsStopped() const;

    /// 获取 strand 引用（供绑定 async 操作用）
    asio::strand<asio::io_context::executor_type>& GetStrand() { return strand_; }
    const asio::strand<asio::io_context::executor_type>& GetStrand() const { return strand_; }

    /// 设置 timer tick 回调（由 NetworkTransport 在 Start 前设置）
    void SetTickFunction(std::function<void()> fn) { tickFn_ = std::move(fn); }

private:
    void ScheduleNext();
    void OnTick(const asio::error_code& ec);

    asio::io_context& io_;
    asio::strand<asio::io_context::executor_type> strand_;
    asio::steady_timer timer_;
    uint32_t intervalMs_;
    std::atomic<bool> stopped_{false};
    std::function<void()> tickFn_;

    /// 下一个 tick 的绝对截止点（steady_clock）。以 expires_at 严格 += intervalMs_
    /// 推进，使 handler 执行耗时不累积成周期漂移，保证真实频率 ~1000/intervalMs_ Hz。
    /// 仅在 strand 内读写（Start 首次赋值亦在 io_.run 之前），无需原子。
    std::chrono::steady_clock::time_point nextDeadline_{};

    /// Phase 4.5：标记首次 ScheduleNext（Start 触发的对齐不计为 overrun）。
    /// 仅在 strand 内读写。
    bool firstSchedule_ = true;
};
