/**
 * @file ork_test_assert.h
 * @brief 三个 C/C++ 冒烟测试共用的极简断言框架（不依赖任何第三方）
 *
 * 用法
 * ----
 * @code{.c}
 *   #include "demo_support_c.h"          // 提供 ORK_BANNER_RULE（可选）
 *
 *   // 失败时顺带把 SDK 的错误串打出来 —— 在 include 之前定义即可覆盖默认空实现
 *   #define ORK_TEST_PRINT_LAST_ERROR() \
 *       printf("         ork_last_error() = \"%s\"\n", ork_last_error())
 *   #include "ork_test_assert.h"
 *
 *   int main(void) {
 *       CHECK_EQ(ork_abi_version(), ORK_ABI_VERSION, "ABI 版本一致");
 *       ORK_TEST_SUMMARY("test_c_abi_host");
 *   }
 * @endcode
 *
 * 设计要点
 * --------
 *   - CHECK_EQ 的两个实参**只求值一次**（先存进局部常量再比较）。测试里大量
 *     传入带副作用的调用（ork_host_start(host)、ork_client_connect(client) …），
 *     双重求值会让第二次返回 AlreadyStarted / NotStarted 而误判失败。
 *   - g_checks / g_failures 是文件作用域的 static，所以自定义断言（如
 *     test_c_abi_client.c 的 EXPECT_REJECT）可以直接累加它们，共用同一份汇总。
 *   - 每次断言后 fflush(stdout)：保持 stdout 与 stderr 的交错顺序可读。
 */

#ifndef ORK_TEST_ASSERT_H
#define ORK_TEST_ASSERT_H

#include <stdio.h>

/* 分隔线：调用方若已引入 demo_support_c.h 就复用它的定义，避免字面量重复 */
#ifndef ORK_BANNER_RULE
#define ORK_BANNER_RULE "============================================================="
#endif

/* 失败时打印 SDK 错误串。默认空实现 —— 在 include 本头文件之前 #define 覆盖。 */
#ifndef ORK_TEST_PRINT_LAST_ERROR
#define ORK_TEST_PRINT_LAST_ERROR() ((void)0)
#endif

static int g_checks   = 0;
static int g_failures = 0;

/** @brief 布尔断言 */
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        ++g_checks;                                                            \
        if (cond) {                                                            \
            printf("  [ok]   %s\n", (msg));                                    \
        } else {                                                               \
            ++g_failures;                                                      \
            printf("  [FAIL] %s   (line %d)\n", (msg), __LINE__);              \
            ORK_TEST_PRINT_LAST_ERROR();                                       \
        }                                                                      \
        fflush(stdout);                                                        \
    } while (0)

/** @brief 相等断言；实参只求值一次（见文件头「设计要点」） */
#define CHECK_EQ(actual, expected, msg)                                        \
    do {                                                                       \
        const long long ork_got_  = (long long)(actual);                       \
        const long long ork_want_ = (long long)(expected);                     \
        ++g_checks;                                                            \
        if (ork_got_ == ork_want_) {                                           \
            printf("  [ok]   %s (= %lld)\n", (msg), ork_got_);                 \
        } else {                                                               \
            ++g_failures;                                                      \
            printf("  [FAIL] %s: got %lld, expected %lld   (line %d)\n",       \
                   (msg), ork_got_, ork_want_, __LINE__);                      \
            ORK_TEST_PRINT_LAST_ERROR();                                       \
        }                                                                      \
        fflush(stdout);                                                        \
    } while (0)

/** @brief 前置条件不满足（拿不到端口 / create 失败）时提前收尾 */
#define ORK_TEST_ABORT()                                                       \
    do {                                                                       \
        printf("\nFAIL (%d/%d)\n", g_failures, g_checks);                      \
        fflush(stdout);                                                        \
        return 1;                                                              \
    } while (0)

/** @brief 打印汇总并返回进程退出码（0 = 全过，1 = 有失败） */
#define ORK_TEST_SUMMARY(name)                                                 \
    do {                                                                       \
        printf("\n" ORK_BANNER_RULE "\n");                                     \
        if (g_failures == 0) {                                                 \
            printf("PASS: %s  (%d/%d checks)\n",                               \
                   (name), g_checks - g_failures, g_checks);                   \
            printf(ORK_BANNER_RULE "\n");                                      \
            fflush(stdout);                                                    \
            return 0;                                                          \
        }                                                                      \
        printf("FAIL: %s  (%d failures / %d checks)\n",                        \
               (name), g_failures, g_checks);                                  \
        printf(ORK_BANNER_RULE "\n");                                          \
        fflush(stdout);                                                        \
        return 1;                                                              \
    } while (0)

#endif /* ORK_TEST_ASSERT_H */
