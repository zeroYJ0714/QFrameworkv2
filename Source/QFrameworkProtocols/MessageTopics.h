#pragma once

// 文件职责：集中维护跨模块共享的主题名称。
// 主题值与宏名保持一致，避免不同 DLL/EXE 手写字符串造成拼写分叉。
// 修改主题名会影响发布/订阅契约，应同时更新配置和测试。
// 原始图像帧主题，通常使用 Latest 策略承载高频帧流。
#define QFRAMEWORK_IMAGE_RAW "QFRAMEWORK_IMAGE_RAW"
// 算法处理后的图像主题，消费者可按 source_sequence 对齐原帧。
#define QFRAMEWORK_IMAGE_PROCESSED "QFRAMEWORK_IMAGE_PROCESSED"
// 结构化日志显示主题，供 UI 模块订阅。
#define QFRAMEWORK_LOG_DISPLAY "QFRAMEWORK_LOG_DISPLAY"
// 模块生命周期状态主题，通常使用 Reliable 策略。
#define QFRAMEWORK_STATUS "QFRAMEWORK_STATUS"
