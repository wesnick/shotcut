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

#ifndef AGENT_AGENTDISPATCHER_H
#define AGENT_AGENTDISPATCHER_H

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <functional>

namespace Agent {

class AgentSession;

// Standard JSON-RPC 2.0 error codes plus a Shotcut-specific range.
namespace Errors {
constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
constexpr int kInternalError = -32603;
// -32000 .. -32099: Shotcut domain errors
constexpr int kNoProjectOpen = -32001;
constexpr int kOutOfBounds = -32002;
constexpr int kInvalidArgument = -32003;
constexpr int kBusy = -32004;
constexpr int kFailed = -32005;
constexpr int kUnsupported = -32006;
constexpr int kPermissionDenied = -32010;
} // namespace Errors

// MethodException is thrown by handlers to turn into a JSON-RPC error response
// without writing the boilerplate at every call site.
class MethodException
{
public:
    MethodException(int code, const QString &message)
        : m_code(code)
        , m_message(message)
    {}
    int code() const { return m_code; }
    QString message() const { return m_message; }

private:
    int m_code;
    QString m_message;
};

// A registered method: receives the params object (per JSON-RPC 2.0 the params
// can also be an array — we accept either, but most handlers expect an
// object), the calling session (for ad-hoc subscription state etc.), and
// returns a JSON-encodable result. Throw MethodException on error.
using MethodHandler = std::function<QJsonValue(const QJsonValue &params, AgentSession *session)>;

class Dispatcher
{
public:
    Dispatcher();

    // Register one method. Names use the dotted lowercase convention,
    // e.g. "timeline.appendClip".
    void registerMethod(const QString &name, MethodHandler handler);

    // True if the named method is registered.
    bool hasMethod(const QString &name) const;

    // Dispatch a single method. May throw MethodException.
    QJsonValue invoke(const QString &name, const QJsonValue &params, AgentSession *session) const;

    // Convenience: list all registered method names (for agent.hello capabilities).
    QStringList methodNames() const;

private:
    QHash<QString, MethodHandler> m_methods;
};

// Forward declarations for the per-namespace registration entry points
// implemented in agentmethods_*.cpp. Each populates handlers under its
// dotted prefix.
void registerAgentMethods(Dispatcher &d);
void registerProjectMethods(Dispatcher &d);
void registerTimelineMethods(Dispatcher &d);
void registerClipMethods(Dispatcher &d);
void registerFilterMethods(Dispatcher &d);
void registerPlayerMethods(Dispatcher &d);
void registerExportMethods(Dispatcher &d);

} // namespace Agent

#endif // AGENT_AGENTDISPATCHER_H
