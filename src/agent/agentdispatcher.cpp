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

namespace Agent {

Dispatcher::Dispatcher()
{
    registerAgentMethods(*this);
    registerProjectMethods(*this);
    registerTimelineMethods(*this);
    registerClipMethods(*this);
    registerFilterMethods(*this);
    registerPlayerMethods(*this);
    registerExportMethods(*this);
}

void Dispatcher::registerMethod(const QString &name, MethodHandler handler)
{
    m_methods.insert(name, std::move(handler));
}

bool Dispatcher::hasMethod(const QString &name) const
{
    return m_methods.contains(name);
}

QJsonValue Dispatcher::invoke(const QString &name,
                              const QJsonValue &params,
                              AgentSession *session) const
{
    const auto it = m_methods.constFind(name);
    if (it == m_methods.constEnd())
        throw MethodException(Errors::kMethodNotFound,
                              QStringLiteral("unknown method: %1").arg(name));
    return it.value()(params, session);
}

QStringList Dispatcher::methodNames() const
{
    auto names = m_methods.keys();
    names.sort();
    return names;
}

} // namespace Agent
