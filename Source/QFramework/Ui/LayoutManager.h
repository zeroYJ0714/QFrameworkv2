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
enum class LayoutActivation
{
    KeepCurrent,
    Activate
};

class QFRAMEWORK_EXPORT LayoutManager
{
public:
    // mainWindow 是借用指针，必须在 LayoutManager 之后销毁。
    /// @brief 创建 LayoutManager 对象并初始化其基础状态。
    /// @param mainWindow 主窗口借用指针，同时作为 Dock 的 Qt 父对象。
    /// @return 无（构造函数不返回值）。
    explicit LayoutManager(QMainWindow* mainWindow);

    // 只注册当前可用的模块 Dock；卸载/释放 UI 时应对应 unregister。
    /// @brief 注册 `registerModuleDock` 对应的框架操作。
    /// @param moduleId 稳定模块 ID。
    /// @param dockWidget 传给该操作的 `dockWidget` 参数；取值应符合声明类型和函数用途。
    /// @return 无。
    void registerModuleDock(const QString& moduleId, QDockWidget* dockWidget);
    /// @brief 注销 `unregisterModuleDock` 对应的框架操作。
    /// @param moduleId 稳定模块 ID。
    /// @return 无。
    void unregisterModuleDock(const QString& moduleId);

    // save 使用 QSaveFile 原子提交，失败不会破坏原布局文件。
    /// @brief 保存 `saveLayout` 对应的框架操作。
    /// @param filePath 文件路径。
    /// @param requestedVisibility 传给该操作的 `requestedVisibility` 参数；取值应符合声明类型和函数用途。
    /// @param errorMessage 可选错误说明输出参数。
    /// @param activation 传给该操作的 `activation` 参数；取值应符合声明类型和函数用途。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool saveLayout(const QString& filePath,
                    const QHash<QString, bool>& requestedVisibility,
                    QString* errorMessage = nullptr,
                    LayoutActivation activation = LayoutActivation::Activate); ///< 保存 `activation` 对应的对象状态或配置值。
    // load 先完整校验；Qt restore 失败会回滚 geometry/state/visibility。
    /// @brief 加载 `loadLayout` 对应的框架操作。
    /// @param filePath 文件路径。
    /// @param requestedVisibility 传给该操作的 `requestedVisibility` 参数；取值应符合声明类型和函数用途。
    /// @param errorMessage 可选错误说明输出参数。
    /// @param unavailableModuleIds 传给该操作的 `unavailableModuleIds` 参数；取值应符合声明类型和函数用途。
    /// @param legacyVisibilitySemantics 传给该操作的 `legacyVisibilitySemantics` 参数；取值应符合声明类型和函数用途。
    /// @param activation 传给该操作的 `activation` 参数；取值应符合声明类型和函数用途。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool loadLayout(const QString& filePath,
                    QHash<QString, bool>* requestedVisibility,
                    QString* errorMessage = nullptr,
                    QStringList* unavailableModuleIds = nullptr,
                    bool* legacyVisibilitySemantics = nullptr,
                    LayoutActivation activation = LayoutActivation::Activate); ///< 保存 `activation` 对应的对象状态或配置值。

    // 只读取并校验文件，不改变主窗口、显示意图或活动路径。
    /// @brief 执行 `validateLayoutFile` 所定义的类职责。
    /// @param filePath 文件路径。
    /// @param errorMessage 可选错误说明输出参数。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool validateLayoutFile(const QString& filePath,
                            QString* errorMessage = nullptr) const; ///< `errorMessage` 对应的对象指针；所有权和线程归属见本类文件级说明。

    // 最近一次成功保存或加载的绝对路径。
    /// @brief 执行 `activeFilePath` 所定义的类职责。
    /// @return 返回对应 Qt 值或容器的安全副本。
    QString activeFilePath() const;

private:
    /// @brief 读取 `readLayoutFile` 对应的框架操作。
    /// @param filePath 文件路径。
    /// @param geometry 传给该操作的 `geometry` 参数；取值应符合声明类型和函数用途。
    /// @param state 状态值。
    /// @param requestedVisibility 传给该操作的 `requestedVisibility` 参数；取值应符合声明类型和函数用途。
    /// @param unavailableModuleIds 传给该操作的 `unavailableModuleIds` 参数；取值应符合声明类型和函数用途。
    /// @param legacyVisibilitySemantics 传给该操作的 `legacyVisibilitySemantics` 参数；取值应符合声明类型和函数用途。
    /// @param errorMessage 可选错误说明输出参数。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool readLayoutFile(const QString& filePath,
                        QByteArray* geometry,
                        QByteArray* state,
                        QHash<QString, bool>* requestedVisibility,
                        QStringList* unavailableModuleIds,
                        bool* legacyVisibilitySemantics,
                        QString* errorMessage) const; ///< `const` 对应的对象指针；所有权和线程归属见本类文件级说明。
    /// @brief 执行 `validateFilePath` 所定义的类职责。
    /// @param filePath 文件路径。
    /// @param errorMessage 可选错误说明输出参数。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool validateFilePath(const QString& filePath,
                          QString* errorMessage) const; ///< `const` 对应的对象指针；所有权和线程归属见本类文件级说明。

    // moduleDocks_ 保存借用指针，Dock 的实际所有权属于 MainWindow。
    QMainWindow* mainWindow_; ///< `mainWindow_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    QHash<QString, QDockWidget*> moduleDocks_; ///< `moduleDocks_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    QString activeFilePath_; ///< 保存 `activeFilePath` 对应的对象状态或配置值。
};
}
