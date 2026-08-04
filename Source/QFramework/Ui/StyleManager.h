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
    explicit StyleManager(QObject* parent = nullptr);

    // 加载新文件成功后才替换当前路径/内容；失败保留旧样式。
    bool loadStyleSheet(const QString& filePath,
                        QString* errorMessage = nullptr);
    // 使用上一次成功路径重新读取，适合开发时修改 QSS 后刷新。
    bool reloadStyleSheet(QString* errorMessage = nullptr);

    QString currentFilePath() const;
    QString currentStyleSheet() const;

signals:
    // 只在新样式已成功设置到 QApplication 后发出。
    void styleSheetChanged(const QString& styleSheet);

private:
    // readStyleSheet 负责文件/UTF-8/结构校验，不修改 QApplication。
    bool readStyleSheet(const QString& filePath,
                        QString* styleSheet,
                        QString* errorMessage) const;
    bool isStructurallyValid(const QString& styleSheet) const;

    // 这两个字段始终代表最近一次成功加载的样式。
    QString currentFilePath_;
    QString currentStyleSheet_;
};
}
