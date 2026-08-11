#pragma once

// 文件职责：以只读表格展示配置中的全部模块，并把“显示界面/重启子进程”
// 转换成带 moduleId 的信号；对话框本身不直接操作插件或 QProcess。

#include <QDialog>
#include <QHash>
#include <QVector>

#include "FrameworkConfig.h"
#include "QFrameworkGlobal.h"

class QTableWidget;
class QToolButton;

namespace qframework
{
class QFRAMEWORK_EXPORT ModuleManagerDialog : public QDialog
{
    Q_OBJECT

public:
    // modules 是构造时快照，每个模块对应一行和一组可用操作。
    /// @brief 创建 ModuleManagerDialog 对象并初始化其基础状态。
    /// @param modules 模块配置列表。
    /// @param parent Qt 父对象或父控件；用于建立所有权关系，允许为空。
    /// @return 无（构造函数不返回值）。
    explicit ModuleManagerDialog(const QVector<ModuleConfig>& modules,
                                 QWidget* parent = nullptr); ///< `parent` 对应的对象指针；所有权和线程归属见本类文件级说明。

    // 由 MainWindow 根据生命周期信号更新状态文本和详细提示。
    /// @brief 设置 `setModuleState` 对应的框架操作。
    /// @param moduleId 稳定模块 ID。
    /// @param state 状态值。
    /// @param detail 状态或故障详情。
    /// @return 无。
    void setModuleState(const QString& moduleId,
                        const QString& state,
                        const QString& detail);
    // 只有真实 QWidget/子进程窗口已准备好时才允许“显示”。
    /// @brief 设置 `setUiAvailable` 对应的框架操作。
    /// @param moduleId 稳定模块 ID。
    /// @param available 目标可用状态。
    /// @return 无。
    void setUiAvailable(const QString& moduleId, bool available);
    // 异步 stop/restart 期间锁住对应按钮，最终信号到达后恢复。
    /// @brief 设置 `setRestartBusy` 对应的框架操作。
    /// @param moduleId 稳定模块 ID。
    /// @param busy 目标忙碌状态。
    /// @return 无。
    void setRestartBusy(const QString& moduleId, bool busy);

signals:
    // 控制请求由 MainWindow 接收，保持 UI 展示层和运行时管理层解耦。
    /// @brief 显示 `showModuleRequested` 对应的框架操作。
    /// @param moduleId 稳定模块 ID。
    /// @return 无。
    void showModuleRequested(const QString& moduleId);
    /// @brief 重启 `restartModuleRequested` 对应的框架操作。
    /// @param moduleId 稳定模块 ID。
    /// @return 无。
    void restartModuleRequested(const QString& moduleId);

private slots:
    // 两个按钮槽从 sender 的 moduleId 属性恢复目标模块。
    /// @brief 处理 `onShowButtonClicked` 对应的框架操作。
    /// @return 无。
    void onShowButtonClicked();
    /// @brief 处理 `onRestartButtonClicked` 对应的框架操作。
    /// @return 无。
    void onRestartButtonClicked();

private:
    /// @brief 执行 `moduleTypeText` 所定义的类职责。
    /// @param type 传给该操作的 `type` 参数；取值应符合声明类型和函数用途。
    /// @return 返回对应 Qt 值或容器的安全副本。
    QString moduleTypeText(ModuleType type) const;
    /// @brief 更新 `updateRestartButton` 对应的框架操作。
    /// @param moduleId 稳定模块 ID。
    /// @return 无。
    void updateRestartButton(const QString& moduleId);

    // rows_ 加速状态定位；showButtons_ 用于动态启用 UI 操作。
    QTableWidget* tableWidget_; ///< `tableWidget_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    QHash<QString, int> rows_; ///< `rows_` 对应的数值配置、计数或时间状态。
    QHash<QString, QToolButton*> showButtons_; ///< `showButtons_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    QHash<QString, QToolButton*> restartButtons_; ///< `restartButtons_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    QHash<QString, QString> moduleStates_; ///< 保存 `moduleStates` 相关值的 Qt 容器。
    QHash<QString, bool> restartBusy_; ///< `restartBusy_` 对应的布尔状态标志。
};
}
