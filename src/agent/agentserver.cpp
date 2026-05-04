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

#include "agentserver.h"

#include "Logger.h"
#include "agentauth.h"
#include "agentdispatcher.h"
#include "agentevents.h"
#include "agentsession.h"
#include "settings.h"

#include <QHostAddress>
#include <QNetworkRequest>
#include <QWebSocket>
#include <QWebSocketServer>

namespace Agent {

AgentServer::AgentServer(QObject *parent)
    : QObject(parent)
    , m_dispatcher(new Dispatcher())
    , m_events(new AgentEvents(this, this))
{}

AgentServer::~AgentServer()
{
    stop();
    delete m_dispatcher;
    m_dispatcher = nullptr;
    // m_events is parented to this and deleted by Qt.
}

bool AgentServer::start(const QString &bindAddress, int port, const QString &token)
{
    if (m_server)
        return true;
    m_bindAddress = bindAddress;
    m_token = token;
    m_allowRemote = Settings.agentServerAllowRemote();

    m_server = new QWebSocketServer(QStringLiteral("Shotcut Agent"),
                                    QWebSocketServer::NonSecureMode,
                                    this);
    QHostAddress host(bindAddress);
    if (host.isNull())
        host = QHostAddress::LocalHost;
    if (!m_server->listen(host, static_cast<quint16>(port))) {
        LOG_WARNING() << "[Agent] listen failed:" << m_server->errorString();
        delete m_server;
        m_server = nullptr;
        return false;
    }
    connect(m_server, &QWebSocketServer::newConnection, this, &AgentServer::onNewConnection);
    m_events->bindToApplication();
    return true;
}

void AgentServer::stop()
{
    if (m_events)
        m_events->unbindFromApplication();
    if (m_session) {
        m_session->close(QStringLiteral("server stopping"));
        m_session->deleteLater();
        m_session = nullptr;
    }
    if (m_server) {
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }
}

bool AgentServer::isRunning() const
{
    return m_server && m_server->isListening();
}

int AgentServer::serverPort() const
{
    return m_server ? m_server->serverPort() : 0;
}

void AgentServer::onNewConnection()
{
    if (!m_server)
        return;
    QWebSocket *socket = m_server->nextPendingConnection();
    if (!socket)
        return;

    // Single-client policy: refuse new connections while one is active.
    if (m_session) {
        LOG_INFO() << "[Agent] refusing extra connection from" << socket->peerAddress().toString();
        socket->close(QWebSocketProtocol::CloseCodePolicyViolated,
                      QStringLiteral("agent server is single-client"));
        socket->deleteLater();
        return;
    }

    const auto request = socket->request();
    const QString authHeader = QString::fromUtf8(request.rawHeader("Authorization"));
    const auto error
        = Auth::validateUpgrade(socket->peerAddress(), authHeader, m_token, m_allowRemote);
    if (!error.isEmpty()) {
        LOG_WARNING() << "[Agent] rejecting" << socket->peerAddress().toString() << ":" << error;
        socket->close(QWebSocketProtocol::CloseCodePolicyViolated, error);
        socket->deleteLater();
        return;
    }

    LOG_INFO() << "[Agent] new session from" << socket->peerAddress().toString();
    auto session = new AgentSession(socket, m_dispatcher, this);
    session->finalizeAuth(m_token, m_allowRemote);
    connect(session, &AgentSession::disconnected, this, &AgentServer::onSessionClosed);
    m_session = session;
}

void AgentServer::onSessionClosed(AgentSession *session)
{
    if (m_session == session)
        m_session = nullptr;
    session->deleteLater();
}

} // namespace Agent
