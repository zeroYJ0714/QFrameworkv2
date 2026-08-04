#include "StyleManager.h"

#include <QApplication>
#include <QFile>
#include <QFileInfo>
#include <QTextCodec>

namespace qframework
{
namespace
{
const qint64 kMaximumStyleSheetBytes = 16 * 1024 * 1024;

void setError(QString* errorMessage, const QString& message)
{
    if (errorMessage != nullptr)
        *errorMessage = message;
}
}

StyleManager::StyleManager(QObject* parent)
    : QObject(parent)
{
}

bool StyleManager::loadStyleSheet(const QString& filePath,
                                  QString* errorMessage)
{
    QString styleSheet;
    if (!readStyleSheet(filePath, &styleSheet, errorMessage))
        return false;

    QApplication* application = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (application == nullptr) {
        setError(errorMessage, QString::fromUtf8(u8"当前进程没有 QApplication"));
        return false;
    }

    application->setStyleSheet(styleSheet);
    currentFilePath_ = QFileInfo(filePath).absoluteFilePath();
    currentStyleSheet_ = styleSheet;
    emit styleSheetChanged(currentStyleSheet_);
    return true;
}

bool StyleManager::reloadStyleSheet(QString* errorMessage)
{
    if (currentFilePath_.isEmpty()) {
        setError(errorMessage, QString::fromUtf8(u8"尚未选择 QSS 文件"));
        return false;
    }
    return loadStyleSheet(currentFilePath_, errorMessage);
}

QString StyleManager::currentFilePath() const
{
    return currentFilePath_;
}

QString StyleManager::currentStyleSheet() const
{
    return currentStyleSheet_;
}

bool StyleManager::readStyleSheet(const QString& filePath,
                                  QString* styleSheet,
                                  QString* errorMessage) const
{
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

bool StyleManager::isStructurallyValid(const QString& styleSheet) const
{
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
            if (character == QLatin1Char('*') && next == QLatin1Char('/')) {
                state = ScanState::Normal;
                ++index;
            }
            continue;
        }
        if (state == ScanState::SingleQuoted || state == ScanState::DoubleQuoted) {
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
    return state == ScanState::Normal && braceDepth == 0;
}
}
