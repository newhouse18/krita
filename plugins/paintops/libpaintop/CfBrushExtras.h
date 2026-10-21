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

#include <vector>
#include <cmath>
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
    double psMixSize {-1.0};     // >= 0: size = this + (1 - this) * (the size curves' product), Photoshop's minimum with jitter and a control (patch 27)
    double psMixRoundness {-1.0};
    bool tiltAngle {false};      // the angle follows the direction of the pen's tilt (Photoshop's Pen Tilt on Angle, patch 28)
    double tiltSize {0.0};       // > 0: the size grows with the pen's tilt, by this share of Photoshop's amount (Pen Tilt on Size, patch 28)
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
        if (s->hasProperty("Cf/PsMix/Size")) e.psMixSize = qBound(0.0, s->getDouble("Cf/PsMix/Size", 0.0), 1.0);
        if (s->hasProperty("Cf/PsMix/Roundness")) e.psMixRoundness = qBound(0.0, s->getDouble("Cf/PsMix/Roundness", 0.0), 1.0);
        e.tiltAngle = s->getBool("Cf/Tilt/Angle", false);
        e.tiltSize = qMax(0.0, s->getDouble("Cf/Tilt/Size", 0.0));
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

    // Pen Tilt on the size (patch 28), measured in Photoshop with a simulated pen: 1 upright, 1.25 at 20 degrees, 1.5 at 40, 1.65 at 60;
    // 1 + 0.75 sin(lean), the lean being the length of the two tilts in degrees
    qreal tiltSizeFactor(const KisPaintInformation &info) const
    {
        if (tiltSize <= 0.0 || info.isHoveringMode()) return 1.0;
        const qreal lean = qMin(90.0, std::sqrt(info.xTilt() * info.xTilt() + info.yTilt() * info.yTilt()));
        return 1.0 + 0.75 * tiltSize * std::sin(lean * M_PI / 180.0);
    }

    // Pen Tilt on the angle (patch 28): the long axis of the tip stands across the direction the pen leans (leaning right makes it
    // vertical, leaning toward the user horizontal, right and down a slant) - radians to add to the tip's angle
    qreal tiltAngleFor(const KisPaintInformation &info) const
    {
        if (!tiltAngle || info.isHoveringMode()) return 0.0;
        if (info.xTilt() == 0.0 && info.yTilt() == 0.0) return 0.0;
        return std::atan2(info.xTilt(), info.yTilt());
    }

    // Photoshop's bristle tip (patch 29), fitted to its own strokes (lab 4): a dab is a handful of tiny dots at random places in a disc, and the
    // stroke is the many dabs laid over one another, so the middle is solid and the edge ragged. The disc is 0.47 of the size across, a dab has about
    // 40 dots at a bristle density of 31 percent (fewer, as the 1.3 power of the density), each 1.6 pixels at size 60 (more with the bristle thickness,
    // and the square root of the size), and one dot is always in the middle (the dark line of a sparse brush). Replaces the dab's opacity.
    static void applyBristles(KisFixedPaintDeviceSP dab, double density, double thickness)
    {
        if (!dab || density <= 0.0) return;
        const QRect bounds = dab->bounds();
        const int w = bounds.width(), h = bounds.height();
        if (w < 2 || h < 2) return;
        const double size = qMax(w, h) / 1.05;
        const double radius = 0.222 * size;
        const double diameter = (1.6 + 5.4 * qBound(0.0, thickness, 1.0)) * std::sqrt(size / 60.0);
        QRandomGenerator *rng = QRandomGenerator::global();
        // the dots per dab by bristle density, from Photoshop's strokes at densities 5, 15, 31, 60 and 100 (the stroke gets solid quicker than the count grows)
        static const double table[][2] = {{0.0, 0.0}, {0.05, 3.8}, {0.15, 12.0}, {0.31, 30.0}, {0.6, 48.0}, {1.0, 60.0}};
        double count = table[5][1];
        for (int i = 1; i < 6; ++i) {
            if (density <= table[i][0]) {
                count = table[i - 1][1] + (density - table[i - 1][0]) / (table[i][0] - table[i - 1][0]) * (table[i][1] - table[i - 1][1]);
                break;
            }
        }
        int dots = int(count) + (rng->generateDouble() < count - int(count) ? 1 : 0);
        std::vector<float> keep(size_t(w) * h, 1.0f);   // what is left of the dab's transparency: the product of (1 - the dots' opacity)
        const double cx = w / 2.0, cy = h / 2.0, reach = diameter / 2.0 + 1.0;
        for (int k = 0; k <= dots; ++k) {
            double x = cx, y = cy;
            if (k > 0) {   // the first dot is the middle one
                const double a = 2.0 * M_PI * rng->generateDouble(), r = radius * std::pow(rng->generateDouble(), 0.6);   // a little more of them near the middle
                x += r * std::cos(a);
                y += r * std::sin(a);
            }
            const int x0 = qMax(0, int(x - reach)), x1 = qMin(w - 1, int(x + reach)), y0 = qMax(0, int(y - reach)), y1 = qMin(h - 1, int(y + reach));
            for (int py = y0; py <= y1; ++py) {
                for (int px = x0; px <= x1; ++px) {
                    const double dist = std::sqrt((px + 0.5 - x) * (px + 0.5 - x) + (py + 0.5 - y) * (py + 0.5 - y));
                    const double a = qBound(0.0, diameter / 2.0 + 0.5 - dist, 1.0);
                    if (a > 0.0) keep[size_t(py) * w + px] *= float(1.0 - qMin(a, 0.999));
                }
            }
        }
        const KoColorSpace *cs = dab->colorSpace();
        const int pixelSize = cs->pixelSize();
        quint8 *p = dab->data();
        for (size_t i = 0; i < size_t(w) * h; ++i, p += pixelSize) {
            cs->setOpacity(p, quint8(qBound(0.0, (1.0 - keep[i]) * 255.0 + 0.5, 255.0)), 1);
        }
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
