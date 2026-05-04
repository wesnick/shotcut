/*
 * Copyright (c) 2026 Shotcut contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "agentsession.h"

#include "Logger.h"
#include "agentauth.h"
#include "agentdispatcher.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QWebSocket>

namespace Agent {

AgentSession::AgentSession(QWebSocket *socket, const Dispatcher *dispatcher, QObject *parent)
    : QObject(parent)
    , m_socket(socket)
    , m_dispatcher(dispatcher)
{
    socket->setParent(this);
    connect(socket, &QWebSocket::textMessageReceived, this, &AgentSession::onTextMessage);
    connect(socket, &QWebSocket::disconnected, this, &AgentSession::onSocketDisconnected);
    socket->setMaxAllowedIncomingMessageSize(kMaxFrameBytes);
}

AgentSession::~AgentSession() = default;

QHostAddress AgentSession::peerAddress() const
{
    return m_socket ? m_socket->peerAddress() : QHostAddress();
}

void AgentSession::finalizeAuth(const QString &expectedToken, bool allowRemote)
{
    m_expectedToken = expectedToken;
    m_allowRemote = allowRemote;
    m_authFinalized = true;
}

bool AgentSession::isSubscribed(const QString &topic) const
{
    return m_subscriptions.contains(topic);
}

void AgentSession::subscribe(const QString &topic)
{
    m_subscriptions.insert(topic);
}

void AgentSession::unsubscribeAll()
{
    m_subscriptions.clear();
}

void AgentSession::sendNotification(const QString &method, const QJsonObject &params)
{
    if (!m_socket || !m_socket->isValid())
        return;
    if (m_socket->bytesToWrite() > kMaxOutgoingBufferBytes) {
        // Backpressure: drop the notification. We deliberately do not log on
        // every drop to avoid log spam at high event rates.
        return;
    }
    QJsonObject env;
    env.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    env.insert(QStringLiteral("method"), method);
    env.insert(QStringLiteral("params"), params);
    sendRaw(env);
}

void AgentSession::close(const QString &reason)
{
    if (!m_socket)
        return;
    if (!reason.isEmpty())
        m_socket->close(QWebSocketProtocol::CloseCodePolicyViolated, reason);
    else
        m_socket->close();
}

void AgentSession::sendRaw(const QJsonObject &envelope)
{
    if (!m_socket || !m_socket->isValid())
        return;
    const auto bytes = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
    m_socket->sendTextMessage(QString::fromUtf8(bytes));
}

void AgentSession::sendError(const QJsonValue &id, int code, const QString &message)
{
    QJsonObject err;
    err.insert(QStringLiteral("code"), code);
    err.insert(QStringLiteral("message"), message);
    QJsonObject env;
    env.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    env.insert(QStringLiteral("error"), err);
    env.insert(QStringLiteral("id"), id.isUndefined() ? QJsonValue(QJsonValue::Null) : id);
    sendRaw(env);
}

void AgentSession::sendResult(const QJsonValue &id, const QJsonValue &result)
{
    QJsonObject env;
    env.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    env.insert(QStringLiteral("result"), result);
    env.insert(QStringLiteral("id"), id);
    sendRaw(env);
}

bool AgentSession::checkMessageAuth(const QJsonObject &request)
{
    // If no token is configured and the peer is loopback, accept silently.
    if (m_expectedToken.isEmpty() && Auth::isLoopback(peerAddress()))
        return true;
    const QString messageToken = request.value(QStringLiteral("auth")).toString();
    return Auth::validateMessageToken(peerAddress(), messageToken, m_expectedToken, m_allowRemote);
}

void AgentSession::onTextMessage(const QString &message)
{
    QJsonParseError parseError{};
    const auto doc = QJsonDocument::fromJson(message.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        sendError(QJsonValue(QJsonValue::Null),
                  Errors::kParseError,
                  QStringLiteral("invalid JSON: %1").arg(parseError.errorString()));
        return;
    }
    const auto request = doc.object();
    const auto idVal = request.value(QStringLiteral("id"));
    const auto method = request.value(QStringLiteral("method")).toString();
    const auto params = request.value(QStringLiteral("params"));

    if (request.value(QStringLiteral("jsonrpc")).toString() != QStringLiteral("2.0")) {
        sendError(idVal, Errors::kInvalidRequest, QStringLiteral("expected jsonrpc=2.0"));
        return;
    }
    if (method.isEmpty()) {
        sendError(idVal, Errors::kInvalidRequest, QStringLiteral("missing method"));
        return;
    }
    if (!checkMessageAuth(request)) {
        sendError(idVal, Errors::kPermissionDenied, QStringLiteral("authentication required"));
        return;
    }

    try {
        const QJsonValue result = m_dispatcher->invoke(method, params, this);
        if (idVal.isUndefined()) {
            // Per JSON-RPC: requests without id are notifications; do not respond.
            return;
        }
        sendResult(idVal, result);
    } catch (const MethodException &e) {
        sendError(idVal, e.code(), e.message());
    } catch (const std::exception &e) {
        LOG_WARNING() << "[Agent] uncaught exception in" << method << e.what();
        sendError(idVal, Errors::kInternalError, QString::fromUtf8(e.what()));
    } catch (...) {
        LOG_WARNING() << "[Agent] unknown exception in" << method;
        sendError(idVal, Errors::kInternalError, QStringLiteral("internal error"));
    }
}

void AgentSession::onSocketDisconnected()
{
    LOG_INFO() << "[Agent] session disconnected from" << peerAddress().toString();
    emit disconnected(this);
}

} // namespace Agent
