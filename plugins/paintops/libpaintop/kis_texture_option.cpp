/* This file is part of the KDE project
 * SPDX-FileCopyrightText: 2012 Boudewijn Rempt <boud@valdyas.org>
 * SPDX-FileCopyrightText: 2014 Mohit Goyal <mohit.bits2011@gmail.com>
 * SPDX-FileCopyrightText: 2021 Deif Lou <ginoba@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#include "kis_texture_option.h"

#include <QString>
#include <QCheckBox>
#include <QBuffer>
#include <QFormLayout>
#include <QPainter>
#include <QBoxLayout>

#include <klocalizedstring.h>

#include <KoPattern.h>
#include <KoAbstractGradient.h>
#include <KoResource.h>
#include <KoResourceServerProvider.h>
#include <kis_paint_device.h>
#include <kis_fill_painter.h>
#include <kis_painter.h>
#include <kis_iterator_ng.h>
#include <kis_fixed_paint_device.h>
#include "KoMixColorsOp.h"
#include <strokes/KisMaskingBrushCompositeOpBase.h>
#include <strokes/KisMaskingBrushCompositeOpFactory.h>
#include <kis_random_accessor_ng.h>
#include <KoCompositeOpRegistry.h>

#include <KoCanvasResourcesIds.h>
#include <KoCanvasResourcesInterface.h>
#include <KoResourceLoadResult.h>
#include <kis_paint_information.h>

/**********************************************************************/
/*       KisTextureOption                                             */
/**********************************************************************/


KisTextureOption::KisTextureOption(const KisPropertiesConfiguration *setting,
                                   KisResourcesInterfaceSP resourcesInterface,
                                   KoCanvasResourcesInterfaceSP canvasResourcesInterface,
                                   int levelOfDetail,
                                   KisBrushTextureFlags flags)
    : m_gradient(0)
    , m_levelOfDetail(levelOfDetail)
    , m_strengthOption(setting)
    , m_flags(flags)
{
    fillProperties(setting, resourcesInterface, canvasResourcesInterface);
}

// CONCEPTFORGE: patch 7. How many extra textures the brush's variation set holds (0 when the switch is off or the set is empty)
int KisTextureOption::cfVariationCount(const KisPropertiesConfiguration *setting)
{
    if (!setting || !setting->getBool("Cf/TexVar/On", false)) return 0;
    return qBound(0, setting->getInt("Cf/TexVar/Count", 0), 64);
}

namespace {
QAtomicInt s_cfStrokeCounter;

quint32 cfMix(quint32 a, quint32 b)
{
    quint32 x = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA6Bu;
    x ^= x >> 15; x *= 0x2C1B3C6Du; x ^= x >> 12; x *= 0x297A2D39u; x ^= x >> 15;
    return x;
}

// The shuffled order of one block of ``count`` textures
QVector<int> cfPermutation(int count, quint32 seed, quint32 block, int notFirst)
{
    QVector<int> order(count);
    for (int i = 0; i < count; ++i) order[i] = i;
    for (int i = count - 1; i > 0; --i) {
        const int j = int(cfMix(seed ^ (block * 2654435761u), quint32(i)) % quint32(i + 1));
        std::swap(order[i], order[j]);
    }
    if (count > 1 && order[0] == notFirst) std::swap(order[0], order[1]);   // never the same texture twice in a row
    return order;
}
}  // namespace

int KisTextureOption::cfVariantIndex(const KisPaintInformation &info) const
{
    const int count = m_cfSet.size();
    if (count < 2) return 0;
    const qreal every = qMax(0.01, m_cfEvery);
    qint64 n;
    quint32 seed = 0x51ed270bu;
    if (m_cfStrokeInstance || m_cfTrigger == 1) {
        n = qint64(m_cfStroke / every);
    } else if (m_cfTrigger == 2) {
        n = qint64(info.currentTime() / (1000.0 * every));
        seed = quint32(info.perStrokeRandomSource()->generate("texvar_seed", 0, 1 << 30));
    } else {
        n = qint64(info.currentDabSeqNo() / every);
        seed = quint32(info.perStrokeRandomSource()->generate("texvar_seed", 0, 1 << 30));
    }
    if (m_cfOrder == 0) return int(n % count);
    const quint32 block = quint32(n / count);
    int lastOfPrevious = -1;
    if (block > 0) lastOfPrevious = cfPermutation(count, seed, block - 1, -1).last();
    return cfPermutation(count, seed, block, lastOfPrevious).at(int(n % count));
}

KisTextureMaskInfoSP KisTextureOption::cfMaskFor(const KisPaintInformation &info) const
{
    if (m_cfSet.isEmpty()) return m_maskInfo;
    const KisTextureMaskInfoSP chosen = m_cfSet.at(cfVariantIndex(info));
    return chosen && chosen->isValid() ? chosen : m_maskInfo;
}

bool KisTextureOption::cfStrokeLevel(const KisPropertiesConfiguration *setting)
{
    if (!setting || !setting->getBool("Cf/Texture/StrokeLevel", false) || setting->getBool("Cf/Texture/EachTip", false)) return false;
    KisTextureOptionData data;
    data.read(setting);
    if (!data.isEnabled || data.textureData.isNull()) return false;
    // patch 7: a texture that changes with every dab or over time has to be applied per dab
    if (cfVariationCount(setting) > 0 && setting->getInt("Cf/TexVar/Trigger", 0) != 1) return false;
    // Photoshop ignores a depth jitter or fade on a texture that goes on the whole stroke (measured, lab 3): whatever the
    // depth follows, the stroke gets the constant depth.
    return true;
}

void KisTextureOption::fillProperties(const KisPropertiesConfiguration *setting, KisResourcesInterfaceSP resourcesInterface, KoCanvasResourcesInterfaceSP canvasResourcesInterface)
{
    m_cfEachTip = setting->getBool("Cf/Texture/EachTip", false);  // CONCEPTFORGE: patch 19
    {
        KisStrengthOptionData strength;   // patch 27: the constant depth, for a depth without a control and for the whole stroke
        strength.read(setting);
        m_cfDepth = strength.strengthValue;
        // Krita's own texture editor puts a Pressure sensor back on the depth; the key says there is no control
        m_cfConstantDepth = setting->hasProperty("Cf/Texture/DepthControl") && setting->getString("Cf/Texture/DepthControl").isEmpty();
    }
    KisTextureOptionData data;
    data.read(setting);

    if (data.textureData.isNull()) {
        m_enabled = false;
        return;
    }

    m_texturingMode = data.texturingMode;
    m_useSoftTexturing = data.useSoftTexturing;

    if (!(m_flags & SupportsLightnessMode) && (m_texturingMode == KisTextureOptionData::LIGHTNESS)) {
        m_texturingMode = KisTextureOptionData::SUBTRACT;
    }

    if (!(m_flags & SupportsGradientMode) && (m_texturingMode == KisTextureOptionData::GRADIENT)) {
        m_texturingMode = KisTextureOptionData::SUBTRACT;
    }

    const bool preserveAlpha = m_texturingMode == KisTextureOptionData::LIGHTNESS || m_texturingMode == KisTextureOptionData::GRADIENT;

    QString effectiveCompositeOp = COMPOSITE_OVER;
    bool additionalInvert = false;

    if (canvasResourcesInterface && data.autoInvertOnErase) {
        effectiveCompositeOp = canvasResourcesInterface->resource(KoCanvasResource::CurrentEffectiveCompositeOp).toString();
        KIS_SAFE_ASSERT_RECOVER (!effectiveCompositeOp.isEmpty()) {
            effectiveCompositeOp = COMPOSITE_OVER;
        }
        additionalInvert = effectiveCompositeOp == COMPOSITE_ERASE;
    }

    m_maskInfo = toQShared(new KisTextureMaskInfo(m_levelOfDetail, preserveAlpha));
    if (!m_maskInfo->fillProperties(setting, resourcesInterface, additionalInvert)) {
        warnKrita << "WARNING: Couldn't load the pattern for a stroke (KisTextureProperties)";
        m_enabled = false;
        return;
    }

    m_maskInfo = KisTextureMaskInfoCache::instance()->fetchCachedTextureInfo(m_maskInfo);

    // CONCEPTFORGE: patch 7. The variation set: one mask for the main texture and one for each extra one, all made now so that
    // changing the texture while painting costs nothing
    if (const int extra = cfVariationCount(setting)) {
        m_cfOrder = setting->getInt("Cf/TexVar/Order", 0);
        m_cfTrigger = setting->getInt("Cf/TexVar/Trigger", 0);
        m_cfEvery = setting->getDouble("Cf/TexVar/Every", 1.0);
        m_cfStroke = s_cfStrokeCounter.fetchAndAddRelaxed(1);
        m_cfSet << m_maskInfo;
        for (int i = 0; i < extra; ++i) {
            KisEmbeddedTextureData variant;
            variant.read(setting, QStringLiteral("Cf/TexVar/%1/").arg(i));
            if (variant.isNull()) continue;
            KisPropertiesConfiguration copy(*setting);
            copy.setProperty("Texture/Pattern/PatternMD5", variant.md5Base64);
            copy.setProperty("Texture/Pattern/PatternMD5Sum", variant.md5sum);
            copy.setProperty("Texture/Pattern/PatternFileName", variant.fileName);
            copy.setProperty("Texture/Pattern/Name", variant.name);
            copy.setProperty("Texture/Pattern/Pattern", variant.patternBase64);
            KisTextureMaskInfoSP mask = toQShared(new KisTextureMaskInfo(m_levelOfDetail, preserveAlpha));
            if (mask->fillProperties(&copy, resourcesInterface, additionalInvert)) {
                m_cfSet << KisTextureMaskInfoCache::instance()->fetchCachedTextureInfo(mask);
            }
        }
        if (m_cfSet.size() < 2) m_cfSet.clear();
    }

    m_enabled = data.isEnabled;
    m_cfWasEnabled = m_enabled;
    if (cfStrokeLevel(setting)) m_enabled = false;   // patch 27: the stroke does it as a whole
    m_offsetX = data.offsetX;
    m_offsetY = data.offsetY;
    m_isRandomOffsetX = data.isRandomOffsetX;
    m_isRandomOffsetY = data.isRandomOffsetY;

    if (m_texturingMode == KisTextureOptionData::GRADIENT && canvasResourcesInterface) {
        KoAbstractGradientSP gradient = canvasResourcesInterface->resource(KoCanvasResource::CurrentGradient).value<KoAbstractGradientSP>()->cloneAndBakeVariableColors(canvasResourcesInterface);
        if (gradient) {
            m_gradient = gradient;
            m_cachedGradient.setGradient(gradient, 256);
        }
    }
}

int KisTextureOption::effectiveOffsetX(const KisPaintInformation &info) const
{
    KIS_SAFE_ASSERT_RECOVER_RETURN_VALUE(m_maskInfo, 0);
    return m_isRandomOffsetX ?
        info.perStrokeRandomSource()->generate("texture_offset_x", 0, m_maskInfo->maskBounds().width()) :
        m_offsetX;
}

int KisTextureOption::effectiveOffsetY(const KisPaintInformation &info) const
{
    KIS_SAFE_ASSERT_RECOVER_RETURN_VALUE(m_maskInfo, 0);
    return m_isRandomOffsetY ?
        info.perStrokeRandomSource()->generate("texture_offset_y", 0, m_maskInfo->maskBounds().height()) :
        m_offsetY;
}

QList<KoResourceLoadResult> KisTextureOption::prepareEmbeddedResources(const KisPropertiesConfigurationSP setting, KisResourcesInterfaceSP resourcesInterface)
{
    /**
     * We cannot use m_enabled here because it is not initialized at this stage.
     * fillProperties() is not necessary for this call, because it is extremely slow.
     */

    QList<KoResourceLoadResult> patterns;

    KisTextureOptionData data;
    data.read(setting.data());

    if (data.isEnabled && !data.textureData.patternBase64.isEmpty()) {
        patterns << data.textureData.loadLinkedPattern(resourcesInterface);
    }
    if (data.isEnabled) {   // CONCEPTFORGE: patch 7, the extra textures of the variation set
        for (int i = 0, n = cfVariationCount(setting.data()); i < n; ++i) {
            KisEmbeddedTextureData variant;
            variant.read(setting.data(), QStringLiteral("Cf/TexVar/%1/").arg(i));
            if (!variant.isNull() && !variant.patternBase64.isEmpty()) patterns << variant.loadLinkedPattern(resourcesInterface);
        }
    }

    return patterns;
}

QList<KoResourceLoadResult> KisTextureOption::prepareLinkedResources(const KisPropertiesConfigurationSP setting, KisResourcesInterfaceSP resourcesInterface)
{
    /**
     * We cannot use m_enabled here because it is not initialized at this stage.
     * fillProperties() is not necessary for this call, because it is extremely slow.
     */

    QList<KoResourceLoadResult> patterns;

    KisTextureOptionData data;
    data.read(setting.data());

    if (data.isEnabled && data.textureData.patternBase64.isEmpty()) {
        patterns << data.textureData.loadLinkedPattern(resourcesInterface);
    }
    if (data.isEnabled) {   // CONCEPTFORGE: patch 7
        for (int i = 0, n = cfVariationCount(setting.data()); i < n; ++i) {
            KisEmbeddedTextureData variant;
            variant.read(setting.data(), QStringLiteral("Cf/TexVar/%1/").arg(i));
            if (!variant.isNull() && variant.patternBase64.isEmpty()) patterns << variant.loadLinkedPattern(resourcesInterface);
        }
    }

    return patterns;
}

bool KisTextureOption::applyingGradient() const
{
    return m_texturingMode == KisTextureOptionData::GRADIENT;
}

bool KisTextureOption::applyingGradient(const KisPropertiesConfiguration *settings)
{
    KisTextureOptionData data;
    data.read(settings);

    return data.texturingMode == KisTextureOptionData::GRADIENT;
}

bool KisTextureOption::requiresEffectiveCompositeOp(const KisPropertiesConfiguration *settings)
{
    KisTextureOptionData data;
    data.read(settings);

    return data.isEnabled && data.autoInvertOnErase;
}


void KisTextureOption::applyLightness(KisFixedPaintDeviceSP dab, const QPoint& offset, const KisPaintInformation& info) {
    const KisTextureMaskInfoSP maskInfo = cfMaskFor(info);   // CONCEPTFORGE: patch 7
    if (!m_enabled) return;
    if (!maskInfo->isValid()) return;

    KisPaintDeviceSP mask = maskInfo->mask();

    const QRect rect = dab->bounds();
    const QRect maskBounds = maskInfo->maskBounds();

    KisCachedPaintDevice::Guard g(mask, KoColorSpaceRegistry::instance()->rgb8(), m_cachedPaintDevice);
    KisPaintDeviceSP fillMaskDevice = g.device();

    int x = offset.x() % maskBounds.width() - effectiveOffsetX(info);
    int y = offset.y() % maskBounds.height() - effectiveOffsetY(info);

    const QRect maskPatchRect = QRect(x, y, rect.width(), rect.height());

    KisFillPainter fillMaskPainter(fillMaskDevice);
    fillMaskPainter.setCompositeOpId(COMPOSITE_COPY);
    fillMaskPainter.fillRect(kisGrowRect(maskPatchRect, 1), mask, maskBounds);
    fillMaskPainter.end();

    qreal pressure = (m_cfConstantDepth ? m_cfDepth : m_strengthOption.apply(info));
    quint8* dabData = dab->data();

    KisSequentialConstIterator it(fillMaskDevice, QRect(x, y, rect.width(), rect.height()));
    while (it.nextPixel()) {
        const QRgb *maskQRgb = reinterpret_cast<const QRgb*>(it.oldRawData());
        dab->colorSpace()->fillGrayBrushWithColorAndLightnessWithStrength(dabData, maskQRgb, dabData, pressure, 1);
        dabData += dab->pixelSize();
    }
}

void KisTextureOption::applyGradient(KisFixedPaintDeviceSP dab, const QPoint& offset, const KisPaintInformation& info) {
    const KisTextureMaskInfoSP maskInfo = cfMaskFor(info);   // CONCEPTFORGE: patch 7
    if (!m_enabled) return;
    if (!maskInfo->isValid()) return;

    KIS_SAFE_ASSERT_RECOVER_RETURN(m_gradient && m_gradient->valid());

    KisPaintDeviceSP mask = maskInfo->mask();
    const QRect maskBounds = maskInfo->maskBounds();
    QRect rect = dab->bounds();

    KisCachedPaintDevice::Guard g(mask, KoColorSpaceRegistry::instance()->rgb8(), m_cachedPaintDevice);
    KisPaintDeviceSP fillDevice = g.device();

    int x = offset.x() % maskBounds.width() - effectiveOffsetX(info);
    int y = offset.y() % maskBounds.height() - effectiveOffsetY(info);

    const QRect maskPatchRect = QRect(x, y, rect.width(), rect.height());

    KisFillPainter fillPainter(fillDevice);
    fillPainter.setCompositeOpId(COMPOSITE_COPY);
    fillPainter.fillRect(kisGrowRect(maskPatchRect, 1), mask, maskBounds);
    fillPainter.end();

    qreal pressure = (m_cfConstantDepth ? m_cfDepth : m_strengthOption.apply(info));
    quint8* dabData = dab->data();

    //for gradient textures...
    KoMixColorsOp* colorMix = dab->colorSpace()->mixColorsOp();
    qint16 colorWeights[2];
    colorWeights[0] = qRound(pressure * 255);
    colorWeights[1] = 255 - colorWeights[0];
    quint8* colors[2];
    m_cachedGradient.setColorSpace(dab->colorSpace()); //Change colorspace here so we don't have to convert each pixel drawn

    KisHLineIteratorSP iter = fillDevice->createHLineIteratorNG(x, y, rect.width());
    for (int row = 0; row < rect.height(); ++row) {
        for (int col = 0; col < rect.width(); ++col) {

            const QRgb* maskQRgb = reinterpret_cast<const QRgb*>(iter->oldRawData());
            qreal gradientvalue = qreal(qGray(*maskQRgb))/255.0;//qreal(*iter->oldRawData()) / 255.0;
            KoColor paintcolor;
            paintcolor.setColor(m_cachedGradient.cachedAt(gradientvalue), dab->colorSpace());
            qreal paintOpacity = paintcolor.opacityF() * (qreal(qAlpha(*maskQRgb)) / 255.0);
            paintcolor.setOpacity(qMin(paintOpacity, dab->colorSpace()->opacityF(dabData)));
            colors[0] = paintcolor.data();
            KoColor dabColor(dabData, dab->colorSpace());
            colors[1] = dabColor.data();
            colorMix->mixColors(colors, colorWeights, 2, dabData);

            iter->nextPixel();
            dabData += dab->pixelSize();
        }
        iter->nextRow();
    }
}

void KisTextureOption::apply(KisFixedPaintDeviceSP dab, const QPoint &offsetIn, const KisPaintInformation & info)
{
    const KisTextureMaskInfoSP maskInfo = cfMaskFor(info);   // CONCEPTFORGE: patch 7
    // CONCEPTFORGE: Photoshop's Texture Each Tip: the pattern is laid on every dab from its own corner,
    // not on the canvas, so each dab carries the same texture (patch 19)
    const QPoint offset = m_cfEachTip ? QPoint(0, 0) : offsetIn;
    if (!m_enabled) return;
    if (!maskInfo->isValid()) return;

    if (m_texturingMode == KisTextureOptionData::LIGHTNESS) {
        applyLightness(dab, offset, info);
        return;
    }
    else if (m_texturingMode == KisTextureOptionData::GRADIENT && m_gradient) {
        applyGradient(dab, offset, info);
        return;
    }

    QRect rect = dab->bounds();
    KisPaintDeviceSP mask = maskInfo->mask();
    const QRect maskBounds = maskInfo->maskBounds();

    KisCachedPaintDevice::Guard g(mask, KoColorSpaceRegistry::instance()->alpha8(), m_cachedPaintDevice);
    KisPaintDeviceSP maskPatch = g.device();

    int x = offset.x() % maskBounds.width() - effectiveOffsetX(info);
    int y = offset.y() % maskBounds.height() - effectiveOffsetY(info);

    const QRect maskPatchRect = QRect(x, y, rect.width(), rect.height());

    KisFillPainter fillPainter(maskPatch);
    fillPainter.setCompositeOpId(COMPOSITE_COPY);
    fillPainter.fillRect(kisGrowRect(maskPatchRect, 1), mask, maskBounds);
    fillPainter.end();

    // Compute final strength
    qreal strength = (m_cfConstantDepth ? m_cfDepth : m_strengthOption.apply(info));

    // Select mask compositing op
    KoChannelInfo::enumChannelValueType alphaChannelType = KoChannelInfo::UINT8;
    int alphaChannelOffset = -1;

    const QList<KoChannelInfo *> channels = dab->colorSpace()->channels();
    for (quint32 i = 0; i < dab->pixelSize(); i++) {
        if (channels[i]->channelType() == KoChannelInfo::ALPHA) {
            // TODO: check correctness for 16bits!
            alphaChannelOffset = channels[i]->pos()/* * channels[i]->size()*/;
            alphaChannelType = channels[i]->channelValueType();
            break;
        }
    }

    KIS_SAFE_ASSERT_RECOVER (alphaChannelOffset >= 0) {
        alphaChannelOffset = 0;
    }

    QScopedPointer<KisMaskingBrushCompositeOpBase> compositeOp;
    QString compositeOpId;

    switch (m_texturingMode) {
    case KisTextureOptionData::MULTIPLY: compositeOpId = COMPOSITE_MULT; break;
    case KisTextureOptionData::SUBTRACT: compositeOpId = COMPOSITE_SUBTRACT; break;
    case KisTextureOptionData::DARKEN: compositeOpId = COMPOSITE_DARKEN; break;
    case KisTextureOptionData::OVERLAY: compositeOpId = COMPOSITE_OVERLAY; break;
    case KisTextureOptionData::COLOR_DODGE: compositeOpId = COMPOSITE_DODGE; break;
    case KisTextureOptionData::COLOR_BURN: compositeOpId = COMPOSITE_BURN; break;
    case KisTextureOptionData::LINEAR_DODGE: compositeOpId = COMPOSITE_LINEAR_DODGE; break;
    case KisTextureOptionData::LINEAR_BURN: compositeOpId = COMPOSITE_LINEAR_BURN; break;
    case KisTextureOptionData::HARD_MIX_PHOTOSHOP: compositeOpId = COMPOSITE_HARD_MIX_PHOTOSHOP; break;
    case KisTextureOptionData::HARD_MIX_SOFTER_PHOTOSHOP: compositeOpId = COMPOSITE_HARD_MIX_SOFTER_PHOTOSHOP; break;
    case KisTextureOptionData::HEIGHT: compositeOpId = "height"; break;
    case KisTextureOptionData::LINEAR_HEIGHT: compositeOpId = "linear_height"; break;
    case KisTextureOptionData::HEIGHT_PHOTOSHOP: compositeOpId = "height_photoshop"; break;
    case KisTextureOptionData::LINEAR_HEIGHT_PHOTOSHOP: compositeOpId = "linear_height_photoshop"; break;
    default: return;
    }
    compositeOp.reset(KisMaskingBrushCompositeOpFactory::createForAlphaSrc(
                        compositeOpId, alphaChannelType, dab->pixelSize(),
                        alphaChannelOffset, strength, m_useSoftTexturing));

    // Apply the mask to the dab
    {
        quint8 *dabIt = nullptr;
        KisRandomConstAccessorSP maskPatchIt = maskPatch->createRandomConstAccessorNG();

        qint32 dabY = dab->bounds().y();
        qint32 maskPatchY = maskPatchRect.y();
        qint32 rowsRemaining = dab->bounds().height();
        const qint32 dabRowStride = dab->bounds().width() * dab->pixelSize();

        while (rowsRemaining > 0) {
            qint32 dabX = dab->bounds().x();
            qint32 maskPatchX = maskPatchRect.x();
            const qint32 numContiguousMaskPatchRows = maskPatchIt->numContiguousRows(maskPatchY);
            const qint32 rows = std::min(rowsRemaining, numContiguousMaskPatchRows);
            qint32 columnsRemaining = dab->bounds().width();

            while (columnsRemaining > 0) {
                const qint32 numContiguousMaskPatchColumns = maskPatchIt->numContiguousColumns(maskPatchX);
                const qint32 columns = std::min(columnsRemaining, numContiguousMaskPatchColumns);

                const qint32 maskPatchRowStride = maskPatchIt->rowStride(maskPatchX, maskPatchY);

                dabIt = dab->data() + (dabY * dab->bounds().width() + dabX) * dab->pixelSize();
                maskPatchIt->moveTo(maskPatchX, maskPatchY);

                compositeOp->composite(maskPatchIt->rawDataConst(), maskPatchRowStride,
                                       dabIt, dabRowStride,
                                       columns, rows);

                dabX += columns;
                maskPatchX += columns;
                columnsRemaining -= columns;

            }

            dabY += rows;
            maskPatchY += rows;
            rowsRemaining -= rows;
        }
    }
}
