// SPDX-License-Identifier: GPL-2.0-or-later
#include "psd_adjustment_export.h"

#include <functional>

#include <QDataStream>
#include <QStringList>

#include <filter/kis_filter_configuration.h>

namespace
{
QByteArray bigEndian(const std::function<void(QDataStream &)> &write)
{
    QByteArray out;
    QDataStream stream(&out, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    write(stream);
    return out;
}

QByteArray padded(QByteArray data)
{
    while (data.size() % 4) data.append(char(0));
    return data;
}

// Krita's curve text ("x,y;x,y;", both 0 to 1) as points
QList<QPointF> readCurve(const QString &text)
{
    QList<QPointF> points;
    for (const QString &pair : text.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        const QStringList xy = pair.split(QLatin1Char(','));
        if (xy.size() == 2) points << QPointF(xy.at(0).toDouble(), xy.at(1).toDouble());
    }
    return points;
}

bool isIdentity(const QList<QPointF> &points)
{
    if (points.size() < 2) return true;
    for (const QPointF &p : points) {
        if (qAbs(p.x() - p.y()) > 1.0 / 512.0) return false;
    }
    return true;
}
}

QByteArray PsdAdjustmentExport::curvesBlock(const QList<QList<QPointF>> &channels)
{
    // Adobe's Curves block: a byte, a version (1), a bit mask of the channels that follow (0 = composite, 1 to 3 = red, green, blue), then for each
    // channel a count and (output, input) pairs on 0 to 255, then the extension "Crv " with its version (4) and no extra curves
    quint32 bits = 0;
    for (int i = 0; i < channels.size() && i < 4; ++i) {
        if (!channels.at(i).isEmpty()) bits |= (1u << i);
    }
    return padded(bigEndian([&](QDataStream &s) {
        s << quint8(0) << quint16(1) << bits;
        for (int i = 0; i < channels.size() && i < 4; ++i) {
            if (channels.at(i).isEmpty()) continue;
            s << quint16(channels.at(i).size());
            for (const QPointF &p : channels.at(i)) {
                s << quint16(qBound(0, qRound(p.y() * 255.0), 255)) << quint16(qBound(0, qRound(p.x() * 255.0), 255));
            }
        }
        s.writeRawData("Crv ", 4);
        s << quint16(4) << quint16(0);
    }));
}

QByteArray PsdAdjustmentExport::hueSaturationBlock(bool colorize, int hue, int saturation, int lightness)
{
    // version 2, the colorize flag, a pad byte, the colorize values and the master values (hue, saturation, lightness as shorts), then Photoshop's six
    // colour ranges with their four range points and their own three values (all left at zero)
    return padded(bigEndian([&](QDataStream &s) {
        s << quint16(2) << quint8(colorize ? 1 : 0) << quint8(0);
        if (colorize) {
            s << qint16(hue) << qint16(saturation) << qint16(lightness) << qint16(0) << qint16(0) << qint16(0);
        } else {
            s << qint16(0) << qint16(0) << qint16(0) << qint16(hue) << qint16(saturation) << qint16(lightness);
        }
        static const qint16 ranges[6][4] = {{315, 345, 15, 45}, {15, 45, 75, 105}, {75, 105, 135, 165}, {135, 165, 195, 225}, {195, 225, 255, 285}, {255, 285, 315, 345}};
        for (const auto &range : ranges) {
            for (qint16 point : range) s << point;
            s << qint16(0) << qint16(0) << qint16(0);
        }
    }));
}

QByteArray PsdAdjustmentExport::colorBalanceBlock(const int values[9], bool preserveLuminosity)
{
    return padded(bigEndian([&](QDataStream &s) {
        for (int i = 0; i < 9; ++i) s << qint16(qBound(-100, values[i], 100));
        s << quint8(preserveLuminosity ? 1 : 0) << quint8(0);
    }));
}

bool PsdAdjustmentExport::encode(KisFilterConfigurationSP config, QByteArray &key, QByteArray &data)
{
    key.clear();
    data.clear();
    if (!config) return false;
    const QString name = config->name();

    if (name == QLatin1String("perchannel")) {
        QList<QList<QPointF>> channels;
        for (int i = 0; i < 4; ++i) {
            const QList<QPointF> points = readCurve(config->getString(QStringLiteral("curve%1").arg(i)));
            channels << (isIdentity(points) ? QList<QPointF>() : points);
        }
        if (channels.at(0).isEmpty() && channels.at(1).isEmpty() && channels.at(2).isEmpty() && channels.at(3).isEmpty()) {
            channels[0] = {QPointF(0, 0), QPointF(1, 1)};
        }
        key = "curv";
        data = curvesBlock(channels);
        return true;
    }
    if (name == QLatin1String("hsvadjustment")) {
        key = "hue2";
        data = hueSaturationBlock(config->getBool(QStringLiteral("colorize"), false), config->getInt(QStringLiteral("h"), 0), config->getInt(QStringLiteral("s"), 0),
                                  config->getInt(QStringLiteral("v"), 0));
        return true;
    }
    if (name == QLatin1String("invert")) {
        key = "nvrt";
        data = QByteArray();
        return true;
    }
    if (name == QLatin1String("posterize")) {
        key = "post";
        const int steps = qBound(2, config->getInt(QStringLiteral("steps"), 4), 255);
        data = padded(bigEndian([&](QDataStream &s) { s << quint16(steps); }));
        return true;
    }
    if (name == QLatin1String("threshold")) {
        key = "thrs";
        const int level = qBound(1, config->getInt(QStringLiteral("threshold"), 128), 255);
        data = padded(bigEndian([&](QDataStream &s) { s << quint16(level); }));
        return true;
    }
    if (name == QLatin1String("colorbalance")) {
        int values[9];
        const char *ranges[3] = {"shadows", "midtones", "highlights"};
        const char *axes[3] = {"cyan_red", "magenta_green", "yellow_blue"};
        for (int r = 0; r < 3; ++r) {
            for (int a = 0; a < 3; ++a) values[r * 3 + a] = config->getInt(QStringLiteral("%1_%2").arg(QLatin1String(axes[a]), QLatin1String(ranges[r])), 0);
        }
        key = "blnc";
        data = colorBalanceBlock(values, config->getBool(QStringLiteral("preserve_luminosity"), true));
        return true;
    }
    return false;
}
