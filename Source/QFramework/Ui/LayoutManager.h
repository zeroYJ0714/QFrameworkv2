#pragma once

// 文件职责：保存和恢复主窗口 geometry、Dock state 及每个模块 Dock 的可见性。
// 文件格式是带版本号的 JSON，二进制 Qt 状态使用 Base64 存储。

#include <QHash>
#include <QString>
#include <QStringList>

#include "QFrameworkGlobal.h"

class QDockWidget;
class QMainWindow;

namespace qframework
{
class QFRAMEWORK_EXPORT LayoutManager
{
public:
    // mainWindow 是借用指针，必须在 LayoutManager 之后销毁。
    explicit LayoutManager(QMainWindow* mainWindow);

    // 只注册当前可用的模块 Dock；卸载/释放 UI 时应对应 unregister。
    void registerModuleDock(const QString& moduleId, QDockWidget* dockWidget);
    void unregisterModuleDock(const QString& moduleId);

    // save 使用 QSaveFile 原子提交，失败不会破坏原布局文件。
    bool saveLayout(const QString& filePath,
                    const QHash<QString, bool>& requestedVisibility,
                    QString* errorMessage = nullptr);
    // load 先完整校验；Qt restore 失败会回滚 geometry/state/visibility。
    bool loadLayout(const QString& filePath,
                    QHash<QString, bool>* requestedVisibility,
                    QString* errorMessage = nullptr,
                    QStringList* unavailableModuleIds = nullptr,
                    bool* legacyVisibilitySemantics = nullptr);

    // 最近一次成功保存或加载的绝对路径。
    QString activeFilePath() const;

private:
    bool validateFilePath(const QString& filePath,
                          QString* errorMessage) const;

    // moduleDocks_ 保存借用指针，Dock 的实际所有权属于 MainWindow。
    QMainWindow* mainWindow_;
    QHash<QString, QDockWidget*> moduleDocks_;
    QString activeFilePath_;
};
}
