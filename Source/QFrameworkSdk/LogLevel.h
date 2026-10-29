// 【阅读导引】统一日志严重程度。
// 阅读顺序：Debug 用于定位细节，Info 记录正常过程，Warning 表示异常但尚可继续，Error 表示操作失败。
// 需要分清：级别用于显示和过滤，不是控制程序是否返回成功的开关。
// 这是声明/类型入口：先看 public 接口和字段注释；复杂执行过程再到同名 .cpp 或指定实现文件阅读。

#pragma once

// 文件职责：集中定义日志严重级别，Logger 和模块 SDK 共用同一枚举。

namespace qframework
{
enum class LogLevel
{
    // 仅用于调试细节，生产环境通常最容易被过滤。
    Debug,
    // 正常运行信息。
    Info,
    // 可恢复问题或降级行为。
    Warning,
    // 需要关注的失败，但不一定终止进程。
    Error
};
}
