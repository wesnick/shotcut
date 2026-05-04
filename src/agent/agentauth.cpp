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

#include "agentauth.h"

namespace Agent {

bool Auth::isLoopback(const QHostAddress &peer)
{
    return peer.isLoopback();
}

QString Auth::extractBearer(const QString &headerValue)
{
    static const QString kPrefix = QStringLiteral("Bearer ");
    if (!headerValue.startsWith(kPrefix, Qt::CaseInsensitive))
        return QString();
    return headerValue.mid(kPrefix.size()).trimmed();
}

QString Auth::validateUpgrade(const QHostAddress &peer,
                              const QString &authorizationHeader,
                              const QString &expectedToken,
                              bool allowRemote)
{
    const bool loopback = isLoopback(peer);
    if (!loopback && !allowRemote)
        return QStringLiteral("non-loopback connection rejected (allowRemote disabled)");

    // If a token is configured, require it from everyone.
    if (!expectedToken.isEmpty()) {
        const QString presented = extractBearer(authorizationHeader);
        if (presented.isEmpty() || presented != expectedToken)
            return QStringLiteral("missing or invalid bearer token");
    } else if (!loopback) {
        // Remote connections without a token are never allowed.
        return QStringLiteral("remote connections require a token");
    }
    return QString();
}

bool Auth::validateMessageToken(const QHostAddress &peer,
                                const QString &messageToken,
                                const QString &expectedToken,
                                bool allowRemote)
{
    Q_UNUSED(allowRemote);
    if (expectedToken.isEmpty()) {
        // No token configured: only loopback peers may speak. Non-loopback
        // peers can never reach this code path because the upgrade was
        // rejected, but defend in depth.
        return isLoopback(peer);
    }
    return messageToken == expectedToken;
}

} // namespace Agent
