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
#include "agentsession.h"
#include "mainwindow.h"

#include <Mlt.h>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QUndoStack>

namespace Agent {

static QJsonObject paramsObject(const QJsonValue &params)
{
    return params.isObject() ? params.toObject() : QJsonObject();
}

static QJsonValue handleHello(const QJsonValue &params, AgentSession *session)
{
    Q_UNUSED(params);
    Q_UNUSED(session);
    QJsonObject result;
    result.insert(QStringLiteral("shotcutVersion"), QStringLiteral(SHOTCUT_VERSION));
    {
        const int v = LIBMLT_VERSION_INT;
        result.insert(QStringLiteral("mltVersion"),
                      QString::asprintf("%d.%d.%d",
                                        (v >> 16) & 0xff,
                                        (v >> 8) & 0xff,
                                        v & 0xff));
    }
    result.insert(QStringLiteral("apiVersion"), QStringLiteral("1.0.0"));
    QJsonArray caps;
    caps.append(QStringLiteral("project"));
    caps.append(QStringLiteral("timeline"));
    caps.append(QStringLiteral("clip"));
    caps.append(QStringLiteral("filter"));
    caps.append(QStringLiteral("player"));
    caps.append(QStringLiteral("export"));
    caps.append(QStringLiteral("snapshot"));
    caps.append(QStringLiteral("undo"));
    result.insert(QStringLiteral("capabilities"), caps);
    return result;
}

static QJsonValue handleSubscribe(const QJsonValue &params, AgentSession *session)
{
    const auto obj = paramsObject(params);
    const auto topics = obj.value(QStringLiteral("topics")).toArray();
    if (topics.isEmpty())
        throw MethodException(Errors::kInvalidParams,
                              QStringLiteral("expected non-empty topics array"));
    static const QSet<QString> kKnown = {QStringLiteral("timeline"),
                                         QStringLiteral("player"),
                                         QStringLiteral("filters"),
                                         QStringLiteral("project"),
                                         QStringLiteral("log")};
    for (const auto &t : topics) {
        const auto topic = t.toString();
        if (!kKnown.contains(topic))
            throw MethodException(Errors::kInvalidParams,
                                  QStringLiteral("unknown topic: %1").arg(topic));
        session->subscribe(topic);
    }
    QJsonObject result;
    result.insert(QStringLiteral("ok"), true);
    return result;
}

static QJsonValue handlePing(const QJsonValue &params, AgentSession *session)
{
    Q_UNUSED(params);
    Q_UNUSED(session);
    QJsonObject result;
    result.insert(QStringLiteral("pong"), QDateTime::currentMSecsSinceEpoch());
    return result;
}

static QJsonValue handleListMethods(const QJsonValue &params, AgentSession *session)
{
    Q_UNUSED(params);
    QJsonArray methods;
    if (session && session->dispatcher()) {
        const auto names = session->dispatcher()->methodNames();
        for (const auto &n : names)
            methods.append(n);
    }
    return methods;
}

static QJsonValue handleUndo(const QJsonValue &params, AgentSession *session)
{
    Q_UNUSED(params);
    Q_UNUSED(session);
    auto *stack = MAIN.undoStack();
    if (!stack || !stack->canUndo())
        throw MethodException(Errors::kFailed, QStringLiteral("nothing to undo"));
    stack->undo();
    QJsonObject result;
    result.insert(QStringLiteral("ok"), true);
    return result;
}

static QJsonValue handleRedo(const QJsonValue &params, AgentSession *session)
{
    Q_UNUSED(params);
    Q_UNUSED(session);
    auto *stack = MAIN.undoStack();
    if (!stack || !stack->canRedo())
        throw MethodException(Errors::kFailed, QStringLiteral("nothing to redo"));
    stack->redo();
    QJsonObject result;
    result.insert(QStringLiteral("ok"), true);
    return result;
}

static QJsonValue handleDryRun(const QJsonValue &params, AgentSession *session)
{
    Q_UNUSED(params);
    Q_UNUSED(session);
    // Not implemented for v1; document the limitation.
    throw MethodException(Errors::kUnsupported,
                          QStringLiteral("agent.dryRun is not implemented in v1"));
}

void registerAgentMethods(Dispatcher &d)
{
    d.registerMethod(QStringLiteral("agent.hello"), handleHello);
    d.registerMethod(QStringLiteral("agent.subscribe"), handleSubscribe);
    d.registerMethod(QStringLiteral("agent.ping"), handlePing);
    d.registerMethod(QStringLiteral("agent.listMethods"), handleListMethods);
    d.registerMethod(QStringLiteral("agent.undo"), handleUndo);
    d.registerMethod(QStringLiteral("agent.redo"), handleRedo);
    d.registerMethod(QStringLiteral("agent.dryRun"), handleDryRun);
}

} // namespace Agent
