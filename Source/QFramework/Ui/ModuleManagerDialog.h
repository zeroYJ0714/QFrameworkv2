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
    explicit ModuleManagerDialog(const QVector<ModuleConfig>& modules,
                                 QWidget* parent = nullptr);

    // 由 MainWindow 根据生命周期信号更新状态文本和详细提示。
    void setModuleState(const QString& moduleId,
                        const QString& state,
                        const QString& detail);
    // 只有真实 QWidget/子进程窗口已准备好时才允许“显示”。
    void setUiAvailable(const QString& moduleId, bool available);
    // 异步 stop/restart 期间锁住对应按钮，最终信号到达后恢复。
    void setRestartBusy(const QString& moduleId, bool busy);

signals:
    // 控制请求由 MainWindow 接收，保持 UI 展示层和运行时管理层解耦。
    void showModuleRequested(const QString& moduleId);
    void restartModuleRequested(const QString& moduleId);

private slots:
    // 两个按钮槽从 sender 的 moduleId 属性恢复目标模块。
    void onShowButtonClicked();
    void onRestartButtonClicked();

private:
    QString moduleTypeText(ModuleType type) const;
    void updateRestartButton(const QString& moduleId);

    // rows_ 加速状态定位；showButtons_ 用于动态启用 UI 操作。
    QTableWidget* tableWidget_;
    QHash<QString, int> rows_;
    QHash<QString, QToolButton*> showButtons_;
    QHash<QString, QToolButton*> restartButtons_;
    QHash<QString, QString> moduleStates_;
    QHash<QString, bool> restartBusy_;
};
}
