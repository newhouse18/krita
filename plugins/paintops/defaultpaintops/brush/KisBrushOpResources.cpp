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
};


KisBrushOpResources::KisBrushOpResources(const KisPaintOpSettingsSP settings, KisPainter *painter)
    : m_d(new Private(settings.data()))
{
    KisColorSourceOption colorSourceOption(settings.data());
    colorSource.reset(colorSourceOption.createColorSource(painter));

    sharpnessOption.reset(new KisSharpnessOption(settings.data()));
    m_d->cfPurity = qBound(-1.0, settings->getDouble("Cf/Purity", 0.0), 1.0);
    if (m_d->cfPurity != 0.0) {
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

    Q_FOREACH (KisHSVOption *option, m_d->hsvOptions) {
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
    qDeleteAll(m_d->hsvOptions);
    delete m_d->hsvTransformation;
}

void KisBrushOpResources::syncResourcesToSeqNo(int seqNo, const KisPaintInformation &info)
{
    colorSource->selectColor(m_d->mixOption.apply(info), info);
    m_d->darkenOption.apply(colorSource.get(), info);

    if (m_d->hsvTransformation) {
        Q_FOREACH (KisHSVOption * option, m_d->hsvOptions) {
            option->apply(m_d->hsvTransformation, info);
        }
        colorSource->applyColorTransformation(m_d->hsvTransformation);
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
