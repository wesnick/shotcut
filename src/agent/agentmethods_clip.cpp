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

#include "agentdispatcher.h"
#include "commands/timelinecommands.h"
#include "docks/timelinedock.h"
#include "mainwindow.h"
#include "models/multitrackmodel.h"

#include <QJsonObject>
#include <QPoint>
#include <QUndoStack>

namespace Agent {

static QJsonObject paramsObj(const QJsonValue &params)
{
    return params.isObject() ? params.toObject() : QJsonObject();
}

static QJsonValue clipMove(const QJsonValue &params, AgentSession *)
{
    auto *dock = MAIN.timelineDock();
    if (!dock || !dock->model()->tractor())
        throw MethodException(Errors::kNoProjectOpen, QStringLiteral("no project is open"));
    const auto obj = paramsObj(params);
    const int fromTrack = obj.value(QStringLiteral("fromTrack")).toInt(-1);
    const int fromClip = obj.value(QStringLiteral("fromClip")).toInt(-1);
    const int toTrack = obj.value(QStringLiteral("toTrack")).toInt(fromTrack);
    const int toPosition = obj.value(QStringLiteral("toPosition")).toInt(-1);
    if (fromTrack < 0 || fromClip < 0 || toPosition < 0)
        throw MethodException(Errors::kInvalidParams,
                              QStringLiteral("fromTrack, fromClip, toPosition required"));
    auto info = dock->model()->getClipInfo(fromTrack, fromClip);
    if (!info)
        throw MethodException(Errors::kOutOfBounds, QStringLiteral("clip not found"));
    const int positionDelta = toPosition - info->start;
    const int trackDelta = toTrack - fromTrack;
    auto *cmd = new Timeline::MoveClipCommand(*dock, trackDelta, positionDelta, false);
    cmd->addClip(fromTrack, fromClip);
    MAIN.undoStack()->push(cmd);
    QJsonObject result;
    result.insert(QStringLiteral("ok"), true);
    return result;
}

static QJsonValue clipTrimIn(const QJsonValue &params, AgentSession *)
{
    auto *dock = MAIN.timelineDock();
    if (!dock || !dock->model()->tractor())
        throw MethodException(Errors::kNoProjectOpen, QStringLiteral("no project is open"));
    const auto obj = paramsObj(params);
    const int track = obj.value(QStringLiteral("track")).toInt(-1);
    const int clip = obj.value(QStringLiteral("clip")).toInt(-1);
    const int delta = obj.value(QStringLiteral("delta")).toInt(0);
    if (track < 0 || clip < 0)
        throw MethodException(Errors::kInvalidParams, QStringLiteral("track and clip required"));
    if (!dock->trimClipIn(track, clip, clip, delta, false, false))
        throw MethodException(Errors::kFailed, QStringLiteral("trim in failed"));
    QJsonObject result;
    result.insert(QStringLiteral("ok"), true);
    return result;
}

static QJsonValue clipTrimOut(const QJsonValue &params, AgentSession *)
{
    auto *dock = MAIN.timelineDock();
    if (!dock || !dock->model()->tractor())
        throw MethodException(Errors::kNoProjectOpen, QStringLiteral("no project is open"));
    const auto obj = paramsObj(params);
    const int track = obj.value(QStringLiteral("track")).toInt(-1);
    const int clip = obj.value(QStringLiteral("clip")).toInt(-1);
    const int delta = obj.value(QStringLiteral("delta")).toInt(0);
    if (track < 0 || clip < 0)
        throw MethodException(Errors::kInvalidParams, QStringLiteral("track and clip required"));
    if (!dock->trimClipOut(track, clip, delta, false, false))
        throw MethodException(Errors::kFailed, QStringLiteral("trim out failed"));
    QJsonObject result;
    result.insert(QStringLiteral("ok"), true);
    return result;
}

void registerClipMethods(Dispatcher &d)
{
    // The spec uses both timeline.* and clip.* names for some operations; we
    // expose the trim/move surface under both for ergonomics.
    d.registerMethod(QStringLiteral("clip.move"), clipMove);
    d.registerMethod(QStringLiteral("clip.trimIn"), clipTrimIn);
    d.registerMethod(QStringLiteral("clip.trimOut"), clipTrimOut);
    d.registerMethod(QStringLiteral("timeline.moveClip"), clipMove);
    d.registerMethod(QStringLiteral("timeline.trimClipIn"), clipTrimIn);
    d.registerMethod(QStringLiteral("timeline.trimClipOut"), clipTrimOut);
}

} // namespace Agent
