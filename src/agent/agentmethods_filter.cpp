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
#include "controllers/filtercontroller.h"
#include "docks/timelinedock.h"
#include "mainwindow.h"
#include "mltcontroller.h"
#include "models/metadatamodel.h"
#include "models/multitrackmodel.h"
#include "qmltypes/qmlmetadata.h"

#include <Mlt.h>
#include <QJsonArray>
#include <QJsonObject>

namespace Agent {

static QJsonObject paramsObj(const QJsonValue &params)
{
    return params.isObject() ? params.toObject() : QJsonObject();
}

// Resolve the target Mlt::Producer that filters should be read from.
// Caller takes ownership of the returned pointer and must delete it.
static Mlt::Producer *resolveTargetProducer(const QJsonObject &obj)
{
    const auto target = obj.value(QStringLiteral("target")).toString(QStringLiteral("output"));
    if (target == QStringLiteral("output")) {
        if (!MLT.producer() || !MLT.producer()->is_valid())
            throw MethodException(Errors::kNoProjectOpen, QStringLiteral("no project is open"));
        return new Mlt::Producer(*MLT.producer());
    }
    auto *dock = MAIN.timelineDock();
    if (!dock || !dock->model() || !dock->model()->tractor())
        throw MethodException(Errors::kNoProjectOpen, QStringLiteral("timeline not available"));
    auto *model = dock->model();
    const int trackIndex = obj.value(QStringLiteral("track")).toInt(-1);
    if (trackIndex < 0 || trackIndex >= model->trackList().size())
        throw MethodException(Errors::kOutOfBounds, QStringLiteral("track out of bounds"));
    Mlt::Producer *track = model->tractor()->track(model->trackList().at(trackIndex).mlt_index);
    if (!track || !track->is_valid()) {
        delete track;
        throw MethodException(Errors::kFailed, QStringLiteral("could not access track"));
    }
    if (target == QStringLiteral("track"))
        return track;
    if (target == QStringLiteral("clip")) {
        const int clipIndex = obj.value(QStringLiteral("clip")).toInt(-1);
        Mlt::Playlist playlist(*track);
        if (clipIndex < 0 || clipIndex >= playlist.count()) {
            delete track;
            throw MethodException(Errors::kOutOfBounds, QStringLiteral("clip out of bounds"));
        }
        QScopedPointer<Mlt::ClipInfo> info(playlist.clip_info(clipIndex));
        delete track;
        if (!info || !info->producer)
            throw MethodException(Errors::kFailed, QStringLiteral("clip has no producer"));
        return new Mlt::Producer(*info->producer);
    }
    delete track;
    throw MethodException(Errors::kInvalidParams,
                          QStringLiteral("target must be output|track|clip"));
}

static QJsonObject filterToJson(Mlt::Service &service)
{
    QJsonObject obj;
    const char *svc = service.get("mlt_service");
    const char *kdenliveId = service.get("kdenlive_id");
    obj.insert(QStringLiteral("service"), QString::fromUtf8(svc ? svc : ""));
    obj.insert(QStringLiteral("name"),
               QString::fromUtf8(kdenliveId ? kdenliveId : (svc ? svc : "")));
    obj.insert(QStringLiteral("disabled"), service.get_int("disable") ? true : false);
    obj.insert(QStringLiteral("in"), service.get_int("in"));
    obj.insert(QStringLiteral("out"), service.get_int("out"));
    QJsonObject params;
    for (int i = 0; i < service.count(); ++i) {
        const char *name = service.get_name(i);
        if (!name || name[0] == '_')
            continue;
        const char *val = service.get(name);
        params.insert(QString::fromUtf8(name), QString::fromUtf8(val ? val : ""));
    }
    obj.insert(QStringLiteral("params"), params);
    return obj;
}

static QJsonValue filterList(const QJsonValue &params, AgentSession *)
{
    const auto obj = paramsObj(params);
    QScopedPointer<Mlt::Producer> producer(resolveTargetProducer(obj));
    QJsonArray arr;
    for (int i = 0; i < producer->filter_count(); ++i) {
        QScopedPointer<Mlt::Filter> filter(producer->filter(i));
        if (!filter || !filter->is_valid())
            continue;
        QJsonObject f = filterToJson(*filter);
        f.insert(QStringLiteral("filterIndex"), i);
        arr.append(f);
    }
    return arr;
}

static QJsonValue filterMetadata(const QJsonValue &params, AgentSession *)
{
    const auto obj = paramsObj(params);
    const auto service = obj.value(QStringLiteral("service")).toString();
    if (service.isEmpty())
        throw MethodException(Errors::kInvalidParams, QStringLiteral("service is required"));
    auto *fc = MAIN.filterController();
    if (!fc)
        throw MethodException(Errors::kFailed, QStringLiteral("filter controller unavailable"));

    auto *meta = fc->metadata(service);
    if (!meta) {
        // Fallback: search by mlt_service name.
        auto *model = fc->metadataModel();
        for (int i = 0; i < model->rowCount(); ++i) {
            auto *m = model->get(i);
            if (m && m->mlt_service() == service) {
                meta = m;
                break;
            }
        }
    }
    if (!meta)
        throw MethodException(Errors::kInvalidArgument,
                              QStringLiteral("no metadata for service %1").arg(service));

    QJsonObject result;
    result.insert(QStringLiteral("name"), meta->name());
    result.insert(QStringLiteral("service"), meta->mlt_service());
    result.insert(QStringLiteral("isAudio"), meta->isAudio());
    result.insert(QStringLiteral("isHidden"), meta->isHidden());
    result.insert(QStringLiteral("allowMultiple"), meta->allowMultiple());
    result.insert(QStringLiteral("isClipOnly"), meta->isClipOnly());
    result.insert(QStringLiteral("isTrackOnly"), meta->isTrackOnly());
    result.insert(QStringLiteral("isOutputOnly"), meta->isOutputOnly());
    return result;
}

static QJsonValue filterAdd(const QJsonValue &params, AgentSession *)
{
    Q_UNUSED(params);
    // The undo-safe add flow runs through AttachedFiltersModel and constructs
    // a Filter::AddCommand that retains a model reference. To honor the
    // "transient model, no user-selection side effects" constraint without
    // risking dangling references after undo, this entry point is deferred
    // to v1.1. See docs/agent-api.md for the rationale and the recommended
    // workaround (drive the FiltersDock UI via the user, then mutate
    // parameters via filter.setParam once that lands).
    throw MethodException(Errors::kUnsupported,
                          QStringLiteral("filter.add is not implemented in v1"));
}

static QJsonValue filterRemove(const QJsonValue &params, AgentSession *)
{
    Q_UNUSED(params);
    throw MethodException(Errors::kUnsupported,
                          QStringLiteral("filter.remove is not implemented in v1"));
}

static QJsonValue filterSetParam(const QJsonValue &params, AgentSession *)
{
    Q_UNUSED(params);
    throw MethodException(Errors::kUnsupported,
                          QStringLiteral("filter.setParam is not implemented in v1"));
}

static QJsonValue filterSetKeyframe(const QJsonValue &params, AgentSession *)
{
    Q_UNUSED(params);
    throw MethodException(Errors::kUnsupported,
                          QStringLiteral("filter.setKeyframe is not implemented in v1"));
}

void registerFilterMethods(Dispatcher &d)
{
    d.registerMethod(QStringLiteral("filter.list"), filterList);
    d.registerMethod(QStringLiteral("filter.metadata"), filterMetadata);
    d.registerMethod(QStringLiteral("filter.add"), filterAdd);
    d.registerMethod(QStringLiteral("filter.remove"), filterRemove);
    d.registerMethod(QStringLiteral("filter.setParam"), filterSetParam);
    d.registerMethod(QStringLiteral("filter.setKeyframe"), filterSetKeyframe);
}

} // namespace Agent
