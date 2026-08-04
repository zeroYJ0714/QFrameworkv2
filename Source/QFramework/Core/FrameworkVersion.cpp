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
