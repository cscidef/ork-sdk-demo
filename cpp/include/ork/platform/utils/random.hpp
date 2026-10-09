#pragma once
/**
 * @file random.hpp
 * @brief 非安全随机数（std::mt19937）—— 仅限测试 / 模拟 / 抖动等非安全用途
 *
 * Bootstrap Security A0.1 收口：本引擎为 Mersenne Twister（种子来自
 * std::random_device），生成流可预测，**不是 CSPRNG**。任何安全敏感值
 * （SessionToken / key / nonce / epoch 等）禁止使用本类，必须改用
 * ork::platform::SecureRandom（libsodium randombytes_buf，见
 * secure_random.hpp）。当前生产代码已无调用点，仅保留给非安全场景。
 */
#include <mutex>
#include <random>

class Random
{
public:
    template <typename T>
    static T Range(T min, T max)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::uniform_int_distribution<T> dist(min, max - 1);
        return dist(engine_);
    }

    static void SetSeed(uint32_t seed)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        engine_.seed(seed);
    }

    static std::mt19937 GetSeed()
    {
        return engine_;
    }

private:
    static std::mt19937 engine_;
    static std::mutex mutex_;
};