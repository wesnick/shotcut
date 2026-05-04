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

#include "agentevents.h"

#include "Logger.h"
#include "agentserver.h"
#include "agentsession.h"
#include "controllers/filtercontroller.h"
#include "docks/timelinedock.h"
#include "mainwindow.h"
#include "models/multitrackmodel.h"
#include "player.h"

#include <QJsonObject>

namespace Agent {

AgentEvents::AgentEvents(AgentServer *server, QObject *parent)
    : QObject(parent)
    , m_server(server)
{
    m_timelineCoalesce.setSingleShot(true);
    m_timelineCoalesce.setInterval(kCoalesceMs);
    connect(&m_timelineCoalesce, &QTimer::timeout, this, &AgentEvents::onTimelineFlush);

    m_positionCoalesce.setSingleShot(true);
    m_positionCoalesce.setInterval(kPositionCoalesceMs);
    connect(&m_positionCoalesce, &QTimer::timeout, this, &AgentEvents::onPlayerPositionFlush);
}

AgentEvents::~AgentEvents()
{
    unbindFromApplication();
}

void AgentEvents::bindToApplication()
{
    if (m_bound)
        return;
    auto *timelineDock = MAIN.timelineDock();
    if (timelineDock) {
        connect(timelineDock->model(),
                &MultitrackModel::modified,
                this,
                &AgentEvents::onTimelineModified);
        connect(timelineDock,
                &TimelineDock::selectionChanged,
                this,
                &AgentEvents::onSelectionChanged);
    }
    connect(&MAIN, &MainWindow::producerOpened, this, &AgentEvents::onProjectOpened);
    connect(&MAIN, &MainWindow::fileSaved, this, &AgentEvents::onFileSaved);
    connect(&MAIN, &MainWindow::aboutToShutDown, this, &AgentEvents::onAboutToShutDown);

    auto *fc = MAIN.filterController();
    if (fc) {
        connect(fc, &FilterController::filterChanged, this, &AgentEvents::onFilterChanged);
    }
    m_bound = true;
}

void AgentEvents::unbindFromApplication()
{
    if (!m_bound)
        return;
    // disconnect everything we connected
    auto *timelineDock = MAIN.timelineDock();
    if (timelineDock) {
        disconnect(timelineDock->model(), nullptr, this, nullptr);
        disconnect(timelineDock, nullptr, this, nullptr);
    }
    disconnect(&MAIN, nullptr, this, nullptr);
    if (auto *fc = MAIN.filterController())
        disconnect(fc, nullptr, this, nullptr);
    m_bound = false;
}

void AgentEvents::notifyTimelineMutation()
{
    onTimelineModified();
}

void AgentEvents::pushIfSubscribed(const QString &topic,
                                   const QString &method,
                                   const QJsonObject &params)
{
    auto *session = m_server ? m_server->currentSession() : nullptr;
    if (!session || !session->isSubscribed(topic))
        return;
    session->sendNotification(method, params);
}

void AgentEvents::onTimelineModified()
{
    m_timelineDirty = true;
    if (!m_timelineCoalesce.isActive())
        m_timelineCoalesce.start();
}

void AgentEvents::onTimelineFlush()
{
    if (!m_timelineDirty)
        return;
    m_timelineDirty = false;
    ++m_revision;
    QJsonObject params;
    params.insert(QStringLiteral("revision"), static_cast<qint64>(m_revision));
    pushIfSubscribed(QStringLiteral("timeline"), QStringLiteral("timeline.changed"), params);
}

void AgentEvents::onPlayerSeeked(int position)
{
    m_pendingPlayerPosition = position;
    if (!m_positionCoalesce.isActive())
        m_positionCoalesce.start();
}

void AgentEvents::onPlayerPositionFlush()
{
    if (m_pendingPlayerPosition < 0)
        return;
    QJsonObject params;
    params.insert(QStringLiteral("position"), m_pendingPlayerPosition);
    m_pendingPlayerPosition = -1;
    pushIfSubscribed(QStringLiteral("player"), QStringLiteral("player.position"), params);
}

void AgentEvents::onPlayerPlayed(double speed)
{
    QJsonObject params;
    params.insert(QStringLiteral("playing"), true);
    params.insert(QStringLiteral("speed"), speed);
    pushIfSubscribed(QStringLiteral("player"), QStringLiteral("player.stateChanged"), params);
}

void AgentEvents::onPlayerPaused(int position)
{
    QJsonObject params;
    params.insert(QStringLiteral("playing"), false);
    params.insert(QStringLiteral("position"), position);
    pushIfSubscribed(QStringLiteral("player"), QStringLiteral("player.stateChanged"), params);
}

void AgentEvents::onProjectOpened(bool withReopen)
{
    QJsonObject params;
    params.insert(QStringLiteral("withReopen"), withReopen);
    pushIfSubscribed(QStringLiteral("project"), QStringLiteral("project.opened"), params);
}

void AgentEvents::onFileSaved(const QString &path)
{
    QJsonObject params;
    params.insert(QStringLiteral("path"), path);
    pushIfSubscribed(QStringLiteral("project"), QStringLiteral("project.saved"), params);
}

void AgentEvents::onAboutToShutDown()
{
    pushIfSubscribed(QStringLiteral("project"), QStringLiteral("project.closing"), QJsonObject());
}

void AgentEvents::onSelectionChanged()
{
    pushIfSubscribed(QStringLiteral("timeline"), QStringLiteral("selection.changed"), QJsonObject());
}

void AgentEvents::onFilterChanged()
{
    pushIfSubscribed(QStringLiteral("filters"), QStringLiteral("filter.changed"), QJsonObject());
}

} // namespace Agent
