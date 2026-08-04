#include "LayoutManager.h"

#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMainWindow>
#include <QSaveFile>

namespace qframework
{
namespace
{
const int kLayoutVersion = 1;
const qint64 kMaximumLayoutBytes = 16 * 1024 * 1024;

void setError(QString* errorMessage, const QString& message)
{
    if (errorMessage != nullptr)
        *errorMessage = message;
}

bool decodeBase64(const QString& encoded, QByteArray* decoded)
{
    const QByteArray::FromBase64Result result = QByteArray::fromBase64Encoding(
        encoded.toLatin1(),
        QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
    if (!result)
        return false;
    *decoded = result.decoded;
    return true;
}
}

LayoutManager::LayoutManager(QMainWindow* mainWindow)
    : mainWindow_(mainWindow)
{
}

void LayoutManager::registerModuleDock(const QString& moduleId,
                                       QDockWidget* dockWidget)
{
    if (!moduleId.isEmpty() && dockWidget != nullptr)
        moduleDocks_.insert(moduleId, dockWidget);
}

void LayoutManager::unregisterModuleDock(const QString& moduleId)
{
    moduleDocks_.remove(moduleId);
}

bool LayoutManager::saveLayout(const QString& filePath,
                               QString* errorMessage)
{
    if (!validateFilePath(filePath, errorMessage) || mainWindow_ == nullptr)
        return false;

    QJsonObject modules;
    QStringList moduleIds = moduleDocks_.keys();
    moduleIds.sort(Qt::CaseInsensitive);
    for (const QString& moduleId : moduleIds) {
        QDockWidget* dockWidget = moduleDocks_.value(moduleId, nullptr);
        if (dockWidget == nullptr)
            continue;
        QJsonObject state;
        state.insert(QStringLiteral("visible"), dockWidget->isVisible());
        modules.insert(moduleId, state);
    }

    QJsonObject root;
    root.insert(QStringLiteral("format"), QStringLiteral("QFrameworkLayout"));
    root.insert(QStringLiteral("version"), kLayoutVersion);
    root.insert(QStringLiteral("geometry"),
                QString::fromLatin1(mainWindow_->saveGeometry().toBase64()));
    root.insert(QStringLiteral("state"),
                QString::fromLatin1(mainWindow_->saveState(kLayoutVersion).toBase64()));
    root.insert(QStringLiteral("modules"), modules);

    const QString absolutePath = QFileInfo(filePath).absoluteFilePath();
    QSaveFile file(absolutePath);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(errorMessage,
                 QString::fromUtf8(u8"无法写入布局文件：%1").arg(file.errorString()));
        return false;
    }
    const QByteArray data = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size()) {
        setError(errorMessage,
                 QString::fromUtf8(u8"布局文件写入不完整：%1").arg(file.errorString()));
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        setError(errorMessage,
                 QString::fromUtf8(u8"无法提交布局文件：%1").arg(file.errorString()));
        return false;
    }

    activeFilePath_ = absolutePath;
    return true;
}

bool LayoutManager::loadLayout(const QString& filePath,
                               QString* errorMessage,
                               QStringList* unavailableModuleIds)
{
    if (!validateFilePath(filePath, errorMessage) || mainWindow_ == nullptr)
        return false;
    if (unavailableModuleIds != nullptr)
        unavailableModuleIds->clear();

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(errorMessage,
                 QString::fromUtf8(u8"无法读取布局文件：%1").arg(file.errorString()));
        return false;
    }
    if (file.size() <= 0 || file.size() > kMaximumLayoutBytes) {
        setError(errorMessage, QString::fromUtf8(u8"布局文件为空或超过大小限制"));
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        setError(errorMessage,
                 QString::fromUtf8(u8"布局 JSON 无效：%1").arg(parseError.errorString()));
        return false;
    }

    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("format")).toString() !=
            QStringLiteral("QFrameworkLayout") ||
        root.value(QStringLiteral("version")).toInt(-1) != kLayoutVersion ||
        !root.value(QStringLiteral("geometry")).isString() ||
        !root.value(QStringLiteral("state")).isString() ||
        !root.value(QStringLiteral("modules")).isObject()) {
        setError(errorMessage, QString::fromUtf8(u8"布局文件格式或版本不受支持"));
        return false;
    }

    QByteArray geometry;
    QByteArray state;
    if (!decodeBase64(root.value(QStringLiteral("geometry")).toString(), &geometry) ||
        !decodeBase64(root.value(QStringLiteral("state")).toString(), &state) ||
        geometry.isEmpty() || state.isEmpty()) {
        setError(errorMessage, QString::fromUtf8(u8"布局中的窗口状态编码无效"));
        return false;
    }

    const QJsonObject modules = root.value(QStringLiteral("modules")).toObject();
    QHash<QString, bool> requestedVisibility;
    for (QJsonObject::const_iterator iterator = modules.constBegin();
         iterator != modules.constEnd(); ++iterator) {
        if (!iterator.value().isObject() ||
            !iterator.value().toObject().value(QStringLiteral("visible")).isBool()) {
            setError(errorMessage,
                     QString::fromUtf8(u8"模块 %1 的布局元数据无效").arg(iterator.key()));
            return false;
        }
        requestedVisibility.insert(
            iterator.key(),
            iterator.value().toObject().value(QStringLiteral("visible")).toBool());
        if (!moduleDocks_.contains(iterator.key()) && unavailableModuleIds != nullptr)
            unavailableModuleIds->append(iterator.key());
    }

    const QByteArray previousGeometry = mainWindow_->saveGeometry();
    const QByteArray previousState = mainWindow_->saveState(kLayoutVersion);
    QHash<QString, bool> previousVisibility;
    for (QHash<QString, QDockWidget*>::const_iterator iterator = moduleDocks_.constBegin();
         iterator != moduleDocks_.constEnd(); ++iterator) {
        if (iterator.value() != nullptr)
            previousVisibility.insert(iterator.key(), iterator.value()->isVisible());
    }

    const bool geometryRestored = mainWindow_->restoreGeometry(geometry);
    const bool stateRestored = mainWindow_->restoreState(state, kLayoutVersion);
    if (!geometryRestored || !stateRestored) {
        mainWindow_->restoreGeometry(previousGeometry);
        mainWindow_->restoreState(previousState, kLayoutVersion);
        for (QHash<QString, bool>::const_iterator iterator = previousVisibility.constBegin();
             iterator != previousVisibility.constEnd(); ++iterator) {
            QDockWidget* dockWidget = moduleDocks_.value(iterator.key(), nullptr);
            if (dockWidget != nullptr)
                dockWidget->setVisible(iterator.value());
        }
        setError(errorMessage, QString::fromUtf8(u8"Qt 无法恢复该布局，已保留当前布局"));
        return false;
    }

    for (QHash<QString, QDockWidget*>::const_iterator iterator = moduleDocks_.constBegin();
         iterator != moduleDocks_.constEnd(); ++iterator) {
        if (iterator.value() != nullptr) {
            iterator.value()->setVisible(
                requestedVisibility.value(iterator.key(), false));
        }
    }

    activeFilePath_ = QFileInfo(filePath).absoluteFilePath();
    return true;
}

QString LayoutManager::activeFilePath() const
{
    return activeFilePath_;
}

bool LayoutManager::validateFilePath(const QString& filePath,
                                     QString* errorMessage) const
{
    if (filePath.trimmed().isEmpty()) {
        setError(errorMessage, QString::fromUtf8(u8"布局文件路径为空"));
        return false;
    }
    if (QFileInfo(filePath).suffix().compare(
            QStringLiteral("qflayout"), Qt::CaseInsensitive) != 0) {
        setError(errorMessage, QString::fromUtf8(u8"布局文件扩展名必须为 .qflayout"));
        return false;
    }
    return true;
}
}
