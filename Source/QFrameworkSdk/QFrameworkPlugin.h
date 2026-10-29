// 【阅读导引】约定 Qt 插件识别标识。
// 阅读顺序：插件通过元数据声明身份，PluginManager 用相同 IID 检查加载对象是否符合约定。
// 需要分清：IID 不等于每个业务模块的 moduleId；不同插件共享接口标识，但必须有自己的稳定模块 ID。
// 这是声明/类型入口：先看 public 接口和字段注释；复杂执行过程再到同名 .cpp 或指定实现文件阅读。

#pragma once

// 文件职责：定义 Qt 插件元数据使用的稳定 IID。
// 所有主进程模块共用该 IID，具体模块身份由插件 JSON 和框架配置校验。
// “/1.0”是插件接口协议版本；不兼容变更时应提升版本，而不是静默复用旧值。
#define QFRAMEWORK_PLUGIN_IID "org.qframework.Module/1.0"
