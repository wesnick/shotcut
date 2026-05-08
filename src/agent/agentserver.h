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

#ifndef AGENT_AGENTSERVER_H
#define AGENT_AGENTSERVER_H

#include "agentsession.h"

#include <QObject>
#include <QPointer>
#include <QString>

class QWebSocketServer;

namespace Agent {

class Dispatcher;
class AgentEvents;

// Owns a QWebSocketServer and at most one AgentSession (single-client mode).
// Lives on the GUI thread; all dispatch happens synchronously inside Qt's
// signal/slot machinery on this same thread.
class AgentServer : public QObject
{
    Q_OBJECT

public:
    explicit AgentServer(QObject *parent = nullptr);
    ~AgentServer() override;

    // Returns true on successful bind/listen.
    bool start(const QString &bindAddress, int port, const QString &token);
    void stop();

    bool isRunning() const;
    int serverPort() const;
    QString bindAddress() const { return m_bindAddress; }

    // For methods that need to broadcast to the live session (e.g. events).
    AgentSession *currentSession() const { return m_session; }
    Dispatcher *dispatcher() const { return m_dispatcher; }

private slots:
    void onNewConnection();
    void onSessionClosed(AgentSession *session);

private:
    QWebSocketServer *m_server{nullptr};
    QPointer<AgentSession> m_session;
    Dispatcher *m_dispatcher{nullptr};
    AgentEvents *m_events{nullptr};
    QString m_bindAddress;
    QString m_token;
    bool m_allowRemote{false};
};

} // namespace Agent

#endif // AGENT_AGENTSERVER_H
