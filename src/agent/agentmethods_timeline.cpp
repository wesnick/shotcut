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

#include "Logger.h"
#include "agentdispatcher.h"
#include "commands/timelinecommands.h"
#include "docks/timelinedock.h"
#include "mainwindow.h"
#include "mltcontroller.h"
#include "models/markersmodel.h"
#include "models/multitrackmodel.h"
#include "shotcut_mlt_properties.h"

#include <Mlt.h>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QUndoStack>

namespace Agent {

static QJsonObject paramsObj(const QJsonValue &params)
{
    return params.isObject() ? params.toObject() : QJsonObject();
}

static MultitrackModel *requireModel()
{
    auto *dock = MAIN.timelineDock();
    if (!dock)
        throw MethodException(Errors::kNoProjectOpen, QStringLiteral("timeline not available"));
    auto *model = dock->model();
    if (!model || !model->tractor())
        throw MethodException(Errors::kNoProjectOpen, QStringLiteral("no project is open"));
    return model;
}

static int requireTrackIndex(MultitrackModel *model, int trackIndex)
{
    if (trackIndex < 0 || trackIndex >= model->trackList().size())
        throw MethodException(Errors::kOutOfBounds,
                              QStringLiteral("trackIndex out of bounds: %1").arg(trackIndex));
    return trackIndex;
}

static QJsonValue timelineTracks(const QJsonValue &, AgentSession *)
{
    auto *model = requireModel();
    QJsonArray arr;
    int i = 0;
    for (const auto &t : model->trackList()) {
        QJsonObject obj;
        obj.insert(QStringLiteral("index"), i);
        obj.insert(QStringLiteral("name"), model->getTrackName(i));
        obj.insert(QStringLiteral("type"),
                   t.type == AudioTrackType ? QStringLiteral("audio") : QStringLiteral("video"));
        Mlt::Producer *track = model->tractor()->track(t.mlt_index);
        if (track && track->is_valid()) {
            obj.insert(QStringLiteral("isMute"), track->get_int("hide") & 2 ? true : false);
            obj.insert(QStringLiteral("isHidden"), track->get_int("hide") & 1 ? true : false);
            obj.insert(QStringLiteral("isLocked"),
                       track->get_int(kTrackLockProperty) ? true : false);
            Mlt::Playlist playlist(*track);
            obj.insert(QStringLiteral("clipCount"), playlist.count());
        }
        delete track;
        arr.append(obj);
        ++i;
    }
    return arr;
}

static QJsonValue timelineClips(const QJsonValue &params, AgentSession *)
{
    auto *model = requireModel();
    const auto obj = paramsObj(params);
    if (!obj.contains(QStringLiteral("trackIndex")))
        throw MethodException(Errors::kInvalidParams, QStringLiteral("trackIndex is required"));
    const int trackIndex = requireTrackIndex(model,
                                             obj.value(QStringLiteral("trackIndex")).toInt(-1));
    const bool includeXml = obj.value(QStringLiteral("includeMltXml")).toBool(false);

    const auto &t = model->trackList().at(trackIndex);
    Mlt::Producer *track = model->tractor()->track(t.mlt_index);
    if (!track || !track->is_valid()) {
        delete track;
        throw MethodException(Errors::kFailed, QStringLiteral("could not access track"));
    }
    Mlt::Playlist playlist(*track);
    QJsonArray arr;
    for (int ci = 0; ci < playlist.count(); ++ci) {
        QJsonObject c;
        QScopedPointer<Mlt::ClipInfo> info(playlist.clip_info(ci));
        if (!info)
            continue;
        c.insert(QStringLiteral("clipIndex"), ci);
        c.insert(QStringLiteral("in"), info->frame_in);
        c.insert(QStringLiteral("out"), info->frame_out);
        c.insert(QStringLiteral("start"), info->start);
        c.insert(QStringLiteral("duration"), info->frame_count);
        c.insert(QStringLiteral("isBlank"), playlist.is_blank(ci));
        if (info->producer && info->producer->is_valid()) {
            const char *caption = info->producer->get("shotcut:caption");
            const char *resource = info->producer->get("resource");
            const char *hash = info->producer->get(kShotcutHashProperty);
            QString name;
            if (caption && *caption)
                name = QString::fromUtf8(caption);
            else if (resource)
                name = QString::fromUtf8(resource);
            c.insert(QStringLiteral("name"), name);
            c.insert(QStringLiteral("resource"), QString::fromUtf8(resource ? resource : ""));
            c.insert(QStringLiteral("hash"), QString::fromUtf8(hash ? hash : ""));
            if (includeXml)
                c.insert(QStringLiteral("mltXml"), MLT.XML(info->producer));
        }
        arr.append(c);
    }
    delete track;
    return arr;
}

static QJsonValue timelineSelection(const QJsonValue &, AgentSession *)
{
    auto *dock = MAIN.timelineDock();
    if (!dock)
        throw MethodException(Errors::kNoProjectOpen, QStringLiteral("timeline not available"));
    QJsonObject result;
    QJsonArray clips;
    for (const auto &p : dock->selection()) {
        QJsonObject c;
        c.insert(QStringLiteral("track"), p.y());
        c.insert(QStringLiteral("clip"), p.x());
        clips.append(c);
    }
    result.insert(QStringLiteral("clips"), clips);
    result.insert(QStringLiteral("currentTrack"), dock->currentTrack());
    result.insert(QStringLiteral("isMultitrackSelected"), dock->isMultitrackSelected());
    return result;
}

static QJsonValue timelineMarkers(const QJsonValue &, AgentSession *)
{
    auto *dock = MAIN.timelineDock();
    if (!dock)
        throw MethodException(Errors::kNoProjectOpen, QStringLiteral("timeline not available"));
    QJsonArray arr;
    for (const auto &m : dock->markersModel()->getMarkers()) {
        QJsonObject obj;
        obj.insert(QStringLiteral("text"), m.text);
        obj.insert(QStringLiteral("start"), m.start);
        obj.insert(QStringLiteral("end"), m.end);
        obj.insert(QStringLiteral("color"), m.color.name());
        arr.append(obj);
    }
    return arr;
}

// Build XML from either a "mltXml" string or a "path" string. In path mode we
// construct an avformat producer at the project profile and serialize it.
static QString buildClipXml(const QJsonObject &obj)
{
    if (obj.contains(QStringLiteral("mltXml")))
        return obj.value(QStringLiteral("mltXml")).toString();
    const auto path = obj.value(QStringLiteral("path")).toString();
    if (path.isEmpty())
        throw MethodException(Errors::kInvalidParams,
                              QStringLiteral("either mltXml or path is required"));
    if (!QFileInfo::exists(path))
        throw MethodException(Errors::kInvalidArgument,
                              QStringLiteral("file not found: %1").arg(path));
    Mlt::Producer producer(MLT.profile(), path.toUtf8().constData());
    if (!producer.is_valid())
        throw MethodException(Errors::kFailed, QStringLiteral("failed to load: %1").arg(path));
    if (obj.contains(QStringLiteral("in")))
        producer.set_in_and_out(obj.value(QStringLiteral("in")).toInt(),
                                obj.value(QStringLiteral("out")).toInt(producer.get_out()));
    return MLT.XML(&producer);
}

static QJsonValue timelineAppendClip(const QJsonValue &params, AgentSession *)
{
    auto *model = requireModel();
    const auto obj = paramsObj(params);
    const int trackIndex = requireTrackIndex(model,
                                             obj.value(QStringLiteral("trackIndex")).toInt(-1));
    const auto xml = buildClipXml(obj);
    MAIN.undoStack()->push(new Timeline::AppendCommand(*model, trackIndex, xml));
    QJsonObject result;
    result.insert(QStringLiteral("ok"), true);
    return result;
}

static QJsonValue timelineInsertClip(const QJsonValue &params, AgentSession *)
{
    auto *model = requireModel();
    auto *dock = MAIN.timelineDock();
    const auto obj = paramsObj(params);
    const int trackIndex = requireTrackIndex(model,
                                             obj.value(QStringLiteral("trackIndex")).toInt(-1));
    const int position = obj.value(QStringLiteral("position")).toInt(dock->position());
    const auto xml = buildClipXml(obj);
    MAIN.undoStack()->push(
        new Timeline::InsertCommand(*model, *dock->markersModel(), trackIndex, position, xml));
    QJsonObject result;
    result.insert(QStringLiteral("ok"), true);
    return result;
}

static QJsonValue timelineOverwriteClip(const QJsonValue &params, AgentSession *)
{
    auto *model = requireModel();
    auto *dock = MAIN.timelineDock();
    const auto obj = paramsObj(params);
    const int trackIndex = requireTrackIndex(model,
                                             obj.value(QStringLiteral("trackIndex")).toInt(-1));
    const int position = obj.value(QStringLiteral("position")).toInt(dock->position());
    const auto xml = buildClipXml(obj);
    MAIN.undoStack()->push(new Timeline::OverwriteCommand(*model, trackIndex, position, xml));
    QJsonObject result;
    result.insert(QStringLiteral("ok"), true);
    return result;
}

static void requireClipIndex(MultitrackModel *model, int trackIndex, int clipIndex)
{
    Mlt::Producer *track = model->tractor()->track(model->trackList().at(trackIndex).mlt_index);
    if (!track || !track->is_valid()) {
        delete track;
        throw MethodException(Errors::kFailed, QStringLiteral("could not access track"));
    }
    Mlt::Playlist playlist(*track);
    if (clipIndex < 0 || clipIndex >= playlist.count()) {
        delete track;
        throw MethodException(Errors::kOutOfBounds,
                              QStringLiteral("clipIndex out of bounds: %1").arg(clipIndex));
    }
    delete track;
}

static QJsonValue timelineRemoveClip(const QJsonValue &params, AgentSession *)
{
    auto *model = requireModel();
    auto *dock = MAIN.timelineDock();
    const auto obj = paramsObj(params);
    const int trackIndex = requireTrackIndex(model,
                                             obj.value(QStringLiteral("trackIndex")).toInt(-1));
    const int clipIndex = obj.value(QStringLiteral("clipIndex")).toInt(-1);
    requireClipIndex(model, trackIndex, clipIndex);
    MAIN.undoStack()->push(
        new Timeline::RemoveCommand(*model, *dock->markersModel(), trackIndex, clipIndex));
    QJsonObject result;
    result.insert(QStringLiteral("ok"), true);
    return result;
}

static QJsonValue timelineLiftClip(const QJsonValue &params, AgentSession *)
{
    auto *model = requireModel();
    const auto obj = paramsObj(params);
    const int trackIndex = requireTrackIndex(model,
                                             obj.value(QStringLiteral("trackIndex")).toInt(-1));
    const int clipIndex = obj.value(QStringLiteral("clipIndex")).toInt(-1);
    requireClipIndex(model, trackIndex, clipIndex);
    MAIN.undoStack()->push(new Timeline::LiftCommand(*model, trackIndex, clipIndex));
    QJsonObject result;
    result.insert(QStringLiteral("ok"), true);
    return result;
}

static QJsonValue timelineSplitClip(const QJsonValue &params, AgentSession *)
{
    auto *model = requireModel();
    auto *dock = MAIN.timelineDock();
    const auto obj = paramsObj(params);
    const int trackIndex = requireTrackIndex(model,
                                             obj.value(QStringLiteral("trackIndex")).toInt(-1));
    const int position = obj.value(QStringLiteral("position")).toInt(dock->position());
    const int clipIndex = dock->clipIndexAtPosition(trackIndex, position);
    if (clipIndex < 0)
        throw MethodException(Errors::kOutOfBounds,
                              QStringLiteral("no clip at position %1 on track %2")
                                  .arg(position)
                                  .arg(trackIndex));
    std::vector<int> tracks{trackIndex};
    std::vector<int> clips{clipIndex};
    MAIN.undoStack()->push(new Timeline::SplitCommand(*model, tracks, clips, position));
    QJsonObject result;
    result.insert(QStringLiteral("ok"), true);
    return result;
}

static QJsonValue timelineAddTrack(const QJsonValue &params, AgentSession *)
{
    auto *model = requireModel();
    const auto obj = paramsObj(params);
    const auto type = obj.value(QStringLiteral("type")).toString(QStringLiteral("video")).toLower();
    if (type != QStringLiteral("video") && type != QStringLiteral("audio"))
        throw MethodException(Errors::kInvalidParams, QStringLiteral("type must be video or audio"));
    MAIN.undoStack()->push(new Timeline::AddTrackCommand(*model, type == QStringLiteral("video")));
    QJsonObject result;
    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("trackIndex"), int(model->trackList().size()) - 1);
    return result;
}

static QJsonValue timelineRemoveTrack(const QJsonValue &params, AgentSession *)
{
    auto *model = requireModel();
    const auto obj = paramsObj(params);
    const int trackIndex = requireTrackIndex(model,
                                             obj.value(QStringLiteral("trackIndex")).toInt(-1));
    MAIN.undoStack()->push(new Timeline::RemoveTrackCommand(*model, trackIndex));
    QJsonObject result;
    result.insert(QStringLiteral("ok"), true);
    return result;
}

static QJsonValue timelineSetTrackProperty(const QJsonValue &params, AgentSession *)
{
    auto *model = requireModel();
    const auto obj = paramsObj(params);
    const int trackIndex = requireTrackIndex(model,
                                             obj.value(QStringLiteral("trackIndex")).toInt(-1));
    const auto prop = obj.value(QStringLiteral("property")).toString();
    const auto val = obj.value(QStringLiteral("value"));
    auto *stack = MAIN.undoStack();
    if (prop == QStringLiteral("name")) {
        stack->push(new Timeline::NameTrackCommand(*model, trackIndex, val.toString()));
    } else if (prop == QStringLiteral("mute")) {
        // MuteTrackCommand toggles state; the boolean value is informational
        // for agents but does not change the toggle semantics.
        stack->push(new Timeline::MuteTrackCommand(*model, trackIndex));
    } else if (prop == QStringLiteral("hidden")) {
        stack->push(new Timeline::HideTrackCommand(*model, trackIndex));
    } else if (prop == QStringLiteral("locked")) {
        stack->push(new Timeline::LockTrackCommand(*model, trackIndex, val.toBool()));
    } else if (prop == QStringLiteral("composite")) {
        stack->push(new Timeline::CompositeTrackCommand(*model, trackIndex, val.toBool()));
    } else {
        throw MethodException(Errors::kInvalidParams,
                              QStringLiteral("unknown track property: %1").arg(prop));
    }
    QJsonObject result;
    result.insert(QStringLiteral("ok"), true);
    return result;
}

static QJsonValue timelineSelect(const QJsonValue &params, AgentSession *)
{
    auto *dock = MAIN.timelineDock();
    if (!dock)
        throw MethodException(Errors::kNoProjectOpen, QStringLiteral("timeline not available"));
    const auto obj = paramsObj(params);
    QList<QPoint> selection;
    if (obj.contains(QStringLiteral("clip")) && obj.contains(QStringLiteral("track"))) {
        selection.append(QPoint(obj.value(QStringLiteral("clip")).toInt(),
                                obj.value(QStringLiteral("track")).toInt()));
    } else if (obj.contains(QStringLiteral("ranges"))) {
        for (const auto &r : obj.value(QStringLiteral("ranges")).toArray()) {
            const auto rObj = r.toObject();
            selection.append(QPoint(rObj.value(QStringLiteral("clip")).toInt(),
                                    rObj.value(QStringLiteral("track")).toInt()));
        }
    }
    dock->setSelection(selection, -1, false);
    QJsonObject result;
    result.insert(QStringLiteral("ok"), true);
    return result;
}

void registerTimelineMethods(Dispatcher &d)
{
    d.registerMethod(QStringLiteral("timeline.tracks"), timelineTracks);
    d.registerMethod(QStringLiteral("timeline.clips"), timelineClips);
    d.registerMethod(QStringLiteral("timeline.selection"), timelineSelection);
    d.registerMethod(QStringLiteral("timeline.markers"), timelineMarkers);
    d.registerMethod(QStringLiteral("timeline.appendClip"), timelineAppendClip);
    d.registerMethod(QStringLiteral("timeline.insertClip"), timelineInsertClip);
    d.registerMethod(QStringLiteral("timeline.overwriteClip"), timelineOverwriteClip);
    d.registerMethod(QStringLiteral("timeline.removeClip"), timelineRemoveClip);
    d.registerMethod(QStringLiteral("timeline.liftClip"), timelineLiftClip);
    d.registerMethod(QStringLiteral("timeline.splitClip"), timelineSplitClip);
    d.registerMethod(QStringLiteral("timeline.addTrack"), timelineAddTrack);
    d.registerMethod(QStringLiteral("timeline.removeTrack"), timelineRemoveTrack);
    d.registerMethod(QStringLiteral("timeline.setTrackProperty"), timelineSetTrackProperty);
    d.registerMethod(QStringLiteral("timeline.select"), timelineSelect);
}

} // namespace Agent
