// SPDX-License-Identifier: GPL-2.0-or-later
// CONCEPTFORGE: Photoshop brush features the pixel brush lacks, read from extra keys of the brush preset
// (patches 15 to 17 in app/PATCHES.md). Header only; every key is absent from Krita's own presets, and
// with the keys absent nothing here changes how a brush paints.
//
//   Cf/ScatterCount, Cf/ScatterCountJitter     Photoshop's Scattering "Count" and "Count Jitter"
//   Cf/Pose/{TiltX,TiltY,Rotation,Pressure}    Photoshop's Brush Pose overrides; each with a Cf/Pose/Override... flag
//   Cf/Noise                                   Photoshop's Noise: random loss of alpha in every dab (0 to 1)
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
        if (scatterCountJitter <= 0.0) return scatterCount;
        const qreal r = info.randomSource()->generateNormalized();
        return qMax(1, qRound(scatterCount * (1.0 - scatterCountJitter * r)));
    }

    // Noise: every pixel of the dab loses a random share of its opacity.
    void applyNoise(KisFixedPaintDeviceSP dab) const
    {
        if (noise <= 0.0 || !dab) return;
        const int pixels = dab->bounds().width() * dab->bounds().height();
        if (pixels <= 0) return;
        QVector<quint8> mask(pixels);
        QRandomGenerator *rng = QRandomGenerator::global();
        for (int i = 0; i < pixels; ++i) {
            mask[i] = quint8(255.0 * (1.0 - noise * rng->generateDouble()));
        }
        dab->colorSpace()->applyAlphaU8Mask(dab->data(), mask.constData(), pixels);
    }
};

}  // namespace CfBrushExtras

#endif
