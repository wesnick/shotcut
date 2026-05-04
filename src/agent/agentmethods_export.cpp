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
#include "docks/encodedock.h"
#include "jobqueue.h"
#include "jobs/abstractjob.h"
#include "mainwindow.h"

#include <QJsonArray>
#include <QJsonObject>

namespace Agent {

static QJsonObject paramsObj(const QJsonValue &params)
{
    return params.isObject() ? params.toObject() : QJsonObject();
}

static QJsonValue exportPresets(const QJsonValue &, AgentSession *)
{
    auto *dock = MAIN.encodeDock();
    if (!dock)
        throw MethodException(Errors::kFailed, QStringLiteral("encode dock unavailable"));
    QJsonArray arr;
    const auto names = dock->agentPresetNames();
    for (const auto &n : names)
        arr.append(n);
    return arr;
}

static QJsonValue exportStart(const QJsonValue &params, AgentSession *)
{
    auto *dock = MAIN.encodeDock();
    if (!dock)
        throw MethodException(Errors::kFailed, QStringLiteral("encode dock unavailable"));
    const auto obj = paramsObj(params);
    const auto path = obj.value(QStringLiteral("path")).toString();
    if (path.isEmpty())
        throw MethodException(Errors::kInvalidParams, QStringLiteral("path is required"));
    if (!dock->encodeForAgent(path))
        throw MethodException(Errors::kBusy,
                              QStringLiteral("export rejected (already in progress?)"));
    QJsonObject result;
    result.insert(QStringLiteral("ok"), true);
    // Job ID: by convention the most recently added job. The caller can poll
    // export.jobStatus to track it.
    const auto jobs = JOBS.jobs();
    if (!jobs.isEmpty())
        result.insert(QStringLiteral("jobId"), int(jobs.size()) - 1);
    return result;
}

static QJsonValue exportJobStatus(const QJsonValue &params, AgentSession *)
{
    const auto obj = paramsObj(params);
    const int jobId = obj.value(QStringLiteral("jobId")).toInt(-1);
    const auto jobs = JOBS.jobs();
    if (jobId < 0 || jobId >= jobs.size())
        throw MethodException(Errors::kOutOfBounds, QStringLiteral("jobId out of bounds"));
    auto *job = jobs.at(jobId);
    QJsonObject result;
    result.insert(QStringLiteral("jobId"), jobId);
    result.insert(QStringLiteral("label"), job->label());
    result.insert(QStringLiteral("ran"), job->ran());
    result.insert(QStringLiteral("paused"), job->paused());
    result.insert(QStringLiteral("stopped"), job->stopped());
    result.insert(QStringLiteral("isFinished"), job->isFinished());
    result.insert(QStringLiteral("target"), job->target());
    return result;
}

void registerExportMethods(Dispatcher &d)
{
    d.registerMethod(QStringLiteral("export.presets"), exportPresets);
    d.registerMethod(QStringLiteral("export.start"), exportStart);
    d.registerMethod(QStringLiteral("export.jobStatus"), exportJobStatus);
}

} // namespace Agent
