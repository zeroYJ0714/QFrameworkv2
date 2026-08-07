#pragma once

// 原始图像帧主题，通常使用 Latest 策略承载高频帧流。
#define QFRAMEWORK_IMAGE_RAW "QFRAMEWORK_IMAGE_RAW"
// 算法处理后的图像主题，消费者可按 source_sequence 对齐原帧。
#define QFRAMEWORK_IMAGE_PROCESSED "QFRAMEWORK_IMAGE_PROCESSED"
// 结构化日志显示主题，供 UI 模块订阅。
#define QFRAMEWORK_LOG_DISPLAY "QFRAMEWORK_LOG_DISPLAY"
// 模块生命周期状态主题，通常使用 Reliable 策略。
#define QFRAMEWORK_STATUS "QFRAMEWORK_STATUS"
