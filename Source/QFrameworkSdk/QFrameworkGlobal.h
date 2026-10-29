// 【阅读导引】控制 DLL 符号导入导出。
// 阅读顺序：编译库本身时导出函数，编译调用库的程序时导入函数；宏由工程预处理定义决定。
// 需要分清：这只影响编译链接，不启动线程、不创建对象。库和调用方宏配置相反会造成未解析符号等链接错误。
// 这是声明/类型入口：先看 public 接口和字段注释；复杂执行过程再到同名 .cpp 或指定实现文件阅读。

#pragma once

// 文件职责：统一 Windows DLL 的导出/导入宏。编译 QFramework 本体时导出，
// 其他项目包含 SDK 时导入；非 Windows 平台不需要额外修饰。

#if defined(_WIN32)
#  if defined(QFRAMEWORK_BUILD)
    // QFramework.dll 自己编译时，把公开符号导出到 DLL。
#    define QFRAMEWORK_EXPORT __declspec(dllexport)
#  else
    // 插件和应用链接 QFramework.dll 时，从 DLL 导入公开符号。
#    define QFRAMEWORK_EXPORT __declspec(dllimport)
#  endif
#  if !defined(QFRAMEWORK_PROTOCOLS_EXPORT)
#    if defined(QFRAMEWORK_PROTOCOLS_BUILD)
      // QFrameworkProtocols.dll 的构建侧导出协议版本和生成接口。
#      define QFRAMEWORK_PROTOCOLS_EXPORT __declspec(dllexport)
#    else
      // 使用协议 DLL 的项目导入这些符号。
#      define QFRAMEWORK_PROTOCOLS_EXPORT __declspec(dllimport)
#    endif
#  endif
#else
#  define QFRAMEWORK_EXPORT
#  if !defined(QFRAMEWORK_PROTOCOLS_EXPORT)
#    define QFRAMEWORK_PROTOCOLS_EXPORT
#  endif
#endif
