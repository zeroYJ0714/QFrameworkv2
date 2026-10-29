// 【阅读导引】把窗口布局保存成可恢复文件。
// 阅读顺序：saveLayout 写 Qt 几何/停靠状态和显示意图；loadLayout 先校验文件，再尝试恢复，失败时保持或回退原状态。
// 需要分清：Qt 的二进制布局通过 Base64 放进 JSON。requestedVisible 是用户意图，不等于此刻屏幕上能看见；隐藏标签页也可能仍要求显示。
// 这是执行实现：按入口调用和信号回调追踪，不要按文件行号把所有函数理解成连续执行。

// 文件职责：保存和恢复主窗口 Dock/Tab 布局，并处理版本化布局键。
// QSettings、QMainWindow 和 QDockWidget 只在 GUI 线程访问；布局管理器不拥有 Dock 或插件，
// 恢复失败时保留 Qt 默认布局并返回错误，避免破坏用户当前窗口状态。
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
const int kLayoutVersion = 1; ///< 布局文件格式版本，同时用于 Qt restoreState 的版本校验。
// 限制文件大小，防止误选大文件导致 readAll 占用过多内存。
const qint64 kMaximumLayoutBytes = 16 * 1024 * 1024; ///< 布局文件大小上限，超过即拒绝读取。

// 统一写入可选错误输出；布局校验失败时不会修改主窗口。
/// @brief 统一写入可选错误输出，调用方传空指针时安全忽略。
/// @param errorMessage 可选输出参数，为空时丢弃消息。
/// @param message 要写入的中文失败原因。
/// @return 无。
void setError(QString* errorMessage, const QString& message)
{
    if (errorMessage != nullptr)
        *errorMessage = message;
}

// 严格解码 Qt geometry/state 的 Base64 文本，非法字符直接失败。
/// @brief 严格解码 Qt 布局使用的 Base64 文本，遇到非法字符直接失败。
/// @param encoded Base64 文本。
/// @param decoded 输出参数，解码成功时写入原始字节。
/// @return 解码成功返回 true，包含非法字符时返回 false。
bool decodeBase64(const QString& encoded, QByteArray* decoded)
{
    // AbortOnBase64DecodingErrors 禁止静默忽略非法字符。
    const QByteArray::FromBase64Result result = QByteArray::fromBase64Encoding(
        encoded.toLatin1(),
        QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors); ///< 严格模式下的解码结果，可判断是否合法。
    if (!result)
        return false;
    *decoded = result.decoded;
    return true;
}
}

// 保存主窗口借用指针；Dock 映射在 MainWindow 创建/释放模块时维护。
/// @brief 构造布局管理器，只保存主窗口借用指针，不接管任何 Dock 所有权。
/// @param mainWindow 目标主窗口，生命周期必须长于本对象。
/// @return 无（构造函数不返回值）。
LayoutManager::LayoutManager(QMainWindow* mainWindow)
    : mainWindow_(mainWindow)
{
}

// 注册一个可用于布局保存/恢复的模块 Dock 借用指针。
/// @brief 登记模块 Dock 的借用指针，供布局保存和恢复时查找。
/// @param moduleId 模块的稳定标识。
/// @param dockWidget 对应的 Dock 指针，所有权仍属于主窗口。
/// @return 无；ID 为空或指针为空时忽略。
void LayoutManager::registerModuleDock(const QString& moduleId,
                                       QDockWidget* dockWidget)
{
    // 空 ID 或空指针没有可恢复意义，直接忽略。
    if (!moduleId.isEmpty() && dockWidget != nullptr)
        moduleDocks_.insert(moduleId, dockWidget);
}

// 仅删除映射，不触碰 MainWindow 对 Dock 的 QObject 所有权。
/// @brief 移除模块 Dock 的借用映射，不销毁 Dock 本身。
/// @param moduleId 模块的稳定标识。
/// @return 无。
void LayoutManager::unregisterModuleDock(const QString& moduleId)
{
    // 只移除借用映射，不 delete MainWindow 拥有的 Dock。
    moduleDocks_.remove(moduleId);
}

// 把窗口几何、Qt Dock 状态和模块可见性写成带版本 JSON，并原子提交。
/// @brief 把窗口几何、Qt Dock 状态和模块显示意图写成带版本 JSON 并原子落盘。
/// @param filePath 目标文件路径，扩展名必须为 .qflayout。
/// @param requestedVisibility 各模块的用户显示意图，按 moduleId 索引。
/// @param errorMessage 可选输出参数，失败时写入中文原因。
/// @param activation 是否把本次保存的路径记为当前活动布局。
/// @return 写入并提交成功返回 true，任一环节失败返回 false。
bool LayoutManager::saveLayout(const QString& filePath,
                               const QHash<QString, bool>& requestedVisibility,
                               QString* errorMessage,
                               LayoutActivation activation)
{
    // 扩展名和主窗口都有效后才调用 Qt saveGeometry/saveState。
    if (!validateFilePath(filePath, errorMessage) || mainWindow_ == nullptr)
        return false;

    // 按 moduleId 排序，生成稳定且便于审查的 JSON。
    QJsonObject modules; ///< 模块可见性节点，按 moduleId 存放显示意图。
    QStringList moduleIds = moduleDocks_.keys(); ///< 已登记模块 ID，排序后保证输出稳定。
    moduleIds.sort(Qt::CaseInsensitive);
    for (const QString& moduleId : moduleIds) { ///< 当前正在写入的模块 ID。
        QDockWidget* dockWidget = moduleDocks_.value(moduleId, nullptr); ///< 该模块对应的 Dock，可能已被主窗口释放。
        if (dockWidget == nullptr)
            continue;
        QJsonObject state; ///< 单个模块的可见性元数据。
        const bool requestedVisible = requestedVisibility.value(moduleId, false); ///< 该模块的用户显示意图，缺省按隐藏处理。
        // visible 保留给旧程序读取；requestedVisible 才是用户的显示意图。
        state.insert(QStringLiteral("visible"), requestedVisible);
        state.insert(QStringLiteral("requestedVisible"), requestedVisible);
        modules.insert(moduleId, state);
    }

    QJsonObject root; ///< 布局文件根对象。
    // format/version 是兼容性门槛；geometry/state 是 Qt 的不透明字节。
    root.insert(QStringLiteral("format"), QStringLiteral("QFrameworkLayout"));
    root.insert(QStringLiteral("version"), kLayoutVersion);
    root.insert(QStringLiteral("visibilitySemantics"), QStringLiteral("userIntent"));
    root.insert(QStringLiteral("geometry"),
                QString::fromLatin1(mainWindow_->saveGeometry().toBase64()));
    root.insert(QStringLiteral("state"),
                QString::fromLatin1(mainWindow_->saveState(kLayoutVersion).toBase64()));
    root.insert(QStringLiteral("modules"), modules);

    const QString absolutePath = QFileInfo(filePath).absoluteFilePath(); ///< 规范化后的绝对路径，用于原子写入和记录。
    QSaveFile file(absolutePath); ///< 原子写入句柄，提交成功才替换目标文件。
    // QSaveFile 先写临时文件，commit 成功后再原子替换目标。
    if (!file.open(QIODevice::WriteOnly)) {
        setError(errorMessage,
                 QString::fromUtf8(u8"无法写入布局文件：%1").arg(file.errorString()));
        return false;
    }
    const QByteArray data = QJsonDocument(root).toJson(QJsonDocument::Indented); ///< 缩进格式的 JSON 字节，便于人工审查布局文件。
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

    if (activation == LayoutActivation::Activate)
        activeFilePath_ = absolutePath;
    return true;
}

// 只解析和校验布局文件，供启动菜单在不改变窗口的前提下判断是否可用。
/// @brief 只解析并校验布局文件，不修改主窗口，供菜单判断布局是否可用。
/// @param filePath 待校验的布局文件路径。
/// @param errorMessage 可选输出参数，失败时写入中文原因。
/// @return 文件有效返回 true，格式或内容不合法返回 false。
bool LayoutManager::validateLayoutFile(const QString& filePath,
                                       QString* errorMessage) const
{
    QByteArray geometry; ///< 占位接收窗口几何字节，此处只做校验。
    QByteArray state; ///< 占位接收 Dock 状态字节，此处只做校验。
    QHash<QString, bool> requestedVisibility; ///< 占位接收模块显示意图，此处只做校验。
    QStringList unavailableModuleIds; ///< 占位接收未注册模块 ID，此处只做校验。
    bool legacyVisibilitySemantics = false; ///< 占位接收是否使用旧可见性语义。
    return readLayoutFile(filePath,
                          &geometry,
                          &state,
                          &requestedVisibility,
                          &unavailableModuleIds,
                          &legacyVisibilitySemantics,
                          errorMessage);
}

// 把磁盘文件转换成经过完整校验的 Qt 状态和模块显示意图，但不应用到窗口。
/// @brief 读取并完整校验布局文件，输出可用的 Qt 状态和模块显示意图。
/// @param filePath 待读取的布局文件路径。
/// @param geometry 可选输出参数，写入校验通过的窗口几何字节。
/// @param state 可选输出参数，写入校验通过的 Dock 状态字节。
/// @param requestedVisibility 可选输出参数，写入各模块的显示意图。
/// @param unavailableModuleIds 可选输出参数，写入当前未注册的模块 ID。
/// @param legacyVisibilitySemantics 可选输出参数，写入文件是否使用旧可见性语义。
/// @param errorMessage 可选输出参数，失败时写入中文原因。
/// @return 全部校验通过返回 true，否则返回 false。
bool LayoutManager::readLayoutFile(const QString& filePath,
                                   QByteArray* geometry,
                                   QByteArray* state,
                                   QHash<QString, bool>* requestedVisibility,
                                   QStringList* unavailableModuleIds,
                                   bool* legacyVisibilitySemantics,
                                   QString* errorMessage) const
{
    if (!validateFilePath(filePath, errorMessage))
        return false;

    QFile file(filePath); ///< 待读取的布局文件句柄。
    if (!file.open(QIODevice::ReadOnly)) {
        setError(errorMessage,
                 QString::fromUtf8(u8"无法读取布局文件：%1").arg(file.errorString()));
        return false;
    }
    if (file.size() <= 0 || file.size() > kMaximumLayoutBytes) {
        setError(errorMessage, QString::fromUtf8(u8"布局文件为空或超过大小限制"));
        return false;
    }

    QJsonParseError parseError; ///< 承接 JSON 解析错误码和出错位置。
    // JSON 语法和顶层类型必须同时正确。
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError); ///< 从文件内容解析出的 JSON 文档。
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        setError(errorMessage,
                 QString::fromUtf8(u8"布局 JSON 无效：%1").arg(parseError.errorString()));
        return false;
    }

    const QJsonObject root = document.object(); ///< 布局文件根对象。
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

    QByteArray loadedGeometry; ///< 解码后的窗口几何字节。
    QByteArray loadedState; ///< 解码后的 Dock 状态字节。
    if (!decodeBase64(root.value(QStringLiteral("geometry")).toString(), &loadedGeometry) ||
        !decodeBase64(root.value(QStringLiteral("state")).toString(), &loadedState) ||
        loadedGeometry.isEmpty() || loadedState.isEmpty()) {
        setError(errorMessage, QString::fromUtf8(u8"布局中的窗口状态编码无效"));
        return false;
    }

    const QJsonObject modules = root.value(QStringLiteral("modules")).toObject(); ///< 模块可见性节点。
    const bool usesUserIntent =
        root.value(QStringLiteral("visibilitySemantics")).toString() ==
        QStringLiteral("userIntent"); ///< 文件是否声明使用“用户意图”可见性语义。
    if (root.contains(QStringLiteral("visibilitySemantics")) && !usesUserIntent) {
        setError(errorMessage, QString::fromUtf8(u8"布局可见性语义不受支持"));
        return false;
    }
    // 先验证所有模块元数据，确认无误后才改动主窗口。
    QHash<QString, bool> loadedRequestedVisibility; ///< 校验通过的模块显示意图。
    QStringList loadedUnavailableModules; ///< 文件中出现但当前未注册的模块 ID。
    for (QJsonObject::const_iterator iterator = modules.constBegin();
         iterator != modules.constEnd(); ++iterator) { ///< 遍历模块节点的迭代器。
        if (!iterator.value().isObject()) {
            setError(errorMessage,
                     QString::fromUtf8(u8"模块 %1 的布局元数据无效").arg(iterator.key()));
            return false;
        }
        const QJsonObject moduleState = iterator.value().toObject(); ///< 单个模块的可见性元数据。
        const QString visibilityKey = usesUserIntent
            ? QStringLiteral("requestedVisible") : QStringLiteral("visible"); ///< 按文件语义选择要读取的可见性字段名。
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

    // 只把已校验的内容返回给 loadLayout；这里绝不能调用 restoreGeometry/restoreState。
    if (geometry != nullptr)
        *geometry = loadedGeometry;
    if (state != nullptr)
        *state = loadedState;
    if (requestedVisibility != nullptr)
        *requestedVisibility = loadedRequestedVisibility;
    if (unavailableModuleIds != nullptr)
        *unavailableModuleIds = loadedUnavailableModules;
    if (legacyVisibilitySemantics != nullptr)
        *legacyVisibilitySemantics = !usesUserIntent;
    return true;
}

// 完整解析并校验布局后尝试恢复；Qt 任一步失败都会回滚到调用前状态。
/// @brief 解析并校验布局文件后应用到主窗口，Qt 恢复失败时回滚到调用前状态。
/// @param filePath 待加载的布局文件路径。
/// @param requestedVisibility 可选输出参数，写入各模块的显示意图。
/// @param errorMessage 可选输出参数，失败时写入中文原因。
/// @param unavailableModuleIds 可选输出参数，写入当前未注册的模块 ID。
/// @param legacyVisibilitySemantics 可选输出参数，写入文件是否使用旧可见性语义。
/// @param activation 是否把本次加载的路径记为当前活动布局。
/// @return 恢复成功返回 true；校验失败或 Qt 回滚时返回 false。
bool LayoutManager::loadLayout(const QString& filePath,
                               QHash<QString, bool>* requestedVisibility,
                               QString* errorMessage,
                               QStringList* unavailableModuleIds,
                               bool* legacyVisibilitySemantics,
                               LayoutActivation activation)
{
    if (mainWindow_ == nullptr) {
        setError(errorMessage, QString::fromUtf8(u8"主窗口不可用"));
        return false;
    }
    if (requestedVisibility != nullptr)
        requestedVisibility->clear();
    if (unavailableModuleIds != nullptr)
        unavailableModuleIds->clear();
    if (legacyVisibilitySemantics != nullptr)
        *legacyVisibilitySemantics = false;

    QByteArray geometry; ///< 从文件读出的窗口几何字节。
    QByteArray state; ///< 从文件读出的 Dock 状态字节。
    QHash<QString, bool> loadedRequestedVisibility; ///< 从文件读出的模块显示意图。
    QStringList loadedUnavailableModules; ///< 文件中出现但当前未注册的模块 ID。
    bool loadedLegacyVisibility = false; ///< 文件是否使用旧的可见性语义。
    if (!readLayoutFile(filePath,
                        &geometry,
                        &state,
                        &loadedRequestedVisibility,
                        &loadedUnavailableModules,
                        &loadedLegacyVisibility,
                        errorMessage)) {
        return false;
    }

    // 保存完整旧状态，用于 restoreGeometry/restoreState 任一失败时回滚。
    const QByteArray previousGeometry = mainWindow_->saveGeometry(); ///< 恢复前的窗口几何快照。
    const QByteArray previousState = mainWindow_->saveState(kLayoutVersion); ///< 恢复前的 Dock 状态快照。
    QHash<QString, bool> previousVisibility; ///< 恢复前各模块 Dock 的实际可见性快照。
    for (QHash<QString, QDockWidget*>::const_iterator iterator = moduleDocks_.constBegin();
         iterator != moduleDocks_.constEnd(); ++iterator) { ///< 遍历已登记 Dock 的迭代器。
        if (iterator.value() != nullptr)
            previousVisibility.insert(iterator.key(), iterator.value()->isVisible());
    }

    const bool geometryRestored = mainWindow_->restoreGeometry(geometry); ///< Qt 是否接受该窗口几何。
    const bool stateRestored = mainWindow_->restoreState(state, kLayoutVersion); ///< Qt 是否接受该 Dock 状态。
    if (!geometryRestored || !stateRestored) {
        mainWindow_->restoreGeometry(previousGeometry);
        mainWindow_->restoreState(previousState, kLayoutVersion);
        for (QHash<QString, bool>::const_iterator iterator = previousVisibility.constBegin();
             iterator != previousVisibility.constEnd(); ++iterator) { ///< 回滚可见性时遍历快照的迭代器。
            QDockWidget* dockWidget = moduleDocks_.value(iterator.key(), nullptr); ///< 回滚目标 Dock。
            if (dockWidget != nullptr)
                dockWidget->setVisible(iterator.value());
        }
        setError(errorMessage, QString::fromUtf8(u8"Qt 无法恢复该布局，已保留当前布局"));
        return false;
    }

    // Qt state 恢复后再应用用户意图；显示 Dock 时保留标签组当前页。
    for (QHash<QString, QDockWidget*>::const_iterator iterator = moduleDocks_.constBegin();
         iterator != moduleDocks_.constEnd(); ++iterator) { ///< 应用用户显示意图时遍历 Dock 的迭代器。
        QDockWidget* dockWidget = iterator.value(); ///< 当前要应用显示意图的 Dock。
        if (dockWidget == nullptr)
            continue;
        if (!loadedRequestedVisibility.value(iterator.key(), false)) {
            dockWidget->hide();
            continue;
        }
        QDockWidget* activeTab = nullptr; ///< 标签组中当前可见的同级 Dock，用于显示后保持置顶。
        const QList<QDockWidget*> tabSiblings = mainWindow_->tabifiedDockWidgets(dockWidget); ///< 与该 Dock 同组的标签页列表。
        for (QDockWidget* sibling : tabSiblings) { ///< 同组标签页中的当前项。
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
        *legacyVisibilitySemantics = loadedLegacyVisibility;
    if (activation == LayoutActivation::Activate)
        activeFilePath_ = QFileInfo(filePath).absoluteFilePath();
    return true;
}

// 返回最近一次成功保存或加载的布局绝对路径。
/// @brief 查询当前活动布局文件的绝对路径。
/// @return 路径副本；尚未保存或加载过布局时返回空字符串。
QString LayoutManager::activeFilePath() const
{
    return activeFilePath_;
}

// 检查路径非空及 .qflayout 扩展名，避免误读/覆盖任意文件。
/// @brief 校验布局路径非空且扩展名为 .qflayout。
/// @param filePath 待校验的路径。
/// @param errorMessage 可选输出参数，失败时写入中文原因。
/// @return 路径合法返回 true，否则返回 false。
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
