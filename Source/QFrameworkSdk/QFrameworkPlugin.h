#pragma once

// 文件职责：定义 Qt 插件元数据使用的稳定 IID。
// 所有主进程模块共用该 IID，具体模块身份由插件 JSON 和框架配置校验。
// “/1.0”是插件接口协议版本；不兼容变更时应提升版本，而不是静默复用旧值。
#define QFRAMEWORK_PLUGIN_IID "org.qframework.Module/1.0"
