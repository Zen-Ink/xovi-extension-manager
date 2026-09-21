#pragma once
#include "diagnostics.h"
#include <QJsonDocument>
#include <QJsonObject>
inline QJsonObject diagnosticObject(const QString &code, const QString &detail={}, const QString &context="operation") {
    const auto json=xem::diagnosticJson(xem::classify(code.toStdString(),detail.toStdString(),context.toStdString()));
    return QJsonDocument::fromJson(QByteArray::fromStdString(json)).object();
}
inline QJsonObject withDiagnostic(QJsonObject result) {
    if(result.value("ok").isBool() && !result.value("ok").toBool() && !result.contains("diagnostic"))
        result.insert("diagnostic",diagnosticObject(result.value("error").toString(),result.value("message").toString()));
    return result;
}
