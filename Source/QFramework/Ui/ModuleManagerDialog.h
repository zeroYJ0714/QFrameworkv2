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
    /// @brief 按模块配置快照一次性建好表格行和每行的“显示界面/重启子进程”按钮；此后只刷新状态，不重建行。
    /// @param modules 模块配置快照，决定表格行数、每行类型和可用操作；构造完成后不再读取配置。
    /// @param parent 父窗口，仅用于建立 Qt 所有权关系；对话框关闭时由 Qt 释放，允许为空。
    /// @return 无（构造函数不返回值）。
    explicit ModuleManagerDialog(const QVector<ModuleConfig>& modules,
                                 QWidget* parent = nullptr);

    /// @brief 更新指定模块“状态”列的文本与悬停详情，并据此刷新该行重启按钮的可用性。
    /// @param moduleId 稳定模块 ID，用于定位表格行。
    /// @param state 状态文本，由 MainWindow 依据生命周期信号给出，同时参与重启按钮的门禁判断。
    /// @param detail 悬停提示里的补充说明（例如启动失败原因），没有额外信息时传空字符串。
    /// @return 无；moduleId 不在表格中（配置已移除）时静默返回。
    void setModuleState(const QString& moduleId,
                        const QString& state,
                        const QString& detail);
    /// @brief 按界面是否就绪切换指定模块“显示界面”按钮的可点击状态。
    /// @param moduleId 稳定模块 ID。
    /// @param available 真实 QWidget 或子进程窗口是否已准备好；false 时禁用按钮，避免触发无效请求。
    /// @return 无；非 UI 类型模块没有显示按钮，直接忽略。
    void setUiAvailable(const QString& moduleId, bool available);
    /// @brief 记录指定模块是否有重启操作正在进行，并立即刷新该行重启按钮。
    /// @param moduleId 稳定模块 ID。
    /// @param busy true 表示异步 stop/restart 尚未结束，按钮保持禁用直到最终信号到达。
    /// @return 无；该模块没有重启按钮时忽略。
    void setRestartBusy(const QString& moduleId, bool busy);

signals:
    // 控制请求由 MainWindow 接收，保持 UI 展示层和运行时管理层解耦。
    /// @brief 用户点击某行的“显示界面”按钮时发射，请求把该模块的界面带到前台。
    /// @param moduleId 被点击按钮所属模块的稳定 ID。
    /// @return 无。
    void showModuleRequested(const QString& moduleId);
    /// @brief 用户点击某行的“重启子进程”按钮时发射，请求重启该模块的子进程。
    /// @param moduleId 被点击按钮所属模块的稳定 ID。
    /// @return 无。
    void restartModuleRequested(const QString& moduleId);

private slots:
    /// @brief 响应某行“显示界面”按钮的点击，从 sender 的动态属性取出 moduleId 后发射 showModuleRequested。
    /// @return 无；sender 为空或缺少 moduleId 属性时不发射信号。
    void onShowButtonClicked();
    /// @brief 响应某行“重启子进程”按钮的点击，取出 moduleId 后发射 restartModuleRequested，不在槽内等待子进程。
    /// @return 无；sender 为空或缺少 moduleId 属性时不发射信号。
    void onRestartButtonClicked();

private:
    /// @brief 把模块类型枚举转换成与 INI 配置一致的稳定文本，便于用户对照配置文件。
    /// @param type 待转换的模块类型。
    /// @return "InProcessUi"、"InProcessNonUi"、"ProcessUi"、"ProcessNonUi" 之一；未匹配的类型按 ProcessNonUi 返回。
    QString moduleTypeText(ModuleType type) const;
    /// @brief 综合配置启用状态、重启是否进行中和生命周期是否处于过渡态，刷新该行重启按钮的可用性。
    /// @param moduleId 稳定模块 ID。
    /// @return 无；该模块没有重启按钮时直接返回。
    void updateRestartButton(const QString& moduleId);

    QTableWidget* tableWidget_; ///< 对话框唯一的表格控件，构造时创建，Qt 父对象为本对话框。
    QHash<QString, int> rows_; ///< moduleId → 表格行号，把状态更新定位到具体行。
    QHash<QString, QToolButton*> showButtons_; ///< moduleId → 该行的“显示界面”按钮；只有 UI 类型模块有。
    QHash<QString, QToolButton*> restartButtons_; ///< moduleId → 该行的“重启子进程”按钮；只有子进程类型模块有。
    QHash<QString, QString> moduleStates_; ///< moduleId → 最近一次上报的状态文本，用于识别 Starting/Stopping/Restarting 过渡态。
    QHash<QString, bool> restartBusy_; ///< moduleId → 是否正在重启，过渡态期间阻止重复提交。
};
}
