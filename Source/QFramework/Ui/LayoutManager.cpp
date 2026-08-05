#include "LayoutManager.h"

#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMainWindow>
#include <QSaveFile>

// 布局处理采用“解析/校验 -> 记录旧状态 -> 尝试恢复 -> 必要时回滚”的顺序，
// 避免损坏文件把用户当前工作区也破坏掉。

namespace qframework
{
namespace
{
const int kLayoutVersion = 1;
// 限制文件大小，防止误选大文件导致 readAll 占用过多内存。
const qint64 kMaximumLayoutBytes = 16 * 1024 * 1024;

// 统一写入可选错误输出；布局校验失败时不会修改主窗口。
void setError(QString* errorMessage, const QString& message)
{
    if (errorMessage != nullptr)
        *errorMessage = message;
}

// 严格解码 Qt geometry/state 的 Base64 文本，非法字符直接失败。
bool decodeBase64(const QString& encoded, QByteArray* decoded)
{
    // AbortOnBase64DecodingErrors 禁止静默忽略非法字符。
    const QByteArray::FromBase64Result result = QByteArray::fromBase64Encoding(
        encoded.toLatin1(),
        QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
    if (!result)
        return false;
    *decoded = result.decoded;
    return true;
}
}

// 保存主窗口借用指针；Dock 映射在 MainWindow 创建/释放模块时维护。
LayoutManager::LayoutManager(QMainWindow* mainWindow)
    : mainWindow_(mainWindow)
{
}

// 注册一个可用于布局保存/恢复的模块 Dock 借用指针。
void LayoutManager::registerModuleDock(const QString& moduleId,
                                       QDockWidget* dockWidget)
{
    // 空 ID 或空指针没有可恢复意义，直接忽略。
    if (!moduleId.isEmpty() && dockWidget != nullptr)
        moduleDocks_.insert(moduleId, dockWidget);
}

// 仅删除映射，不触碰 MainWindow 对 Dock 的 QObject 所有权。
void LayoutManager::unregisterModuleDock(const QString& moduleId)
{
    // 只移除借用映射，不 delete MainWindow 拥有的 Dock。
    moduleDocks_.remove(moduleId);
}

// 把窗口几何、Qt Dock 状态和模块可见性写成带版本 JSON，并原子提交。
bool LayoutManager::saveLayout(const QString& filePath,
                               const QHash<QString, bool>& requestedVisibility,
                               QString* errorMessage)
{
    // 扩展名和主窗口都有效后才调用 Qt saveGeometry/saveState。
    if (!validateFilePath(filePath, errorMessage) || mainWindow_ == nullptr)
        return false;

    // 按 moduleId 排序，生成稳定且便于审查的 JSON。
    QJsonObject modules;
    QStringList moduleIds = moduleDocks_.keys();
    moduleIds.sort(Qt::CaseInsensitive);
    for (const QString& moduleId : moduleIds) {
        QDockWidget* dockWidget = moduleDocks_.value(moduleId, nullptr);
        if (dockWidget == nullptr)
            continue;
        QJsonObject state;
        const bool requestedVisible = requestedVisibility.value(moduleId, false);
        // visible 保留给旧程序读取；requestedVisible 才是用户的显示意图。
        state.insert(QStringLiteral("visible"), requestedVisible);
        state.insert(QStringLiteral("requestedVisible"), requestedVisible);
        modules.insert(moduleId, state);
    }

    QJsonObject root;
    // format/version 是兼容性门槛；geometry/state 是 Qt 的不透明字节。
    root.insert(QStringLiteral("format"), QStringLiteral("QFrameworkLayout"));
    root.insert(QStringLiteral("version"), kLayoutVersion);
    root.insert(QStringLiteral("visibilitySemantics"), QStringLiteral("userIntent"));
    root.insert(QStringLiteral("geometry"),
                QString::fromLatin1(mainWindow_->saveGeometry().toBase64()));
    root.insert(QStringLiteral("state"),
                QString::fromLatin1(mainWindow_->saveState(kLayoutVersion).toBase64()));
    root.insert(QStringLiteral("modules"), modules);

    const QString absolutePath = QFileInfo(filePath).absoluteFilePath();
    QSaveFile file(absolutePath);
    // QSaveFile 先写临时文件，commit 成功后再原子替换目标。
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

// 完整解析并校验布局后尝试恢复；Qt 任一步失败都会回滚到调用前状态。
bool LayoutManager::loadLayout(const QString& filePath,
                               QHash<QString, bool>* requestedVisibility,
                               QString* errorMessage,
                               QStringList* unavailableModuleIds,
                               bool* legacyVisibilitySemantics)
{
    // 输出列表每次调用先清空，避免调用方误用上一次结果。
    if (!validateFilePath(filePath, errorMessage) || mainWindow_ == nullptr)
        return false;
    if (unavailableModuleIds != nullptr)
        unavailableModuleIds->clear();
    if (legacyVisibilitySemantics != nullptr)
        *legacyVisibilitySemantics = false;

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
    // JSON 语法和顶层类型必须同时正确。
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        setError(errorMessage,
                 QString::fromUtf8(u8"布局 JSON 无效：%1").arg(parseError.errorString()));
        return false;
    }

    const QJsonObject root = document.object();
    // 严格检查格式、版本和必需字段类型，不猜测旧版结构。
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
    const bool usesUserIntent =
        root.value(QStringLiteral("visibilitySemantics")).toString() ==
        QStringLiteral("userIntent");
    if (root.contains(QStringLiteral("visibilitySemantics")) && !usesUserIntent) {
        setError(errorMessage, QString::fromUtf8(u8"布局可见性语义不受支持"));
        return false;
    }
    // 先验证所有模块元数据，确认无误后才改动主窗口。
    QHash<QString, bool> loadedRequestedVisibility;
    QStringList loadedUnavailableModules;
    for (QJsonObject::const_iterator iterator = modules.constBegin();
         iterator != modules.constEnd(); ++iterator) {
        if (!iterator.value().isObject()) {
            setError(errorMessage,
                     QString::fromUtf8(u8"模块 %1 的布局元数据无效").arg(iterator.key()));
            return false;
        }
        const QJsonObject moduleState = iterator.value().toObject();
        const QString visibilityKey = usesUserIntent
            ? QStringLiteral("requestedVisible") : QStringLiteral("visible");
        if (!moduleState.value(visibilityKey).isBool() ||
            (usesUserIntent && !moduleState.value(QStringLiteral("visible")).isBool())) {
            setError(errorMessage,
                     QString::fromUtf8(u8"模块 %1 的布局可见性无效").arg(iterator.key()));
            return false;
        }
        loadedRequestedVisibility.insert(
            iterator.key(), moduleState.value(visibilityKey).toBool());
        if (!moduleDocks_.contains(iterator.key()))
            loadedUnavailableModules.append(iterator.key());
    }

    // 保存完整旧状态，用于 restoreGeometry/restoreState 任一失败时回滚。
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
        // Qt 恢复失败时还原三部分状态，保证用户当前布局不被半应用。
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

    // Qt state 恢复后再应用用户意图；显示 Dock 时保留标签组当前页。
    for (QHash<QString, QDockWidget*>::const_iterator iterator = moduleDocks_.constBegin();
         iterator != moduleDocks_.constEnd(); ++iterator) {
        QDockWidget* dockWidget = iterator.value();
        if (dockWidget == nullptr)
            continue;
        if (!loadedRequestedVisibility.value(iterator.key(), false)) {
            dockWidget->hide();
            continue;
        }
        QDockWidget* activeTab = nullptr;
        const QList<QDockWidget*> tabSiblings = mainWindow_->tabifiedDockWidgets(dockWidget);
        for (QDockWidget* sibling : tabSiblings) {
            if (sibling != nullptr && sibling->isVisible()) {
                activeTab = sibling;
                break;
            }
        }
        dockWidget->show();
        if (activeTab != nullptr)
            activeTab->raise();
    }

    if (requestedVisibility != nullptr)
        *requestedVisibility = loadedRequestedVisibility;
    if (unavailableModuleIds != nullptr)
        *unavailableModuleIds = loadedUnavailableModules;
    if (legacyVisibilitySemantics != nullptr)
        *legacyVisibilitySemantics = !usesUserIntent;
    activeFilePath_ = QFileInfo(filePath).absoluteFilePath();
    return true;
}

// 返回最近一次成功保存或加载的布局绝对路径。
QString LayoutManager::activeFilePath() const
{
    return activeFilePath_;
}

// 检查路径非空及 .qflayout 扩展名，避免误读/覆盖任意文件。
bool LayoutManager::validateFilePath(const QString& filePath,
                                     QString* errorMessage) const
{
    // 布局只接受专用扩展名，避免覆盖用户选中的任意文件。
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
