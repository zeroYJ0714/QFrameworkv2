#include "StyleManager.h"

#include <QApplication>
#include <QFile>
#include <QFileInfo>
#include <QTextCodec>

// QSS 加载采用“先完整验证，后一次应用”的事务式流程，失败不会清空旧样式。

namespace qframework
{
namespace
{
const qint64 kMaximumStyleSheetBytes = 16 * 1024 * 1024;

// 统一写入可选错误输出，避免每个校验分支重复判断 nullptr。
void setError(QString* errorMessage, const QString& message)
{
    // errorMessage 是可选输出参数，调用方不需要错误文本时允许传 nullptr。
    if (errorMessage != nullptr)
        *errorMessage = message;
}
}

// StyleManager 不持有 QApplication；只在真正加载时检查当前应用对象。
StyleManager::StyleManager(QObject* parent)
    : QObject(parent)
{
}

// 先读取和结构校验，再一次性应用 QSS；失败时保留上一份成功样式。
bool StyleManager::loadStyleSheet(const QString& filePath,
                                  QString* errorMessage)
{
    // 临时变量接收文件内容，只有全部成功才写入 current* 字段。
    QString styleSheet;
    if (!readStyleSheet(filePath, &styleSheet, errorMessage))
        return false;

    QApplication* application = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (application == nullptr) {
        setError(errorMessage, QString::fromUtf8(u8"当前进程没有 QApplication"));
        return false;
    }

    application->setStyleSheet(styleSheet);
    // QApplication 接受样式后再更新快照并通知子进程。
    currentFilePath_ = QFileInfo(filePath).absoluteFilePath();
    currentStyleSheet_ = styleSheet;
    emit styleSheetChanged(currentStyleSheet_);
    return true;
}

// 重新读取最近一次成功路径，适合开发阶段修改 QSS 后热加载。
bool StyleManager::reloadStyleSheet(QString* errorMessage)
{
    // 未成功加载过任何文件时没有可重载目标。
    if (currentFilePath_.isEmpty()) {
        setError(errorMessage, QString::fromUtf8(u8"尚未选择 QSS 文件"));
        return false;
    }
    return loadStyleSheet(currentFilePath_, errorMessage);
}

// 返回当前成功样式的绝对路径副本。
QString StyleManager::currentFilePath() const
{
    // 返回值副本，调用方修改它不会改变内部状态。
    return currentFilePath_;
}

// 返回当前成功样式文本副本，调用方可以安全修改返回值。
QString StyleManager::currentStyleSheet() const
{
    return currentStyleSheet_;
}

// 读取文件、限制大小、验证 UTF-8 和括号结构；不修改 QApplication 或成员快照。
bool StyleManager::readStyleSheet(const QString& filePath,
                                  QString* styleSheet,
                                  QString* errorMessage) const
{
    // 先限制扩展名和大小，再读取全部内容，避免误把任意大文件当 QSS。
    if (filePath.trimmed().isEmpty() ||
        QFileInfo(filePath).suffix().compare(
            QStringLiteral("qss"), Qt::CaseInsensitive) != 0) {
        setError(errorMessage, QString::fromUtf8(u8"QSS 文件路径为空或扩展名无效"));
        return false;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(errorMessage,
                 QString::fromUtf8(u8"无法读取 QSS 文件：%1").arg(file.errorString()));
        return false;
    }
    if (file.size() < 0 || file.size() > kMaximumStyleSheetBytes) {
        setError(errorMessage, QString::fromUtf8(u8"QSS 文件超过大小限制"));
        return false;
    }

    const QByteArray data = file.readAll();
    // ConverterState 能区分“替换了非法字符”和真正的合法 UTF-8。
    QTextCodec* codec = QTextCodec::codecForName("UTF-8");
    QTextCodec::ConverterState state;
    const QString decoded = codec->toUnicode(data.constData(), data.size(), &state);
    if (state.invalidChars > 0) {
        setError(errorMessage, QString::fromUtf8(u8"QSS 文件不是有效的 UTF-8"));
        return false;
    }
    if (!isStructurallyValid(decoded)) {
        setError(errorMessage, QString::fromUtf8(u8"QSS 文件的括号、引号或注释不完整"));
        return false;
    }

    *styleSheet = decoded;
    return true;
}

// 用状态机检查引号、注释和大括号是否闭合，拦截常见的半写入 QSS 文件。
bool StyleManager::isStructurallyValid(const QString& styleSheet) const
{
    // 轻量扫描器只检查括号、单双引号和 /* */ 注释是否闭合；
    // 它不是完整 QSS 解析器，但能在 QApplication 应用前拦截常见截断文件。
    enum class ScanState
    {
        Normal,
        SingleQuoted,
        DoubleQuoted,
        Comment
    };

    ScanState state = ScanState::Normal;
    int braceDepth = 0;
    bool escaped = false;
    for (int index = 0; index < styleSheet.size(); ++index) {
        const QChar character = styleSheet.at(index);
        const QChar next = index + 1 < styleSheet.size()
            ? styleSheet.at(index + 1) : QChar();

        if (state == ScanState::Comment) {
            // 注释内部的大括号和引号不参与结构计数。
            if (character == QLatin1Char('*') && next == QLatin1Char('/')) {
                state = ScanState::Normal;
                ++index;
            }
            continue;
        }
        if (state == ScanState::SingleQuoted || state == ScanState::DoubleQuoted) {
            // 反斜杠转义的下一字符不应结束字符串。
            if (escaped) {
                escaped = false;
                continue;
            }
            if (character == QLatin1Char('\\')) {
                escaped = true;
                continue;
            }
            if ((state == ScanState::SingleQuoted && character == QLatin1Char('\'')) ||
                (state == ScanState::DoubleQuoted && character == QLatin1Char('"'))) {
                state = ScanState::Normal;
            }
            continue;
        }

        if (character == QLatin1Char('/') && next == QLatin1Char('*')) {
            // 进入注释/字符串后由上面的状态分支负责退出。
            state = ScanState::Comment;
            ++index;
        } else if (character == QLatin1Char('\'')) {
            state = ScanState::SingleQuoted;
        } else if (character == QLatin1Char('"')) {
            state = ScanState::DoubleQuoted;
        } else if (character == QLatin1Char('{')) {
            ++braceDepth;
        } else if (character == QLatin1Char('}')) {
            --braceDepth;
            if (braceDepth < 0)
                return false;
        }
    }
    // 文件结束时必须回到普通状态，并且所有左大括号都有对应右大括号。
    return state == ScanState::Normal && braceDepth == 0;
}
}
