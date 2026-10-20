// SPDX-License-Identifier: GPL-2.0-or-later
// CONCEPTFORGE: Photoshop brush features the pixel brush lacks, read from extra keys of the brush preset
// (patches 15 to 17 in app/PATCHES.md). Header only; every key is absent from Krita's own presets, and
// with the keys absent nothing here changes how a brush paints.
//
//   Cf/ScatterCount, Cf/ScatterCountJitter     Photoshop's Scattering "Count" and "Count Jitter"
//   Cf/Pose/{TiltX,TiltY,Rotation,Pressure}    Photoshop's Brush Pose overrides; each with a Cf/Pose/Override... flag
//   Cf/Noise                                   Photoshop's Noise: random loss of alpha in every dab (0 to 1)
//   Cf/TipRoundness                            roundness (0 to 1) of a picture tip, which Krita's own tip settings lack (patch 23)
//   Cf/AngleJitter                             Photoshop's Angle Jitter: a random turn added to whatever the angle control gives
//   Cf/ScatterCountControl, Cf/ScatterCountSteps   what the Scattering "Count" follows (pressure, fade, ...)
#ifndef CF_BRUSH_EXTRAS_H
#define CF_BRUSH_EXTRAS_H

#include <QRandomGenerator>
#include <QVector>

#include <KoColorSpace.h>
#include <kis_fixed_paint_device.h>
#include <kis_paint_information.h>
#include <kis_properties_configuration.h>

namespace CfBrushExtras
{

struct Extras
{
    int scatterCount {1};
    double scatterCountJitter {0.0};  // 0 to 1
    bool overrideTiltX {false}, overrideTiltY {false}, overrideRotation {false}, overridePressure {false};
    double tiltX {0.0}, tiltY {0.0}, rotation {0.0}, pressure {1.0};
    double noise {0.0};
    double wetEdges {0.0};  // 0 to 1, patch 18 (the stroke-level work is in KisIndirectPaintingSupport)
    double tipRoundness {1.0};   // patch 23
    double angleJitter {0.0};    // 0 to 1: a turn of up to +-360 degrees times this, added to the angle (patch 23, range from Photoshop: patch 26)
    bool roundTip {false};       // a generated round tip (patch 26)
    double softScale {1.0};      // the tip's mask is drawn this much bigger than its size: Photoshop's soft tips reach past it (patch 26)
    QString countControl;        // "", pressure, tangentialpressure, tiltelevation, rotation, drawingangle, fade
    int countSteps {25};         // the steps of the fade control

    static Extras read(const KisPropertiesConfiguration *s)
    {
        Extras e;
        if (!s) return e;
        e.scatterCount = qMax(1, s->getInt("Cf/ScatterCount", 1));
        e.scatterCountJitter = qBound(0.0, s->getDouble("Cf/ScatterCountJitter", 0.0), 1.0);
        e.overrideTiltX = s->getBool("Cf/Pose/OverrideTiltX", false);
        e.overrideTiltY = s->getBool("Cf/Pose/OverrideTiltY", false);
        e.overrideRotation = s->getBool("Cf/Pose/OverrideRotation", false);
        e.overridePressure = s->getBool("Cf/Pose/OverridePressure", false);
        e.tiltX = s->getDouble("Cf/Pose/TiltX", 0.0);
        e.tiltY = s->getDouble("Cf/Pose/TiltY", 0.0);
        e.rotation = s->getDouble("Cf/Pose/Rotation", 0.0);
        e.pressure = s->getDouble("Cf/Pose/Pressure", 1.0);
        e.noise = qBound(0.0, s->getDouble("Cf/Noise", 0.0), 1.0);
        e.wetEdges = qBound(0.0, s->getDouble("Cf/WetEdges", 0.0), 1.0);
        e.tipRoundness = qBound(0.01, s->getDouble("Cf/TipRoundness", 1.0), 1.0);
        e.angleJitter = qBound(0.0, s->getDouble("Cf/AngleJitter", 0.0), 1.0);
        e.roundTip = s->getBool("Cf/SpacingMinor", false);
        e.softScale = qBound(1.0, s->getDouble("Cf/SoftScale", 1.0), 3.0);
        e.countControl = s->getString("Cf/ScatterCountControl", QString());
        e.countSteps = qMax(1, s->getInt("Cf/ScatterCountSteps", 25));
        return e;
    }

    bool hasPose() const { return overrideTiltX || overrideTiltY || overrideRotation || overridePressure; }

    // Brush Pose: the values the artist fixed replace what the pen reports.
    void applyPose(KisPaintInformation &info) const
    {
        if (overrideTiltX) info.setXTilt(tiltX);
        if (overrideTiltY) info.setYTilt(tiltY);
        if (overrideRotation) info.setRotation(rotation);
        if (overridePressure) info.setPressure(pressure);
    }

    // Scatter count: marks per dab, each scattered on its own; the jitter lowers it at random.
    int countFor(const KisPaintInformation &info) const
    {
        if (scatterCount <= 1) return 1;
        qreal count = scatterCount;
        if (!countControl.isEmpty()) count = qMax(1.0, count * controlValue(info));
        // Photoshop's Count Jitter changes nothing on the marks (measured: the same area per mark with jitter 0, 50 and
        // 100), so the jitter is not applied; the key stays for the panel
        return qMax(1, qRound(count));
    }

    // 0 to 1: how far the count's control is turned up (patch 23)
    qreal controlValue(const KisPaintInformation &info) const
    {
        if (info.isHoveringMode()) return 1.0;
        if (countControl == QLatin1String("pressure")) return info.pressure();
        if (countControl == QLatin1String("tangentialpressure")) return info.tangentialPressure();
        if (countControl == QLatin1String("tiltelevation")) return KisPaintInformation::tiltElevation(info, 60.0, 60.0, true);
        if (countControl == QLatin1String("rotation")) return qBound(0.0, info.rotation() / 360.0, 1.0);
        if (countControl == QLatin1String("drawingangle")) return qBound(0.0, info.drawingAngle() / (2.0 * M_PI) + 0.5, 1.0);
        if (countControl == QLatin1String("fade")) return qBound(0.0, 1.0 - qreal(info.currentDabSeqNo()) / countSteps, 1.0);
        return 1.0;
    }

    // Angle Jitter: a random turn on top of the angle (radians)
    qreal angleJitterFor(const KisPaintInformation &info) const
    {
        if (angleJitter <= 0.0) return 0.0;
        // Photoshop: 10 percent turns a mark by up to 36 degrees either way, 25 percent by 90 (measured on its own marks)
        return (2.0 * info.randomSource()->generateNormalized() - 1.0) * angleJitter * 2.0 * M_PI;
    }

    // Noise, as Photoshop does it (measured on a single soft mark): every pixel that is neither empty nor solid
    // gets a random change of up to 0.72 times the nearer of its opacity and what is missing to full opacity, either
    // way. Solid and empty pixels are left alone, so a hard tip shows no noise at all, as in Photoshop. (patch 26)
    void applyNoise(KisFixedPaintDeviceSP dab) const
    {
        if (noise <= 0.0 || !dab) return;
        const int pixels = dab->bounds().width() * dab->bounds().height();
        if (pixels <= 0) return;
        const KoColorSpace *cs = dab->colorSpace();
        const int size = cs->pixelSize();
        QRandomGenerator *rng = QRandomGenerator::global();
        quint8 *p = dab->data();
        for (int i = 0; i < pixels; ++i, p += size) {
            const double a = cs->opacityF(p);
            if (a <= 0.0 || a >= 1.0) continue;
            const double change = (2.0 * rng->generateDouble() - 1.0) * 0.72 * qMin(a, 1.0 - a) * qMin(1.0, noise * 2.0);
            cs->setOpacity(p, quint8(qBound(0.0, (a + change) * 255.0 + 0.5, 255.0)), 1);
        }
    }
};

}  // namespace CfBrushExtras

#endif
