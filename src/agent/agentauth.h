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

#ifndef AGENT_AGENTAUTH_H
#define AGENT_AGENTAUTH_H

#include <QHostAddress>
#include <QString>

class QNetworkRequest;
class QWebSocket;

namespace Agent {

// Authorization helpers. The policy is:
//   * Loopback peers may always connect, with or without a token.
//   * Non-loopback peers require BOTH allowRemote and a non-empty token, and
//     must present that token in an Authorization: Bearer <token> header on
//     the upgrade or in the {"auth": "<token>"} field of every message.
//
// These helpers are pure functions of state (no QObject) so they can be unit
// tested without spinning up a server.
class Auth
{
public:
    static bool isLoopback(const QHostAddress &peer);

    // Returns empty QString on success, or a human-readable error message.
    static QString validateUpgrade(const QHostAddress &peer,
                                   const QString &authorizationHeader,
                                   const QString &expectedToken,
                                   bool allowRemote);

    // Returns true if the per-message auth field is acceptable.
    static bool validateMessageToken(const QHostAddress &peer,
                                     const QString &messageToken,
                                     const QString &expectedToken,
                                     bool allowRemote);

    // Extract the bearer token from an "Authorization: Bearer <token>" header
    // value. Returns empty string if not a bearer header.
    static QString extractBearer(const QString &headerValue);
};

} // namespace Agent

#endif // AGENT_AGENTAUTH_H
