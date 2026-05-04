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
#include "mainwindow.h"
#include "mltcontroller.h"

#include <Mlt.h>
#include <QBuffer>
#include <QByteArray>
#include <QImage>
#include <QJsonObject>

namespace Agent {

static QJsonObject paramsObj(const QJsonValue &params)
{
    return params.isObject() ? params.toObject() : QJsonObject();
}

static int parsePosition(const QJsonValue &v)
{
    if (v.isDouble())
        return v.toInt();
    if (v.isString()) {
        // Accept timecode via Mlt's parser.
        Mlt::Properties p;
        p.set("_t", v.toString().toUtf8().constData());
        return p.get_int("_t");
    }
    throw MethodException(Errors::kInvalidParams,
                          QStringLiteral("expected position frame or timecode"));
}

static QJsonValue playerState(const QJsonValue &, AgentSession *)
{
    QJsonObject result;
    if (!MLT.producer() || !MLT.producer()->is_valid()) {
        result.insert(QStringLiteral("hasProducer"), false);
        return result;
    }
    result.insert(QStringLiteral("hasProducer"), true);
    result.insert(QStringLiteral("position"), MLT.producer()->position());
    result.insert(QStringLiteral("isPlaying"), !MLT.isPaused());
    result.insert(QStringLiteral("in"), MLT.producer()->get_in());
    result.insert(QStringLiteral("out"), MLT.producer()->get_out());
    result.insert(QStringLiteral("fps"), MLT.profile().fps());
    return result;
}

static QJsonValue playerPlay(const QJsonValue &params, AgentSession *)
{
    const auto obj = paramsObj(params);
    double speed = 1.0;
    if (obj.contains(QStringLiteral("speed")))
        speed = obj.value(QStringLiteral("speed")).toDouble(1.0);
    MLT.play(speed);
    QJsonObject result;
    result.insert(QStringLiteral("ok"), true);
    return result;
}

static QJsonValue playerPause(const QJsonValue &, AgentSession *)
{
    MLT.pause();
    QJsonObject result;
    result.insert(QStringLiteral("ok"), true);
    return result;
}

static QJsonValue playerSeek(const QJsonValue &params, AgentSession *)
{
    const auto obj = paramsObj(params);
    if (!obj.contains(QStringLiteral("position")))
        throw MethodException(Errors::kInvalidParams, QStringLiteral("position is required"));
    const int frame = parsePosition(obj.value(QStringLiteral("position")));
    MLT.seek(frame);
    QJsonObject result;
    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("position"), frame);
    return result;
}

static QJsonValue playerSetIn(const QJsonValue &params, AgentSession *)
{
    const auto obj = paramsObj(params);
    if (!obj.contains(QStringLiteral("position")))
        throw MethodException(Errors::kInvalidParams, QStringLiteral("position is required"));
    MLT.setIn(parsePosition(obj.value(QStringLiteral("position"))));
    QJsonObject result;
    result.insert(QStringLiteral("ok"), true);
    return result;
}

static QJsonValue playerSetOut(const QJsonValue &params, AgentSession *)
{
    const auto obj = paramsObj(params);
    if (!obj.contains(QStringLiteral("position")))
        throw MethodException(Errors::kInvalidParams, QStringLiteral("position is required"));
    MLT.setOut(parsePosition(obj.value(QStringLiteral("position"))));
    QJsonObject result;
    result.insert(QStringLiteral("ok"), true);
    return result;
}

static QJsonValue playerSnapshot(const QJsonValue &params, AgentSession *)
{
    if (!MLT.producer() || !MLT.producer()->is_valid())
        throw MethodException(Errors::kNoProjectOpen, QStringLiteral("no project is open"));
    const auto obj = paramsObj(params);
    QString format = obj.value(QStringLiteral("format")).toString(QStringLiteral("png")).toLower();
    if (format != QStringLiteral("png") && format != QStringLiteral("jpeg"))
        throw MethodException(Errors::kInvalidParams, QStringLiteral("format must be png or jpeg"));

    int frame = MLT.producer()->position();
    if (obj.contains(QStringLiteral("position")))
        frame = parsePosition(obj.value(QStringLiteral("position")));

    auto &profile = MLT.profile();
    int width = obj.value(QStringLiteral("width")).toInt(profile.width());
    if (width <= 0 || width > 8192)
        throw MethodException(Errors::kInvalidParams, QStringLiteral("invalid width"));
    int height = profile.width() > 0 ? width * profile.height() / profile.width()
                                     : profile.height();

    QImage image = MLT.image(*MLT.producer(), frame, width, height);
    if (image.isNull())
        throw MethodException(Errors::kFailed, QStringLiteral("frame extraction failed"));

    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, format == QStringLiteral("png") ? "PNG" : "JPEG"))
        throw MethodException(Errors::kFailed, QStringLiteral("image encode failed"));

    QJsonObject result;
    result.insert(QStringLiteral("format"), format);
    result.insert(QStringLiteral("width"), image.width());
    result.insert(QStringLiteral("height"), image.height());
    result.insert(QStringLiteral("position"), frame);
    result.insert(QStringLiteral("data"), QString::fromLatin1(bytes.toBase64()));
    return result;
}

void registerPlayerMethods(Dispatcher &d)
{
    d.registerMethod(QStringLiteral("player.state"), playerState);
    d.registerMethod(QStringLiteral("player.play"), playerPlay);
    d.registerMethod(QStringLiteral("player.pause"), playerPause);
    d.registerMethod(QStringLiteral("player.seek"), playerSeek);
    d.registerMethod(QStringLiteral("player.setIn"), playerSetIn);
    d.registerMethod(QStringLiteral("player.setOut"), playerSetOut);
    d.registerMethod(QStringLiteral("player.snapshot"), playerSnapshot);
}

} // namespace Agent
