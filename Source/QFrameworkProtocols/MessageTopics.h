#pragma once

// 示例主题及其直接序列化的 Proto 载荷；通用消息总线不解析载荷。

// 发送：图像采集模块 -> 图像处理模块、图像显示模块。
// 作用：传递未经算法处理的原始图像帧。
// Proto：直接序列化 ImageFrame。
#define QFRAMEWORK_IMAGE_RAW "QFRAMEWORK_IMAGE_RAW"
// 发送：图像处理模块 -> 图像显示模块、图像存储模块。
// 作用：传递算法处理后的图像及其来源帧序号。
// Proto：直接序列化 ProcessedImage。
#define QFRAMEWORK_IMAGE_PROCESSED "QFRAMEWORK_IMAGE_PROCESSED"
// 发送：业务模块 -> 日志显示 UI 模块。
// 作用：把结构化日志内容发送到界面显示。
// Proto：直接序列化 LogDisplayMessage。
#define QFRAMEWORK_LOG_DISPLAY "QFRAMEWORK_LOG_DISPLAY"
// 发送：业务模块或进程监督模块 -> 状态显示/监控模块。
// 作用：通知模块当前的生命周期状态和状态说明。
// Proto：直接序列化 ModuleStatus。
#define QFRAMEWORK_STATUS "QFRAMEWORK_STATUS"
