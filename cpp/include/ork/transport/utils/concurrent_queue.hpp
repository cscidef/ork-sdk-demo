#pragma once
#include <concurrentqueue.h>
#include <utility>

template <typename T>
class ConcurrentQueue
{
public:
    /// Phase 4.2: move 语义 — strand 内 push 避免拷贝
    void Push(T&& value)
    {
        queue_.enqueue(std::move(value));
    }

    void Push(const T& value)
    {
        queue_.enqueue(value);
    }

    template <typename... Args>
    void Emplace(Args&&... args)
    {
        queue_.enqueue(T(std::forward<Args>(args)...));
    }

    bool TryPop(T& v)
    {
        return queue_.try_dequeue(v);
    }

private:
    moodycamel::ConcurrentQueue<T> queue_;
};
