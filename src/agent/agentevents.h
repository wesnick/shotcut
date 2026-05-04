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

#ifndef AGENT_AGENTEVENTS_H
#define AGENT_AGENTEVENTS_H

#include <QObject>
#include <QTimer>

namespace Agent {

class AgentServer;

// Bridges Shotcut Qt signals (MultitrackModel::modified, Player::seeked, ...)
// into JSON-RPC notifications on the connected agent session.
//
// Coalescing rules:
//   * timeline.changed: at most one per kCoalesceMs (~33ms / 30Hz).
//   * player.position : at most one per kPositionCoalesceMs (~33ms).
//   * other signals    : forwarded directly.
//
// Subscriptions are honored on the AgentSession side. A topic must be
// in the session's subscription set before notifications under that
// topic are pushed.
class AgentEvents : public QObject
{
    Q_OBJECT

public:
    explicit AgentEvents(AgentServer *server, QObject *parent = nullptr);
    ~AgentEvents() override;

    // Connect to MAIN, MLT.*, MultitrackModel, Player. Idempotent.
    void bindToApplication();
    void unbindFromApplication();

    // Used by mutating method handlers to indicate something changed
    // outside of a signal we already listen to (e.g. agent.undo without
    // a fresh modified() emission). Increments the revision counter.
    void notifyTimelineMutation();
    quint64 revision() const { return m_revision; }

private slots:
    void onTimelineModified();
    void onTimelineFlush();
    void onPlayerSeeked(int position);
    void onPlayerPositionFlush();
    void onPlayerPlayed(double speed);
    void onPlayerPaused(int position);
    void onProjectOpened(bool withReopen);
    void onFileSaved(const QString &path);
    void onAboutToShutDown();
    void onSelectionChanged();
    void onFilterChanged();

private:
    static constexpr int kCoalesceMs = 33;
    static constexpr int kPositionCoalesceMs = 33;

    void pushIfSubscribed(const QString &topic, const QString &method, const QJsonObject &params);

    AgentServer *m_server;
    bool m_bound{false};
    quint64 m_revision{0};

    QTimer m_timelineCoalesce;
    QTimer m_positionCoalesce;
    int m_pendingPlayerPosition{-1};
    bool m_timelineDirty{false};
};

} // namespace Agent

#endif // AGENT_AGENTEVENTS_H
