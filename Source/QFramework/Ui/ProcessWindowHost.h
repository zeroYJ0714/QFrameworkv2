#pragma once

// 文件职责：把子进程提供的原生窗口句柄包装成 QWindow，并嵌入主窗口 Dock。
// 当子进程尚未启动、故障或重启时，改为显示文字占位页。

#include <QSize>
#include <QWidget>

#include "QFrameworkGlobal.h"

class QLabel;
class QResizeEvent;
class QShowEvent;
class QStackedLayout;
class QTimer;
class QWindow;

namespace qframework
{
class QFRAMEWORK_EXPORT ProcessWindowHost : public QWidget
{
    Q_OBJECT

public:
    // stackedLayout_ 同一时间只显示占位页或窗口容器之一。
    /// @brief 创建 ProcessWindowHost 对象并初始化其基础状态。
    /// @param parent Qt 父对象或父控件；用于建立所有权关系，允许为空。
    /// @return 无（构造函数不返回值）。
    explicit ProcessWindowHost(QWidget* parent = nullptr);
    /// @brief 销毁 ProcessWindowHost 对象并释放其拥有的资源。
    /// @return 无（析构函数不返回值）。
    ~ProcessWindowHost() override;

    // 成功后主机显示嵌入窗口；失败保留可读错误且不持有无效句柄。
    /// @brief 挂接 `attachWindow` 对应的框架操作。
    /// @param windowId 传给该操作的 `windowId` 参数；取值应符合声明类型和函数用途。
    /// @param errorMessage 可选错误说明输出参数。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool attachWindow(quintptr windowId, QString* errorMessage = nullptr);
    /// @brief 显示 `showPlaceholder` 对应的框架操作。
    /// @param detail 状态或故障详情。
    /// @return 无。
    void showPlaceholder(const QString& detail);
    /// @brief 检查 `hasEmbeddedWindow` 对应的框架操作。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool hasEmbeddedWindow() const;

signals:
    // 宿主客户区尺寸稳定后通知 MainWindow，再由它发送 resizeWindow 控制帧。
    /// @brief 执行 `clientSizeChanged` 所定义的类职责。
    /// @param size 传给该操作的 `size` 参数；取值应符合声明类型和函数用途。
    /// @return 无。
    void clientSizeChanged(const QSize& size);

protected:
    /// @brief 调整 `resizeEvent` 对应的框架操作。
    /// @param event Qt 事件对象；仅在当前回调期间有效。
    /// @return 无。
    void resizeEvent(QResizeEvent* event) override;
    /// @brief 显示 `showEvent` 对应的框架操作。
    /// @param event Qt 事件对象；仅在当前回调期间有效。
    /// @return 无。
    void showEvent(QShowEvent* event) override;
    /// @brief 执行 `sizeHint` 所定义的类职责。
    /// @return 返回对应 Qt 值或容器的安全副本。
    QSize sizeHint() const override;
    /// @brief 执行 `minimumSizeHint` 所定义的类职责。
    /// @return 返回对应 Qt 值或容器的安全副本。
    QSize minimumSizeHint() const override;

private:
    // 连续拖动只保留最后一次尺寸，避免每个 resize 事件都产生 IPC 帧。
    /// @brief 执行 `scheduleClientSizeNotification` 所定义的类职责。
    /// @return 无。
    void scheduleClientSizeNotification();
    /// @brief 执行 `flushClientSizeNotification` 所定义的类职责。
    /// @return 无。
    void flushClientSizeNotification();

    // 释放 Qt 容器；createWindowContainer 会连同其 QWindow 包装对象一起管理。
    /// @brief 清理 `clearEmbeddedWindow` 对应的框架操作。
    /// @return 无。
    void clearEmbeddedWindow();

    QStackedLayout* stackedLayout_; ///< `stackedLayout_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    QLabel* placeholderLabel_; ///< `placeholderLabel_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    QWidget* windowContainer_; ///< `windowContainer_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    QWindow* foreignWindow_; ///< `foreignWindow_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    QTimer* resizeTimer_; ///< `resizeTimer_` 对应的对象指针；所有权和线程归属见本类文件级说明。
};
}
