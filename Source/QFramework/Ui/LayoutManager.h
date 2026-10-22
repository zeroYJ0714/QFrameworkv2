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
    KeepCurrent, ///< 不更新 activeFilePath_，本次保存或加载不改变当前活动布局记录。
    Activate ///< 成功后把本次文件的绝对路径记入 activeFilePath_。
};

class QFRAMEWORK_EXPORT LayoutManager
{
public:
    /// @brief 只记录主窗口借用指针；此时尚未登记任何模块 Dock，也没有活动布局路径。
    /// @param mainWindow 主窗口借用指针，所有权归调用方，生命周期必须长于本对象。
    /// @return 无（构造函数不返回值）。
    explicit LayoutManager(QMainWindow* mainWindow);

    /// @brief 登记模块 Dock 的借用指针，供保存和恢复时按 moduleId 找回该 Dock。
    /// @param moduleId 模块的稳定标识，为空时本次登记被忽略。
    /// @param dockWidget Dock 借用指针，所有权仍属主窗口；为空时本次登记被忽略。
    /// @return 无；moduleId 为空或 dockWidget 为空时不会写入任何映射。
    void registerModuleDock(const QString& moduleId, QDockWidget* dockWidget);

    /// @brief 把主窗口 geometry、Qt Dock state 和各模块显示意图写成版本化 JSON，用 QSaveFile 原子提交，失败不会破坏原布局文件。
    /// @param filePath 目标文件路径，扩展名必须为 .qflayout，否则直接失败。
    /// @param requestedVisibility 各模块的用户显示意图，按 moduleId 索引；缺失的模块按隐藏写入。
    /// @param errorMessage 可选输出参数，失败时写入中文原因，可为 nullptr。
    /// @param activation 为 Activate 时把本次保存的绝对路径记为活动布局，KeepCurrent 则保持不变。
    /// @return 写入并原子提交成功返回 true；路径非法、主窗口为空或打开/写入/提交失败返回 false。
    bool saveLayout(const QString& filePath,
                    const QHash<QString, bool>& requestedVisibility,
                    QString* errorMessage = nullptr,
                    LayoutActivation activation = LayoutActivation::Activate);
    /// @brief 先完整校验布局文件，再把其中的 geometry 和 Dock state 恢复到主窗口，并按文件记录的用户意图显示各模块 Dock。
    /// @param filePath 待加载的布局文件路径，扩展名必须为 .qflayout。
    /// @param requestedVisibility 可选输出参数，写入文件中各模块的显示意图，可为 nullptr。
    /// @param errorMessage 可选输出参数，失败时写入中文原因，可为 nullptr。
    /// @param unavailableModuleIds 可选输出参数，写入文件中出现但当前未注册的模块 ID，可为 nullptr。
    /// @param legacyVisibilitySemantics 可选输出参数，文件未声明 userIntent 语义时写入 true，可为 nullptr。
    /// @param activation 为 Activate 时把本次加载的绝对路径记为活动布局，KeepCurrent 则保持不变。
    /// @return 校验通过且 Qt 接受 geometry/state 返回 true；校验失败或 Qt 拒绝该布局（此时已回滚到调用前状态）返回 false。
    bool loadLayout(const QString& filePath,
                    QHash<QString, bool>* requestedVisibility,
                    QString* errorMessage = nullptr,
                    QStringList* unavailableModuleIds = nullptr,
                    bool* legacyVisibilitySemantics = nullptr,
                    LayoutActivation activation = LayoutActivation::Activate);

    /// @brief 只解析并校验布局文件是否可用，不修改主窗口、不改变显示意图，也不更新活动路径。
    /// @param filePath 待校验的布局文件路径，扩展名必须为 .qflayout。
    /// @param errorMessage 可选输出参数，失败时写入中文原因，可为 nullptr。
    /// @return 文件可读且 JSON、版本、Base64 编码、模块元数据全部合法返回 true，否则返回 false。
    bool validateLayoutFile(const QString& filePath,
                            QString* errorMessage = nullptr) const;

    /// @brief 查询最近一次以 Activate 保存或加载成功时记下的布局文件路径。
    /// @return 活动布局的绝对路径副本；从未成功保存或加载过时返回空字符串。
    QString activeFilePath() const;

private:
    /// @brief 读取布局文件并完成大小、JSON、版本、Base64 与模块可见性的全部校验；只解析不应用，绝不触碰主窗口。
    /// @param filePath 待读取的布局文件路径，扩展名必须为 .qflayout。
    /// @param geometry 可选输出参数，写入 Base64 解码后的窗口几何字节，可为 nullptr。
    /// @param state 可选输出参数，写入 Base64 解码后的 Qt Dock 状态字节，可为 nullptr。
    /// @param requestedVisibility 可选输出参数，写入各模块的显示意图，可为 nullptr。
    /// @param unavailableModuleIds 可选输出参数，写入文件中出现但当前未注册的模块 ID，可为 nullptr。
    /// @param legacyVisibilitySemantics 可选输出参数，文件未声明 userIntent 语义时写入 true，可为 nullptr。
    /// @param errorMessage 可选输出参数，失败时写入中文原因，可为 nullptr。
    /// @return 全部校验通过返回 true；文件不可读、为空或超过 16 MB、JSON 或格式版本非法时返回 false。
    bool readLayoutFile(const QString& filePath,
                        QByteArray* geometry,
                        QByteArray* state,
                        QHash<QString, bool>* requestedVisibility,
                        QStringList* unavailableModuleIds,
                        bool* legacyVisibilitySemantics,
                        QString* errorMessage) const;
    /// @brief 校验布局路径非空且扩展名为 .qflayout，避免误读或覆盖任意文件。
    /// @param filePath 待校验的路径，只检查字符串本身，不要求文件已存在。
    /// @param errorMessage 可选输出参数，失败时写入中文原因，可为 nullptr。
    /// @return 路径非空且扩展名（忽略大小写）为 .qflayout 返回 true，否则返回 false。
    bool validateFilePath(const QString& filePath,
                          QString* errorMessage) const;

    QMainWindow* mainWindow_; ///< 主窗口借用指针，所有权不在本类；为空时 saveLayout/loadLayout 直接失败。
    QHash<QString, QDockWidget*> moduleDocks_; ///< moduleId 到 Dock 的借用映射，Dock 所有权属于 MainWindow，释放前必须先 unregister。
    QString activeFilePath_; ///< 最近一次以 Activate 保存或加载成功的布局绝对路径，从未成功过时为空字符串。
};
}
