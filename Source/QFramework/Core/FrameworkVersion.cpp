// 文件职责：提供框架版本字符串和版本比较辅助常量。
// 本文件没有运行时状态、线程或资源所有权，只返回编译期确定的值，供启动日志和兼容性
// 检查使用；它不负责读取配置，也不负责升级协议。
#include "QFrameworkVersion.h"

// 版本字符串编译进 QFramework.dll，供测试、诊断和插件兼容性提示读取。

namespace qframework
{
const char* frameworkVersion()
{
    // 字符串字面量具有静态存储期，调用方不需要也不能释放返回指针。
    return "0.1.0-baseline";
}
}
