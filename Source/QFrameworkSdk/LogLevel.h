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
