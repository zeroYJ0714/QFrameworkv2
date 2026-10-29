// 【阅读导引】把另一个进程的原生窗口放进 Qt 面板。
// 阅读顺序：子进程就绪前显示 placeholder；拿到窗口句柄后包装 QWindow 并放入容器；尺寸变化再通知监督器。
// 需要分清：原生窗口属于子进程，包装器属于宿主。窗口句柄失效后不能继续当 QWidget 使用；释放包装不应误当作直接销毁子进程。
// 这是声明/类型入口：先看 public 接口和字段注释；复杂执行过程再到同名 .cpp 或指定实现文件阅读。

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
    /// @brief 构造宿主控件：装配堆叠布局与占位标签，初始只显示「等待子进程窗口」占位页。
    /// @param parent 父控件，通常是对应模块的 ManagedDockWidget，生命周期交由 Qt 管理；可为空。
    /// @return 无（构造函数不返回值）。
    explicit ProcessWindowHost(QWidget* parent = nullptr);
    /// @brief 析构前先解除嵌入容器，避免宿主销毁后 Qt 仍持有悬空的原生窗口句柄。
    /// @return 无（析构函数不返回值）。
    ~ProcessWindowHost() override;

    /// @brief 把子进程上报的原生窗口句柄包装成 QWindow 容器，并切换为当前显示页。
    /// @param windowId 子进程上报的原生窗口句柄（HWND 数值）；0 视为无效，直接失败。
    /// @param errorMessage 可选输出参数：失败时写入中文原因，成功时不改写；可为空。
    /// @return 成功嵌入返回 true；句柄为 0，或 Qt 无法包装句柄、无法创建容器时返回 false，且不保留无效句柄。
    bool attachWindow(quintptr windowId, QString* errorMessage = nullptr);
    /// @brief 清除已嵌入窗口并切回占位页，用于进程未启动、断开、故障或重启时说明原因。
    /// @param detail 占位页显示的说明文字；传空串时回落到默认的「等待子进程窗口」。
    /// @return 无。
    void showPlaceholder(const QString& detail);
    /// @brief 查询当前是否已成功嵌入子进程窗口容器。
    /// @return 嵌入容器已创建返回 true；仅有裸句柄而容器创建失败时返回 false。
    bool hasEmbeddedWindow() const;

signals:
    /// @brief 宿主客户区尺寸稳定后发射，由 MainWindow 转成 resizeWindow 控制帧下发给子进程。
    /// @param size 稳定后的宿主客户区尺寸；宽高均为正，未嵌入窗口时不会发射。
    /// @return 无（信号不返回值）。
    void clientSizeChanged(const QSize& size);

protected:
    /// @brief 覆写尺寸变化处理：同步嵌入容器几何，并重新安排一次客户区尺寸上报。
    /// @param event 尺寸变化事件，先转交 QWidget::resizeEvent；仅在本次回调期间有效。
    /// @return 无。
    void resizeEvent(QResizeEvent* event) override;
    /// @brief 覆写显示处理：显示时重新对齐嵌入容器几何，并安排一次客户区尺寸上报。
    /// @param event 显示事件，先转交 QWidget::showEvent；仅在本次回调期间有效。
    /// @return 无。
    void showEvent(QShowEvent* event) override;
    /// @brief 给出宿主控件的推荐尺寸，用固定值，避免外部窗口的历史尺寸把 Dock 撑大。
    /// @return 固定推荐尺寸 320x240；无论是否已嵌入窗口都返回该值。
    QSize sizeHint() const override;
    /// @brief 给出宿主控件的最小推荐尺寸，防止 Dock 被压缩到无法显示。
    /// @return 固定最小尺寸 180x120，与构造函数中 setMinimumSize 的取值一致。
    QSize minimumSizeHint() const override;

private:
    /// @brief 安排一次延迟的客户区尺寸上报；50 ms 内的多次调用只保留最后一次，避免连续拖动时每个 resize 都产生 IPC 帧。
    /// @return 无；未嵌入窗口时不安排上报。
    void scheduleClientSizeNotification();
    /// @brief 定时器到期后把最终客户区尺寸经 clientSizeChanged 上报给 MainWindow。
    /// @return 无；未嵌入窗口或客户区尺寸非正时不发信号。
    void flushClientSizeNotification();

    /// @brief 从堆叠布局移除容器，立即解除原生嵌入，再延后回收容器和 Qt 包装对象。
    /// @return 无；未嵌入窗口时只把外部窗口指针清空。
    void clearEmbeddedWindow();

    QStackedLayout* stackedLayout_; ///< 占位标签与嵌入容器共用的堆叠布局；本控件持有，随 QWidget 父子关系销毁。
    QLabel* placeholderLabel_; ///< 子进程窗口就绪前显示的占位标签，文案由 showPlaceholder 更新，居中且自动换行。
    QWidget* windowContainer_; ///< 由 createWindowContainer 产出的嵌入容器；非空即代表嵌入成功，也是嵌入状态的唯一判据。
    QWindow* foreignWindow_; ///< 子进程 HWND 的 Qt 包装对象；解除嵌入后由旧容器持有并回收，此指针立即清空。
    QTimer* resizeTimer_; ///< 50 ms 单次定时器，用于合并连续 resize 事件后的客户区尺寸上报。
};
}
