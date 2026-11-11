/**
 * @file ork_export.h
 * @brief Ork C ABI 的跨平台符号可见性宏（Phase W1.1）
 *
 * 设计原则：
 * - 纯 C：不依赖任何 C++ 头，可被 C11 编译器独立解析。
 * - 零体积：只定义宏，无声明、无 include，可被任意 C ABI 头包含。
 * - 双开关：构建动态库（Windows ork_shared / Apple ork_bundle）时定义
 *   OrkExport_EXPORTS，则该头产出「导出」语义；消费者只得到「导入」
 *   （Windows）或默认可见性（其他平台）。
 * - 静态消费开关：链接静态 SDK（ork_static → libork_static.a /
 *   ork_static.lib）时定义 ORK_STATIC，Windows 侧不展开 __declspec，
 *   即普通外部声明。否则 Windows 消费者会得到 dllimport 而无法从静态
 *   归档解析符号（LNK2019 / LNK4217）。
 * - 开关优先级：ORK_STATIC > OrkExport_EXPORTS > 默认（导入）。
 *   三者互斥使用，不得同时定义 ORK_STATIC 与 OrkExport_EXPORTS。
 *
 * 用法：
 *   OrkExport uint32_t OrkHost_Create(...);
 *
 * 平台语义：
 * - Windows（_WIN32 / _WIN64）：
 *     ORK_STATIC            → 无属性（静态链接，普通外部符号）
 *     OrkExport_EXPORTS     → __declspec(dllexport)（构建 DLL）
 *     其他（默认）           → __declspec(dllimport)（消费 DLL）
 * - 其他平台（macOS / Linux）：__attribute__((visibility("default")))，
 *   与是否静态链接无关（ORK_STATIC 在该分支不改变语义）。
 *   注：Apple .bundle 侧符号收敛仍由 -Wl,-exported_symbols_list
 *   （source/core/bundle/ork_exports.txt）控制；本宏不替代该名单，
 *   仅保证函数在 -fvisibility=hidden 编译单元中仍具备默认可见性。
 */

#ifndef ORK_SDK_C_ORK_EXPORT_H
#define ORK_SDK_C_ORK_EXPORT_H

#if defined(_WIN32) || defined(_WIN64)
  #if defined(ORK_STATIC)
    /* 静态 SDK：不展开 dllimport / dllexport，保持普通外部声明 */
    #define OrkExport
  #elif defined(OrkExport_EXPORTS)
    #define OrkExport __declspec(dllexport)
  #else
    #define OrkExport __declspec(dllimport)
  #endif
#else
  #define OrkExport __attribute__((visibility("default")))
#endif

#endif /* ORK_SDK_C_ORK_EXPORT_H */
