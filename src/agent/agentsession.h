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

#ifndef AGENT_AGENTSESSION_H
#define AGENT_AGENTSESSION_H

#include <QHostAddress>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QSet>
#include <QString>

class QWebSocket;

namespace Agent {

class Dispatcher;

// One AgentSession per connected WebSocket. Lives on the GUI thread.
//
// All inbound text frames are parsed as JSON-RPC 2.0 requests/notifications.
// Outbound notifications use sendNotification(); responses are delivered
// directly inside handleTextMessage().
class AgentSession : public QObject
{
    Q_OBJECT

public:
    AgentSession(QWebSocket *socket, const Dispatcher *dispatcher, QObject *parent = nullptr);
    ~AgentSession() override;

    QHostAddress peerAddress() const;
    QWebSocket *socket() const { return m_socket; }
    const Dispatcher *dispatcher() const { return m_dispatcher; }

    // After construction the server calls finalizeAuth() with the token policy
    // resolved. We capture it so per-message validation is consistent.
    void finalizeAuth(const QString &expectedToken, bool allowRemote);

    // Subscriptions: which event topics this client wants pushed.
    bool isSubscribed(const QString &topic) const;
    void subscribe(const QString &topic);
    void unsubscribeAll();

    // Server-pushed JSON-RPC notification. Drops on backpressure: if the
    // outgoing buffer would exceed kMaxOutgoingBufferBytes, the notification
    // is silently discarded (request responses are never dropped).
    void sendNotification(const QString &method, const QJsonObject &params);

    // Also used by the events bridge to coalesce: returns true if we should
    // currently suppress emission of `method` (already pending). Caller is
    // responsible for tracking state — this is just a convenience for the
    // session to maintain its own coalescing if desired in the future.

    // Connection close.
    void close(const QString &reason = QString());

signals:
    void disconnected(AgentSession *self);

private slots:
    void onTextMessage(const QString &message);
    void onSocketDisconnected();

private:
    static constexpr qint64 kMaxOutgoingBufferBytes = 4 * 1024 * 1024;
    static constexpr int kMaxFrameBytes = 16 * 1024 * 1024;

    void sendError(const QJsonValue &id, int code, const QString &message);
    void sendResult(const QJsonValue &id, const QJsonValue &result);
    void sendRaw(const QJsonObject &envelope);
    bool checkMessageAuth(const QJsonObject &request);

    QWebSocket *m_socket;
    const Dispatcher *m_dispatcher;
    QSet<QString> m_subscriptions;
    QString m_expectedToken;
    bool m_allowRemote{false};
    bool m_authFinalized{false};
};

} // namespace Agent

#endif // AGENT_AGENTSESSION_H
