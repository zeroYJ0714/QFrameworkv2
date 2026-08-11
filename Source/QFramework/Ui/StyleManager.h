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
    // StyleManager 不拥有 QApplication，只通过 QCoreApplication::instance 查找。
    /// @brief 创建 StyleManager 对象并初始化其基础状态。
    /// @param parent Qt 父对象或父控件；用于建立所有权关系，允许为空。
    /// @return 无（构造函数不返回值）。
    explicit StyleManager(QObject* parent = nullptr);

    // 加载新文件成功后才替换当前路径/内容；失败保留旧样式。
    /// @brief 加载 `loadStyleSheet` 对应的框架操作。
    /// @param filePath 文件路径。
    /// @param errorMessage 可选错误说明输出参数。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool loadStyleSheet(const QString& filePath,
                        QString* errorMessage = nullptr); ///< `errorMessage` 对应的对象指针；所有权和线程归属见本类文件级说明。
    // 使用上一次成功路径重新读取，适合开发时修改 QSS 后刷新。
    /// @brief 执行 `reloadStyleSheet` 所定义的类职责。
    /// @param errorMessage 可选错误说明输出参数。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool reloadStyleSheet(QString* errorMessage = nullptr);

    /// @brief 执行 `currentFilePath` 所定义的类职责。
    /// @return 返回对应 Qt 值或容器的安全副本。
    QString currentFilePath() const;
    /// @brief 执行 `currentStyleSheet` 所定义的类职责。
    /// @return 返回对应 Qt 值或容器的安全副本。
    QString currentStyleSheet() const;

signals:
    // 只在新样式已成功设置到 QApplication 后发出。
    /// @brief 执行 `styleSheetChanged` 所定义的类职责。
    /// @param styleSheet 完整 QSS 文本。
    /// @return 无。
    void styleSheetChanged(const QString& styleSheet);

private:
    // readStyleSheet 负责文件/UTF-8/结构校验，不修改 QApplication。
    /// @brief 读取 `readStyleSheet` 对应的框架操作。
    /// @param filePath 文件路径。
    /// @param styleSheet 完整 QSS 文本。
    /// @param errorMessage 可选错误说明输出参数。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool readStyleSheet(const QString& filePath,
                        QString* styleSheet,
                        QString* errorMessage) const; ///< `const` 对应的对象指针；所有权和线程归属见本类文件级说明。
    /// @brief 检查 `isStructurallyValid` 对应的框架操作。
    /// @param styleSheet 完整 QSS 文本。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool isStructurallyValid(const QString& styleSheet) const;

    // 这两个字段始终代表最近一次成功加载的样式。
    QString currentFilePath_; ///< 保存 `currentFilePath` 对应的对象状态或配置值。
    QString currentStyleSheet_; ///< 保存 `currentStyleSheet` 对应的对象状态或配置值。
};
}
