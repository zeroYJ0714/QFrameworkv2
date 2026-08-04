#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include "QFrameworkGlobal.h"

namespace qframework
{
namespace process
{
enum class FrameResult
{
    Incomplete,
    Ready,
    Invalid
};

QFRAMEWORK_EXPORT QByteArray encodeFrame(const QJsonObject& object);
QFRAMEWORK_EXPORT FrameResult takeFrame(QByteArray* buffer,
                                        QJsonObject* object,
                                        int maxFrameBytes,
                                        QString* errorMessage = nullptr);
}
}
