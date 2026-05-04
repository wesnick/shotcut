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
#include "mainwindow.h"
#include "mltcontroller.h"

#include <Mlt.h>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QMetaObject>
#include <QString>
#include <QTimer>
#include <QUndoStack>

namespace Agent {

static QJsonObject paramsObj(const QJsonValue &params)
{
    return params.isObject() ? params.toObject() : QJsonObject();
}

static QJsonValue projectState(const QJsonValue &, AgentSession *)
{
    QJsonObject result;
    result.insert(QStringLiteral("file"), MAIN.fileName());
    result.insert(QStringLiteral("dirty"), MAIN.isWindowModified());

    QJsonObject profile;
    auto &p = MLT.profile();
    profile.insert(QStringLiteral("width"), p.width());
    profile.insert(QStringLiteral("height"), p.height());
    profile.insert(QStringLiteral("fps"), p.fps());
    profile.insert(QStringLiteral("sar"),
                   p.sample_aspect_den() > 0 ? double(p.sample_aspect_num()) / p.sample_aspect_den()
                                             : 1.0);
    profile.insert(QStringLiteral("colorspace"), p.colorspace());
    profile.insert(QStringLiteral("progressive"), p.progressive());
    result.insert(QStringLiteral("profile"), profile);

    if (MLT.producer() && MLT.producer()->is_valid()) {
        result.insert(QStringLiteral("duration"), MLT.producer()->get_length());
        result.insert(QStringLiteral("resource"), MLT.resource());
    }

    auto *stack = MAIN.undoStack();
    QJsonObject undo;
    undo.insert(QStringLiteral("canUndo"), stack && stack->canUndo());
    undo.insert(QStringLiteral("canRedo"), stack && stack->canRedo());
    undo.insert(QStringLiteral("count"), stack ? stack->count() : 0);
    undo.insert(QStringLiteral("index"), stack ? stack->index() : 0);
    result.insert(QStringLiteral("undo"), undo);

    return result;
}

static QJsonValue projectOpen(const QJsonValue &params, AgentSession *)
{
    const auto obj = paramsObj(params);
    const auto path = obj.value(QStringLiteral("path")).toString();
    if (path.isEmpty())
        throw MethodException(Errors::kInvalidParams, QStringLiteral("path is required"));
    if (!QFileInfo::exists(path))
        throw MethodException(Errors::kInvalidArgument,
                              QStringLiteral("file not found: %1").arg(path));

    // Run on the GUI thread; use a queued single-shot so we return immediately
    // and the user-visible "save unsaved changes" dialog is allowed to surface.
    QTimer::singleShot(0, &MAIN, [path]() { MAIN.open(path); });

    QJsonObject result;
    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("queued"), true);
    return result;
}

static QJsonValue projectSave(const QJsonValue &params, AgentSession *)
{
    const auto obj = paramsObj(params);
    const auto path = obj.value(QStringLiteral("path")).toString();
    bool ok = false;
    if (!path.isEmpty()) {
        ok = MAIN.saveXML(path);
    } else {
        ok = MAIN.on_actionSave_triggered();
    }
    if (!ok)
        throw MethodException(Errors::kFailed, QStringLiteral("save failed"));
    QJsonObject result;
    result.insert(QStringLiteral("ok"), true);
    return result;
}

static QJsonValue projectClose(const QJsonValue &, AgentSession *)
{
    QTimer::singleShot(0, &MAIN, []() {
        QMetaObject::invokeMethod(&MAIN, "on_actionClose_triggered");
    });
    QJsonObject result;
    result.insert(QStringLiteral("ok"), true);
    return result;
}

static QJsonValue projectNew(const QJsonValue &, AgentSession *)
{
    QTimer::singleShot(0, &MAIN, []() {
        QMetaObject::invokeMethod(&MAIN, "on_actionNew_triggered");
    });
    QJsonObject result;
    result.insert(QStringLiteral("ok"), true);
    return result;
}

static QJsonValue projectGetMltXml(const QJsonValue &, AgentSession *)
{
    if (!MLT.producer() || !MLT.producer()->is_valid())
        throw MethodException(Errors::kNoProjectOpen, QStringLiteral("no project is open"));
    const auto xml = MLT.XML();
    QJsonObject result;
    result.insert(QStringLiteral("xml"), xml);
    result.insert(QStringLiteral("length"), xml.size());
    return result;
}

void registerProjectMethods(Dispatcher &d)
{
    d.registerMethod(QStringLiteral("project.state"), projectState);
    d.registerMethod(QStringLiteral("project.open"), projectOpen);
    d.registerMethod(QStringLiteral("project.save"), projectSave);
    d.registerMethod(QStringLiteral("project.close"), projectClose);
    d.registerMethod(QStringLiteral("project.new"), projectNew);
    d.registerMethod(QStringLiteral("project.getMltXml"), projectGetMltXml);
}

} // namespace Agent
