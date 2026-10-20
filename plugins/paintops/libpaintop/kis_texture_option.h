/* This file is part of the KDE project
 * SPDX-FileCopyrightText: 2012 Boudewijn Rempt <boud@valdyas.org>
 * SPDX-FileCopyrightText: 2021 Deif Lou <ginoba@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#ifndef KIS_TEXTURE_OPTION_H
#define KIS_TEXTURE_OPTION_H

#include <kritapaintop_export.h>

#include <kis_paint_device.h>
#include <kis_cached_paint_device.h>
#include <kis_types.h>
#include <resources/KoAbstractGradient.h>
#include <resources/KoCachedGradient.h>

#include "KisTextureMaskInfo.h"


class KoPattern;
class KoResource;
class KisPropertiesConfiguration;
class KisResourcesInterface;

#include <KisStandardOptions.h>
#include <KisTextureOptionData.h>


class PAINTOP_EXPORT KisTextureOption
{
public:
    KisTextureOption(const KisPropertiesConfiguration *setting, KisResourcesInterfaceSP resourcesInterface,
                     KoCanvasResourcesInterfaceSP canvasResourcesInterface,
                     int levelOfDetail,
                     KisBrushTextureFlags flags = None);

    bool m_enabled {false};
    bool m_cfConstantDepth {false};
    qreal m_cfDepth {1.0};
    bool m_cfWasEnabled {false};
    bool m_cfEachTip {false};  // CONCEPTFORGE: the pattern starts again at every dab (patch 19)

    /**
     * @brief apply combine the texture map with the dab
     * @param dab the colored, final representation of the dab, after mirroring and everything.
     * @param offset the position of the dab on the image. used to calculate the position of the mask pattern
     * @param info the paint information
     */
    void apply(KisFixedPaintDeviceSP dab, const QPoint& offset, const KisPaintInformation & info);
    static QList<KoResourceLoadResult> prepareLinkedResources(const KisPropertiesConfigurationSP setting, KisResourcesInterfaceSP resourcesInterface);
    static QList<KoResourceLoadResult> prepareEmbeddedResources(const KisPropertiesConfigurationSP setting, KisResourcesInterfaceSP resourcesInterface);
    bool applyingGradient() const;

    // CONCEPTFORGE: patch 27. Photoshop puts a brush texture on the stroke as a whole unless "Texture Each Tip" is on: such a
    // preset (key Cf/Texture/StrokeLevel, texture on, not each tip, depth without a control) has the dab-level option off,
    // and its settings give a processor that does the same on the composed stroke.
    static bool cfStrokeLevel(const KisPropertiesConfiguration *setting);
    void cfMakeStrokeInstance() { m_enabled = m_cfWasEnabled; }

    static bool applyingGradient(const KisPropertiesConfiguration *settings);
    static bool requiresEffectiveCompositeOp(const KisPropertiesConfiguration *settings);

private:
    void applyLightness(KisFixedPaintDeviceSP dab, const QPoint& offset, const KisPaintInformation& info);
    void applyGradient(KisFixedPaintDeviceSP dab, const QPoint& offset, const KisPaintInformation& info);
    void fillProperties(const KisPropertiesConfiguration *setting, KisResourcesInterfaceSP resourcesInterface, KoCanvasResourcesInterfaceSP canvasResourcesInterface);
    int effectiveOffsetX(const KisPaintInformation &info) const;
    int effectiveOffsetY(const KisPaintInformation &info) const;
private:

    int m_offsetX {0};
    int m_offsetY {0};
    bool m_isRandomOffsetX {false};
    bool m_isRandomOffsetY {false};
    KisTextureOptionData::TexturingMode m_texturingMode {KisTextureOptionData::MULTIPLY};
    bool m_useSoftTexturing {false};
    KoAbstractGradientSP m_gradient;
    KoCachedGradient m_cachedGradient;

    int m_levelOfDetail {0};

private:
    KisStrengthOption m_strengthOption;
    KisTextureMaskInfoSP m_maskInfo;
    KisBrushTextureFlags m_flags;
    KisCachedPaintDevice m_cachedPaintDevice;
};

#endif // KIS_TEXTURE_OPTION_H
