/*
 *  SPDX-FileCopyrightText: 2017 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisBrushOpResources.h"

#include "kis_brush.h"

#include <KoColorSpace.h>
#include <KoColorTransformation.h>

#include "kis_color_source.h"
#include "kis_color_source_option.h"
#include "KisSharpnessOption.h"
#include "KisStandardOptions.h"
#include "KisDarkenOption.h"
#include "KisHSVOption.h"
#include "kis_texture_option.h"
#include <CfBrushExtras.h>
#include "kis_painter.h"
#include "kis_paintop_settings.h"

#include <QColor>

namespace {

// CONCEPTFORGE: patch 27. Photoshop's colour jitter and purity, measured on its own marks (docs/spikes/ps-lab.md): saturation and
// brightness jitter ADD a random amount (up to the jitter) and clamp; purity above 0 adds that share of what is missing to full
// saturation, below 0 takes that share away.
class CfHsvShift : public KoColorTransformation
{
public:
    explicit CfHsvShift(const KoColorSpace *cs) : m_cs(cs) {}
    void transform(const quint8 *src, quint8 *dst, qint32 nPixels) const override
    {
        const int size = m_cs->pixelSize();
        for (qint32 i = 0; i < nPixels; ++i, src += size, dst += size) {
            QColor c;
            m_cs->toQColor(src, &c);
            qreal h, s, v, a;
            c.getHsvF(&h, &s, &v, &a);
            s = qBound(0.0, s + dS, 1.0);
            v = qBound(0.0, v + dV, 1.0);
            if (purity > 0.0) s += (1.0 - s) * purity;
            else if (purity < 0.0) s *= 1.0 + purity;
            c.setHsvF(h < 0 ? 0.0 : h, qBound(0.0, s, 1.0), v, a);
            m_cs->fromQColor(c, dst);
        }
    }
    QList<QString> parameters() const override { return {}; }
    int parameterId(const QString &) const override { return -1; }
    void setParameter(int, const QVariant &) override {}
    qreal dS {0.0}, dV {0.0}, purity {0.0};

private:
    const KoColorSpace *m_cs;
};

}

struct KisBrushOpResources::Private
{
    Private(const KisPaintOpSettings *setting)
        : mixOption(setting)
        , darkenOption(setting)
    {
    }

    QList<KisHSVOption*> hsvOptions;
    KoColorTransformation *hsvTransformation = 0;
    KisMixOption mixOption;
    KisDarkenOption darkenOption;
    qreal cfPurity {0.0};  // CONCEPTFORGE: Photoshop's Purity, -1 to 1 (patch 21)
    KoColorTransformation *cfPurityTransformation {nullptr};
    CfHsvShift *cfShift {nullptr};   // patch 27: used instead of the two above and Krita's saturation and value when Cf/PsColor is set
    KisHSVOption *cfSat {nullptr}, *cfVal {nullptr};
};


KisBrushOpResources::KisBrushOpResources(const KisPaintOpSettingsSP settings, KisPainter *painter)
    : m_d(new Private(settings.data()))
{
    KisColorSourceOption colorSourceOption(settings.data());
    colorSource.reset(colorSourceOption.createColorSource(painter));

    sharpnessOption.reset(new KisSharpnessOption(settings.data()));
    m_d->cfPurity = qBound(-1.0, settings->getDouble("Cf/Purity", 0.0), 1.0);
    const bool psColor = settings->getBool("Cf/PsColor", false);
    if (psColor) {
        m_d->cfShift = new CfHsvShift(painter->backgroundColor().colorSpace());
        m_d->cfShift->purity = m_d->cfPurity;
    } else if (m_d->cfPurity != 0.0) {
        m_d->cfPurityTransformation =
            painter->backgroundColor().colorSpace()->createColorTransformation("hsv_adjustment", QHash<QString, QVariant>());
    }
    cfNoise = CfBrushExtras::Extras::read(settings.data()).noise;  // CONCEPTFORGE: patch 17

    textureOption.reset(new KisTextureOption(settings.data(),
                                             settings->resourcesInterface(),
                                             settings->canvasResourcesInterface(),
                                             painter->device()->defaultBounds()->currentLevelOfDetail(),
                                             SupportsGradientMode | SupportsLightnessMode));

    m_d->hsvOptions.append(KisHSVOption::createHueOption(settings.data()));
    m_d->hsvOptions.append(KisHSVOption::createSaturationOption(settings.data()));
    m_d->hsvOptions.append(KisHSVOption::createValueOption(settings.data()));

    if (psColor) {
        m_d->cfSat = m_d->hsvOptions.at(1);
        m_d->cfVal = m_d->hsvOptions.at(2);
    }
    Q_FOREACH (KisHSVOption *option, m_d->hsvOptions) {
        if (psColor && option != m_d->hsvOptions.first()) continue;   // saturation and value are done by cfShift
        if (option->isChecked() && !m_d->hsvTransformation) {
            m_d->hsvTransformation = painter->backgroundColor().colorSpace()->createColorTransformation("hsv_adjustment", QHash<QString, QVariant>());
        }
    }

    // the brush should be initialized explicitly by the caller later
    KIS_SAFE_ASSERT_RECOVER_NOOP(!brush);
}

KisBrushOpResources::~KisBrushOpResources()
{
    delete m_d->cfPurityTransformation;
    delete m_d->cfShift;
    qDeleteAll(m_d->hsvOptions);
    delete m_d->hsvTransformation;
}

void KisBrushOpResources::syncResourcesToSeqNo(int seqNo, const KisPaintInformation &info)
{
    colorSource->selectColor(m_d->mixOption.apply(info), info);
    m_d->darkenOption.apply(colorSource.get(), info);

    if (m_d->hsvTransformation) {
        Q_FOREACH (KisHSVOption * option, m_d->hsvOptions) {
            if (m_d->cfShift && option != m_d->hsvOptions.first()) continue;
            option->apply(m_d->hsvTransformation, info);
        }
        colorSource->applyColorTransformation(m_d->hsvTransformation);
    }

    if (m_d->cfShift) {
        m_d->cfShift->dS = m_d->cfSat->cfShift(info);
        m_d->cfShift->dV = m_d->cfVal->cfShift(info);
        if (m_d->cfShift->dS != 0.0 || m_d->cfShift->dV != 0.0 || m_d->cfShift->purity != 0.0) {
            colorSource->applyColorTransformation(m_d->cfShift);
        }
    }

    if (m_d->cfPurityTransformation) {
        // Purity is a fixed shift of saturation, on top of whatever the saturation option did
        m_d->cfPurityTransformation->setParameter(m_d->cfPurityTransformation->parameterId("s"), m_d->cfPurity);
        m_d->cfPurityTransformation->setParameter(3, 0);
        m_d->cfPurityTransformation->setParameter(4, false);
        m_d->cfPurityTransformation->setParameter(8, false);
        colorSource->applyColorTransformation(m_d->cfPurityTransformation);
    }

    KisDabCacheUtils::DabRenderingResources::syncResourcesToSeqNo(seqNo, info);
}
