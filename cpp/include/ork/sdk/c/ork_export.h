/**
 * @file ork_export.h
 * @brief Ork C ABI 的跨平台符号可见性宏（Phase W1.1）
 *
 * 设计原则：
 * - 纯 C：不依赖任何 C++ 头，可被 C11 编译器独立解析。
 * - 零体积：只定义宏，无声明、无 include，可被任意 C ABI 头包含。
 * - 单一开关：构建动态库（Windows ork_shared / Apple ork_bundle）时定义
 *   OrkExport_EXPORTS，则该头产出「导出」语义；消费者只得到「导入」
 *   （Windows）或默认可见性（其他平台）。
 *
 * 用法：
 *   OrkExport uint32_t OrkHost_Create(...);
 *
 * 平台语义：
 * - Windows（_WIN32 / _WIN64）：__declspec(dllexport) / __declspec(dllimport)
 * - 其他平台（macOS / Linux）：__attribute__((visibility("default")))
 *   注：Apple .bundle 侧符号收敛仍由 -Wl,-exported_symbols_list
 *   （source/core/bundle/ork_exports.txt）控制；本宏不替代该名单，
 *   仅保证函数在 -fvisibility=hidden 编译单元中仍具备默认可见性。
 */

#ifndef ORK_SDK_C_ORK_EXPORT_H
#define ORK_SDK_C_ORK_EXPORT_H

#if defined(_WIN32) || defined(_WIN64)
  #ifdef OrkExport_EXPORTS
    #define OrkExport __declspec(dllexport)
  #else
    #define OrkExport __declspec(dllimport)
  #endif
#else
  #define OrkExport __attribute__((visibility("default")))
#endif

#endif /* ORK_SDK_C_ORK_EXPORT_H */
