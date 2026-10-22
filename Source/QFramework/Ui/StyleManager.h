#pragma once

// 文件职责：加载、校验并应用全局 QSS，同时保留当前文件路径供重新加载。
// 成功应用后通过信号把同一份样式广播给 UI 子进程。

#include <QObject>
#include <QString>

#include "QFrameworkGlobal.h"

namespace qframework
{
class QFRAMEWORK_EXPORT StyleManager : public QObject
{
    Q_OBJECT

public:
    /// @brief 构造样式管理器；此时尚未加载任何 QSS，内部路径和文本快照都是空字符串。
    /// @param parent 所属 QObject，通常为 FrameworkRuntime，允许为空。
    /// @return 无（构造函数不返回值）。
    explicit StyleManager(QObject* parent = nullptr);

    /// @brief 读取并校验 QSS 文件，全部通过后一次性应用到 QApplication；成功后更新快照并发射 styleSheetChanged，失败则保留旧样式。
    /// @param filePath 待加载的 QSS 文件路径，扩展名必须是 .qss。
    /// @param errorMessage 可选的错误输出参数；失败时写入中文原因，不需要错误文本时传 nullptr。
    /// @return 应用成功返回 true；扩展名无效、读取失败、超过大小上限、非 UTF-8、结构不完整或当前进程没有 QApplication 时返回 false。
    bool loadStyleSheet(const QString& filePath,
                        QString* errorMessage = nullptr);
    /// @brief 按最近一次成功加载的路径重新读取并应用样式，用于开发期改完 QSS 后热加载。
    /// @param errorMessage 可选的错误输出参数；失败时写入中文原因，不需要错误文本时传 nullptr。
    /// @return 重载成功返回 true；从未成功加载过任何文件，或重新加载失败时返回 false。
    bool reloadStyleSheet(QString* errorMessage = nullptr);
    /// @brief 查询当前生效样式的绝对文件路径。
    /// @return 绝对路径副本（调用方修改它不影响内部状态）；尚未成功加载过样式时返回空字符串。
    QString currentFilePath() const;
    /// @brief 查询当前生效样式表的完整文本。
    /// @return QSS 文本副本（调用方可以安全修改）；尚未成功加载过样式时返回空字符串。
    QString currentStyleSheet() const;

signals:
    /// @brief 新样式已成功设置到 QApplication 之后发射，用于把同一份样式同步给 UI 子进程。
    /// @param styleSheet 完整 QSS 文本，与当前生效的样式一致。
    /// @return 无。
    void styleSheetChanged(const QString& styleSheet);

private:
    /// @brief 读取 QSS 文件并依次完成扩展名、大小、UTF-8 编码和括号结构校验，全程不修改 QApplication 和成员快照。
    /// @param filePath 待读取的 QSS 文件路径。
    /// @param styleSheet 输出参数，接收校验通过的文件内容，调用方必须传入有效指针。
    /// @param errorMessage 可选的错误输出参数；失败时写入中文原因，不需要错误文本时传 nullptr。
    /// @return 全部校验通过返回 true；任一环节失败返回 false，此时不写入 styleSheet。
    bool readStyleSheet(const QString& filePath,
                        QString* styleSheet,
                        QString* errorMessage) const;
    /// @brief 用轻量状态机检查引号、注释和大括号是否闭合，在交给 QApplication 之前拦截半写入的 QSS 文件。
    /// @param styleSheet 待校验的样式文本。
    /// @return 结构完整返回 true；存在未闭合的引号、注释或大括号返回 false。
    bool isStructurallyValid(const QString& styleSheet) const;

    QString currentFilePath_; ///< 最近一次成功加载的 QSS 绝对路径，供 reloadStyleSheet 复用；从未成功加载时为空。
    QString currentStyleSheet_; ///< 最近一次成功应用的完整 QSS 文本，与 QApplication 当前生效的样式一致。
};
}
