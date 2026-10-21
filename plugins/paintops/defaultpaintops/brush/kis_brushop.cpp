/*
 *  SPDX-FileCopyrightText: 2002 Patrick Julien <freak@codepimps.org>
 *  SPDX-FileCopyrightText: 2004-2008 Boudewijn Rempt <boud@valdyas.org>
 *  SPDX-FileCopyrightText: 2004 Clarence Dang <dang@kde.org>
 *  SPDX-FileCopyrightText: 2004 Adrian Page <adrian@pagenet.plus.com>
 *  SPDX-FileCopyrightText: 2004 Cyrille Berger <cberger@cberger.net>
 *  SPDX-FileCopyrightText: 2010 Lukáš Tvrdý <lukast.dev@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_brushop.h"

#include <QRect>

#include <kis_image.h>
#include <kis_vec.h>
#include <kis_debug.h>

#include <KoColorTransformation.h>
#include <KoColor.h>

#include <kis_brush.h>
#include <kis_global.h>
#include <kis_paint_device.h>
#include <kis_painter.h>
#include <kis_brush_based_paintop_settings.h>
#include <kis_lod_transform.h>
#include <kis_paintop_plugin_utils.h>
#include "krita_utils.h"
#include "kis_algebra_2d.h"
#include <KisDabRenderingExecutor.h>
#include <KisDabCacheUtils.h>
#include <KisRenderedDab.h>
#include <kis_tool_freehand.h>
#include "KisBrushOpResources.h"

#include <KisRunnableStrokeJobData.h>
#include <KisRunnableStrokeJobUtils.h>
#include <KisRunnableStrokeJobsInterface.h>

#include <QThread>
#include "kis_image_config.h"
#include "kis_wrapped_rect.h"


KisBrushOp::KisBrushOp(const KisPaintOpSettingsSP settings, KisPainter *painter, KisNodeSP node, KisImageSP image)
    : KisBrushBasedPaintOp(settings, painter, SupportsGradientMode | SupportsLightnessMode)
    , m_sizeOption(settings.data())
    , m_ratioOption(settings.data())
    , m_rateOption(settings.data())
    , m_softnessOption(settings.data())
    , m_lightnessStrengthOption(settings.data())
    , m_spacingOption(settings.data())
    , m_scatterOption(settings.data())
    , m_sharpnessOption(settings.data())
    , m_rotationOption(settings.data())
    , m_opacityOption(settings.data(), node)
    , m_avgSpacing(50)
    , m_avgNumDabs(50)
    , m_avgUpdateTimePerDab(50)
    , m_idealNumRects(KisImageConfig(true).maxNumberOfThreads())
    , m_minUpdatePeriod(10)
    , m_maxUpdatePeriod(100)
{
    Q_UNUSED(image);
    Q_ASSERT(settings);

    m_cf = CfBrushExtras::Extras::read(settings.data());  // CONCEPTFORGE: patches 15, 16
    if (m_cf.wetEdges > 0.0) {
        m_cfWetGrow = qCeil(qBound(1.5, 0.2 * settings->paintOpSize(), 40.0)) + 3;  // patch 18
    }

    m_airbrushData.read(settings.data());
    m_rotationOption.applyFanCornersInfo(this);

    m_precisionOption.setHasImprecisePositionOptions(
        m_precisionOption.hasImprecisePositionOptions()
        || m_scatterOption.isChecked() || m_rotationOption.isChecked()
        || m_airbrushData.isChecked || m_cf.scatterCount > 1);  // CONCEPTFORGE: patch 15

    m_brush->notifyBrushIsGoingToBeClonedForStroke();

    KisBrushSP baseBrush = m_brush;
    auto resourcesFactory =
        [baseBrush, settings, painter] () {
            KisDabCacheUtils::DabRenderingResources *resources =
                new KisBrushOpResources(settings, painter);
            resources->brush = baseBrush->clone().dynamicCast<KisBrush>();

            return resources;
        };


    m_dabExecutor.reset(
        new KisDabRenderingExecutor(
                    painter->device()->compositionSourceColorSpace(),
                    resourcesFactory,
                    painter->runnableStrokeJobsInterface(),
                    &m_mirrorOption,
                    &m_precisionOption));
}

KisBrushOp::~KisBrushOp()
{
}

KisSpacingInformation KisBrushOp::paintAt(const KisPaintInformation& infoIn)
{
    // CONCEPTFORGE: Brush Pose replaces what the pen reports (patch 16)
    KisPaintInformation info = infoIn;
    if (m_cf.hasPose()) m_cf.applyPose(info);

    if (!painter()->device()) return KisSpacingInformation(1.0);

    KisBrushSP brush = m_brush;
    Q_ASSERT(brush);
    if (!brush)
        return KisSpacingInformation(1.0);

    if (!brush->canPaintFor(info))
        return KisSpacingInformation(1.0);

    qreal scale = m_sizeOption.apply(info);
    if (m_cf.psMixSize >= 0.0) scale = m_cf.psMixSize + (1.0 - m_cf.psMixSize) * scale;   // CONCEPTFORGE: patch 27
    scale *= m_cf.tiltSizeFactor(info);   // CONCEPTFORGE: patch 28
    const qreal bristleless = scale;   // the tip's size without the room a bristle tip's dots need (the spacing counts the size)
    scale *= m_cf.bristleScale;          // CONCEPTFORGE: patch 29
    scale *= KisLodTransform::lodToScale(painter()->device());
    if (checkSizeTooSmall(scale)) return KisSpacingInformation();

    qreal rotation = m_rotationOption.apply(info);
    rotation += m_cf.angleJitterFor(info);  // CONCEPTFORGE: patch 23
    rotation += m_cf.tiltAngleFor(info);    // CONCEPTFORGE: patch 28
    qreal ratio = m_ratioOption.apply(info);
    if (m_cf.psMixRoundness >= 0.0) ratio = m_cf.psMixRoundness + (1.0 - m_cf.psMixRoundness) * ratio;   // CONCEPTFORGE: patch 27
    ratio *= m_cf.tipRoundness;  // CONCEPTFORGE: patch 23

    // CONCEPTFORGE: patch 26. A Photoshop soft tip reaches past its size: its mask is drawn bigger (the spacing keeps the size)
    if (m_cf.roundTip && m_cf.softScale < 1.1) {
        // CONCEPTFORGE: patch 26. A thin hard tip is thicker in Photoshop than its roundness says (measured, lab 3, a hard tip at
        // roundness 10: 1 pixel thick -> 2.9, 2 -> 3.5, 4 -> 5.0, 8 -> 9.0; at roundness 25 a half pixel, at 50 nothing): the edge
        // antialiasing adds to the thin side
        // the tip's own sides (the brush's unturned width and height), not the bounding box of the turned mask
        const qreal w = brush->width() * scale, h = brush->height() * scale * ratio;
        const qreal major = qMax(w, h), minor = qMin(w, h);
        const qreal share = major > 0.0 ? minor / major : 1.0;
        qreal extra = 0.0;
        if (share <= 0.12) extra = minor < 3.0 ? 2.0 : 1.15;
        else if (share <= 0.3 && minor < 12.0) extra = 0.35;
        // the whole extra shows with the thin side upright; turned to 90 degrees about half of it does (lab 3: ink per column of a thin
        // tip at 0, 30, 45, 60 and 90 degrees)
        const qreal turn = rotation + brush->angle();
        extra *= 0.55 + 0.45 * cos(turn) * cos(turn);
        if (minor > 0.3 && extra > 0.0) ratio *= (minor + extra) / minor;
    }
    KisDabShape shape(scale * m_cf.softScale, ratio, rotation);

    qreal dabOpacity = OPACITY_OPAQUE_F;
    qreal dabFlow = OPACITY_OPAQUE_F;

    m_opacityOption.apply(info, &dabOpacity, &dabFlow);

    // CONCEPTFORGE: Scatter count: several marks per dab, each scattered on its own (patch 15)
    const int cfCount = m_cf.countFor(info);
    for (int cfMark = 0; cfMark < cfCount; ++cfMark) {
        const QPointF cursorPos =
            m_scatterOption.apply(info,
                                  brush->maskWidth(shape, 0, 0, info) / m_cf.softScale,   // patch 26: the tip's size, not the bigger mask
                                  brush->maskHeight(shape, 0, 0, info) / m_cf.softScale);

        KisDabCacheUtils::DabRequestInfo request(painter()->paintColor(),
                                                 cursorPos,
                                                 shape,
                                                 info,
                                                 m_softnessOption.apply(info),
                                                 m_lightnessStrengthOption.apply(info));

        m_dabExecutor->addDab(request, dabOpacity, dabFlow);
    }


    KisSpacingInformation spacingInfo =
        effectiveSpacing(bristleless * KisLodTransform::lodToScale(painter()->device()), rotation, &m_airbrushData, &m_spacingOption, info);

    // gather statistics about dabs
    m_avgSpacing(spacingInfo.scalarApprox());

    return spacingInfo;
}

struct KisBrushOp::UpdateSharedState
{
    // rendering data
    KisPainter *painter = 0;
    QList<KisRenderedDab> dabsQueue;

    // speed metrics
    QVector<QPointF> dabPoints;
    QElapsedTimer dabRenderingTimer;

    // final report
    QVector<QRect> allDirtyRects;
};

void KisBrushOp::addMirroringJobs(Qt::Orientation direction,
                                  QVector<QRect> &rects,
                                  UpdateSharedStateSP state,
                                  QVector<KisRunnableStrokeJobData*> &jobs)
{
    KritaUtils::addJobSequential(jobs, nullptr);

    /**
     * Some KisRenderedDab may share their devices, so we should mirror them
     * carefully, avoiding doing that twice. KisDabRenderingQueue is implemented in
     * a way that duplicated dabs can go only sequentially, one after another, so
     * we don't have to use complex deduplication algorithms here.
     */
    KisFixedPaintDeviceSP prevDabDevice = 0;
    for (KisRenderedDab &dab : state->dabsQueue) {
        const bool skipMirrorPixels = prevDabDevice && prevDabDevice == dab.device;

        KritaUtils::addJobConcurrent(jobs,
            [state, &dab, direction, skipMirrorPixels] () {
                state->painter->mirrorDab(direction, &dab, skipMirrorPixels);
            }
        );

        prevDabDevice = dab.device;
    }

    KritaUtils::addJobSequential(jobs, nullptr);

    for (QRect &rc : rects) {
        state->painter->mirrorRect(direction, &rc);

        KritaUtils::addJobConcurrent(jobs,
            [rc, state] () {
                state->painter->bltFixed(rc, state->dabsQueue);
            }
        );
    }

    state->allDirtyRects.append(rects);
}

std::pair<int, bool> KisBrushOp::doAsynchronousUpdate(QVector<KisRunnableStrokeJobData*> &jobs)
{
    bool someDabsAreStillInQueue = false;
    const bool hasPreparedDabsAtStart = m_dabExecutor->hasPreparedDabs();

    if (!m_updateSharedState && hasPreparedDabsAtStart) {

        m_updateSharedState = toQShared(new UpdateSharedState());
        UpdateSharedStateSP state = m_updateSharedState;

        state->painter = painter();

        {
            const qreal dabRenderingTime = m_dabExecutor->averageDabRenderingTime();
            const qreal totalRenderingTimePerDab = dabRenderingTime + m_avgUpdateTimePerDab.rollingMeanSafe();

            // we limit the number of fetched dabs to fit the maximum update period and not
            // make visual hiccups
            const int dabsLimit =
                totalRenderingTimePerDab > 0 ?
                    qMax(10, int(m_maxUpdatePeriod  / totalRenderingTimePerDab * m_idealNumRects)) :
                    -1;

            state->dabsQueue = m_dabExecutor->takeReadyDabs(painter()->hasMirroring(), dabsLimit, &someDabsAreStillInQueue);
        }

        KIS_SAFE_ASSERT_RECOVER_RETURN_VALUE(!state->dabsQueue.isEmpty(),
                                             std::make_pair(m_currentUpdatePeriod, false));

        const int diameter = m_dabExecutor->averageDabSize();
        const qreal spacing = m_avgSpacing.rollingMean();

        const int idealNumRects = m_idealNumRects;

        QVector<QRect> rects;

        // wrap the dabs if needed
        if (painter()->device()->defaultBounds()->wrapAroundMode()) {
            /**
             * In WA mode we do two things:
             *
             * 1) We ensure that the parallel threads do not access the same are on
             *    the image. For normal updates that is ensured by the code in KisImage
             *    and the scheduler. Here we should do that manually by adjusting 'rects'
             *    so that they would not intersect in the wrapped space.
             *
             * 2) We duplicate dabs, to ensure that all the pieces of dabs are painted
             *    inside the wrapped rect. No pieces are dabs are painted twice, because
             *    we paint only the parts intersecting the wrap rect.
             */

            const QRect wrapRect = painter()->device()->defaultBounds()->imageBorderRect();
            const WrapAroundAxis wrapAroundModeAxis = painter()->device()->defaultBounds()->wrapAroundModeAxis();

            QList<KisRenderedDab> wrappedDabs;

            Q_FOREACH (const KisRenderedDab &dab, state->dabsQueue) {
                const QVector<QPoint> normalizationOrigins =
                    KisWrappedRect::normalizationOriginsForRect(dab.realBounds(), wrapRect, wrapAroundModeAxis);

                Q_FOREACH(const QPoint &pt, normalizationOrigins) {
                    KisRenderedDab newDab = dab;

                    newDab.offset = pt;
                    rects.append(KisWrappedRect::clipToWrapRect(newDab.realBounds(), wrapRect, wrapAroundModeAxis));
                    wrappedDabs.append(newDab);
                }
            }

            state->dabsQueue = wrappedDabs;

        } else {
            // just get all rects
            Q_FOREACH (const KisRenderedDab &dab, state->dabsQueue) {
                rects.append(dab.realBounds());
            }
        }

        // split/merge rects into non-overlapping areas
        rects = KisPaintOpUtils::splitDabsIntoRects(rects,
                                                    idealNumRects, diameter, spacing);

        state->allDirtyRects = rects;

        Q_FOREACH (const KisRenderedDab &dab, state->dabsQueue) {
            state->dabPoints.append(dab.realBounds().center());
        }

        state->dabRenderingTimer.start();

        Q_FOREACH (const QRect &rc, rects) {
            KritaUtils::addJobConcurrent(jobs,
                [rc, state] () {
                    state->painter->bltFixed(rc, state->dabsQueue);
                }
            );
        }

        /**
         * After the dab has been rendered once, we should mirror it either one
         * (h __or__ v) or three (h __and__ v) times. This sequence of 'if's achieves
         * the goal without any extra copying. Please note that it has __no__ 'else'
         * branches, which is done intentionally!
         */
        if (state->painter->hasHorizontalMirroring()) {
            addMirroringJobs(Qt::Horizontal, rects, state, jobs);
        }

        if (state->painter->hasVerticalMirroring()) {
            addMirroringJobs(Qt::Vertical, rects, state, jobs);
        }

        if (state->painter->hasHorizontalMirroring() && state->painter->hasVerticalMirroring()) {
            addMirroringJobs(Qt::Horizontal, rects, state, jobs);
        }

        KritaUtils::addJobSequential(jobs,
                [state, this, someDabsAreStillInQueue] () {
                    Q_FOREACH(const QRect &rc, state->allDirtyRects) {
                        // CONCEPTFORGE: a wet edge reaches past the dab (patch 18)
                        state->painter->addDirtyRect(m_cfWetGrow > 0 ? kisGrowRect(rc, m_cfWetGrow) : rc);
                    }

                    state->painter->setAverageOpacity(state->dabsQueue.last().averageOpacity);

                    const int updateRenderingTime = state->dabRenderingTimer.elapsed();
                    const qreal dabRenderingTime = m_dabExecutor->averageDabRenderingTime();

                    m_avgNumDabs(state->dabsQueue.size());

                    const qreal currentUpdateTimePerDab = qreal(updateRenderingTime) / state->dabsQueue.size();
                    m_avgUpdateTimePerDab(currentUpdateTimePerDab);

                    /**
                     * NOTE: using currentUpdateTimePerDab in the calculation for the next update time instead
                     *       of the average one makes rendering speed about 40% faster. It happens because the
                     *       adaptation period is shorter than if it used
                     */
                    const qreal totalRenderingTimePerDab = dabRenderingTime + currentUpdateTimePerDab;

                    const int approxDabRenderingTime =
                        qreal(totalRenderingTimePerDab) * m_avgNumDabs.rollingMean() / m_idealNumRects;

                    m_currentUpdatePeriod =
                        someDabsAreStillInQueue ? m_minUpdatePeriod :
                        qBound(m_minUpdatePeriod, int(1.5 * approxDabRenderingTime), m_maxUpdatePeriod);


                    { // debug chunk
//                        ENTER_FUNCTION() << ppVar(state->allDirtyRects.size()) << ppVar(state->dabsQueue.size()) << ppVar(dabRenderingTime) << ppVar(updateRenderingTime);
//                        ENTER_FUNCTION() << ppVar(m_currentUpdatePeriod) << ppVar(someDabsAreStillInQueue);
                    }

                    // release all the dab devices
                    state->dabsQueue.clear();

                    m_updateSharedState.clear();
                }
        );
    } else if (m_updateSharedState && hasPreparedDabsAtStart) {
        someDabsAreStillInQueue = true;
    }

    return std::make_pair(m_currentUpdatePeriod, someDabsAreStillInQueue);
}

KisSpacingInformation KisBrushOp::updateSpacingImpl(const KisPaintInformation &info) const
{
    qreal sizeValue = m_sizeOption.apply(info);
    if (m_cf.psMixSize >= 0.0) sizeValue = m_cf.psMixSize + (1.0 - m_cf.psMixSize) * sizeValue;   // CONCEPTFORGE: patch 27
    const qreal scale = sizeValue * KisLodTransform::lodToScale(painter()->device());
    qreal rotation = m_rotationOption.apply(info);
    return effectiveSpacing(scale, rotation, &m_airbrushData, &m_spacingOption, info);
}

KisTimingInformation KisBrushOp::updateTimingImpl(const KisPaintInformation &info) const
{
    return KisPaintOpPluginUtils::effectiveTiming(&m_airbrushData, &m_rateOption, info);
}

void KisBrushOp::paintLine(const KisPaintInformation& pi1, const KisPaintInformation& pi2, KisDistanceInformation *currentDistance)
{
    if (m_sharpnessOption.isChecked() && m_brush && (m_brush->width() == 1) && (m_brush->height() == 1)) {

        if (!m_lineCacheDevice) {
            m_lineCacheDevice = source()->createCompositionSourceDevice();
        }
        else {
            m_lineCacheDevice->clear();
        }

        KisPainter p(m_lineCacheDevice);
        p.setPaintColor(painter()->paintColor());
        p.drawDDALine(pi1.pos(), pi2.pos());

        QRect rc = m_lineCacheDevice->extent();
        painter()->bitBlt(rc.x(), rc.y(), m_lineCacheDevice, rc.x(), rc.y(), rc.width(), rc.height());
    //fixes Bug 338011
    painter()->renderMirrorMask(rc, m_lineCacheDevice);
    }
    else {
        KisPaintOp::paintLine(pi1, pi2, currentDistance);
    }
}
