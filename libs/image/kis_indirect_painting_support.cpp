/*
 *  SPDX-FileCopyrightText: 2004 Bart Coppens <kde@bartcoppens.be>
 *  SPDX-FileCopyrightText: 2010 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_indirect_painting_support.h"

#include <QReadWriteLock>

#include <KoCompositeOp.h>
#include "kis_layer.h"
#include "kis_paint_layer.h"
#include "kis_paint_device.h"
#include "kis_selection.h"
#include "kis_painter.h"
#include <KisFakeRunnableStrokeJobsExecutor.h>
#include "KisRunnableStrokeJobData.h"
#include "KisRunnableStrokeJobUtils.h"
#include "kis_transaction.h"
#include "kis_pointer_utils.h"
#include <KoColorSpace.h>
#include <kis_global.h>
#include <vector>


struct Q_DECL_HIDDEN KisIndirectPaintingSupport::Private {
    qreal wetEdgeStrength {0.0};  // CONCEPTFORGE: patch 18
    qreal wetEdgeRadius {0.0};
    // To simulate the indirect painting
    KisPaintDeviceSP temporaryTarget;
    QString compositeOp;
    qreal compositeOpacity;
    QBitArray channelFlags;
    KisSelectionSP selection;

    QReadWriteLock lock;
    bool finalMergeInProgress = true;
};


KisIndirectPaintingSupport::KisIndirectPaintingSupport()
    : d(new Private)
{
}

KisIndirectPaintingSupport::~KisIndirectPaintingSupport()
{
    delete d;
}

void KisIndirectPaintingSupport::setCurrentColor(const KoColor &color)
{
    Q_UNUSED(color);
}

void KisIndirectPaintingSupport::setTemporaryTarget(KisPaintDeviceSP t)
{
    d->temporaryTarget = t;
}

void KisIndirectPaintingSupport::setTemporaryCompositeOp(const QString &id)
{
    d->compositeOp = id;
}

void KisIndirectPaintingSupport::setTemporaryWetEdges(qreal strength, qreal radius)
{
    d->wetEdgeStrength = qBound(0.0, strength, 1.0);
    d->wetEdgeRadius = qMax(0.0, radius);
}

namespace {
// a box blur of width 2r + 1 along one axis, zero outside the picture, by a running sum
void boxBlur(const std::vector<float> &in, std::vector<float> &out, int w, int h, int r, bool horizontal)
{
    const int lines = horizontal ? h : w;
    const int length = horizontal ? w : h;
    const float norm = 1.0f / float(2 * r + 1);
    for (int line = 0; line < lines; ++line) {
        auto at = [&](int i) -> float {
            if (i < 0 || i >= length) return 0.0f;
            return horizontal ? in[line * w + i] : in[i * w + line];
        };
        float sum = 0.0f;
        for (int i = -r; i <= r; ++i) sum += at(i);
        for (int i = 0; i < length; ++i) {
            const float value = sum * norm;
            if (horizontal) out[line * w + i] = value; else out[i * w + line] = value;
            sum += at(i + r + 1) - at(i - r);
        }
    }
}
}  // namespace

KisPaintDeviceSP KisIndirectPaintingSupport::wetEdgeSource(KisPaintDeviceSP src, const QRect &rect) const
{
    if (d->wetEdgeStrength <= 0.0 || !src || rect.isEmpty()) return src;

    // The rim is where the stroke's own coverage, blurred over the radius, is still below one: at the
    // outline the blur is one half, a radius inside it is one. Two box passes make the blur smooth.
    const int r = qMax(1, qRound(d->wetEdgeRadius / 2.0));
    const QRect big = kisGrowRect(rect, 2 * r + 1);
    const int w = big.width();
    const int h = big.height();
    const KoColorSpace *cs = src->colorSpace();
    const int pixelSize = cs->pixelSize();

    QByteArray bytes(w * h * pixelSize, 0);
    src->readBytes(reinterpret_cast<quint8 *>(bytes.data()), big);
    std::vector<float> a(size_t(w) * h), b(size_t(w) * h);
    for (int i = 0; i < w * h; ++i) {
        a[i] = cs->opacityF(reinterpret_cast<const quint8 *>(bytes.constData()) + i * pixelSize);
    }
    boxBlur(a, b, w, h, r, true);
    boxBlur(b, a, w, h, r, false);
    boxBlur(a, b, w, h, r, true);
    boxBlur(b, a, w, h, r, false);

    KisPaintDeviceSP out = new KisPaintDevice(cs);
    KisPainter::copyAreaOptimized(rect.topLeft(), src, out, rect);
    const int n = rect.width() * rect.height();
    QByteArray outBytes(n * pixelSize, 0);
    out->readBytes(reinterpret_cast<quint8 *>(outBytes.data()), rect);
    QVector<quint8> mask(n);
    for (int y = 0; y < rect.height(); ++y) {
        for (int x = 0; x < rect.width(); ++x) {
            const float blurred = a[size_t(y + rect.top() - big.top()) * w + (x + rect.left() - big.left())];
            // 0 on the rim (blur <= 0.5), 1 inside (blur >= 1), smooth between
            float t = qBound(0.0f, (blurred - 0.5f) * 2.0f, 1.0f);
            t = t * t * (3.0f - 2.0f * t);
            mask[y * rect.width() + x] = quint8(255.0f * (1.0f - float(d->wetEdgeStrength) * t) + 0.5f);
        }
    }
    cs->applyAlphaU8Mask(reinterpret_cast<quint8 *>(outBytes.data()), mask.constData(), n);
    out->writeBytes(reinterpret_cast<const quint8 *>(outBytes.constData()), rect);
    return out;
}

void KisIndirectPaintingSupport::setTemporaryOpacity(qreal o)
{
    d->compositeOpacity = o;
}

void KisIndirectPaintingSupport::setTemporaryChannelFlags(const QBitArray& channelFlags)
{
    d->channelFlags = channelFlags;
}

void KisIndirectPaintingSupport::setTemporarySelection(KisSelectionSP selection)
{
    d->selection = selection;
}

void KisIndirectPaintingSupport::lockTemporaryTarget() const
{
    d->lock.lockForRead();
}

void KisIndirectPaintingSupport::lockTemporaryTargetForWrite() const
{
    d->lock.lockForWrite();
    d->finalMergeInProgress = true;
}

void KisIndirectPaintingSupport::unlockTemporaryTarget() const
{
    d->lock.unlock();
    d->finalMergeInProgress = false;
}

KisPaintDeviceSP KisIndirectPaintingSupport::temporaryTarget() const
{
    return d->temporaryTarget;
}

bool KisIndirectPaintingSupport::supportsNonIndirectPainting() const
{
    return true;
}

KisIndirectPaintingSupport::FinalMergeSuspenderSP KisIndirectPaintingSupport::trySuspendFinalMerge()
{
    return toQShared(d->finalMergeInProgress ? new FinalMergeSuspender(this) : nullptr);
}

QString KisIndirectPaintingSupport::temporaryCompositeOp() const
{
    return d->compositeOp;
}

KisSelectionSP KisIndirectPaintingSupport::temporarySelection() const
{
    return d->selection;
}

bool KisIndirectPaintingSupport::hasTemporaryTarget() const
{
    return d->temporaryTarget;
}

void KisIndirectPaintingSupport::setupTemporaryPainter(KisPainter *painter) const
{
     painter->setOpacityF(d->compositeOpacity);
     painter->setCompositeOpId(d->compositeOp);
     painter->setChannelFlags(d->channelFlags);
     painter->setSelection(d->selection);
}

void KisIndirectPaintingSupport::mergeToLayer(KisNodeSP layer, KUndo2Command *parentCommand, const KUndo2MagicString &transactionText, int timedID)
{
    QVector<KisRunnableStrokeJobData*> jobs;
    mergeToLayerThreaded(layer, parentCommand, transactionText, timedID, &jobs);

    /**
     * When merging, we use barrier jobs only for ensuring that the merge jobs
     * are not split by the update jobs. Merge jobs hold the shared lock, so
     * forcing them out of CPU will basically cause a deadlock. When running in
     * the fake executor, the jobs cannot be split anyway, so there is no danger
     * in that.
     */
    KisFakeRunnableStrokeJobsExecutor executor(KisFakeRunnableStrokeJobsExecutor::AllowBarrierJobs);
    executor.addRunnableJobs(implicitCastList<KisRunnableStrokeJobDataBase*>(jobs));
}

void KisIndirectPaintingSupport::mergeToLayerThreaded(KisNodeSP layer, KUndo2Command *parentCommand, const KUndo2MagicString &transactionText,int timedID, QVector<KisRunnableStrokeJobData*> *jobs)
{
    /**
     * We create the lock in an unlocked state to avoid a deadlock, when
     * layer-stack updating jobs push out the stroke jobs from the CPU and
     * start sleeping on lockTemporaryTarget().
     */

    WriteLockerSP sharedWriteLock(new WriteLocker(this, std::defer_lock));

    /**
     * Now wait for all update jobs to finish and lock the indirect target
     */
    KritaUtils::addJobBarrier(*jobs,
        [sharedWriteLock] () {
            sharedWriteLock->relock();
        });

    mergeToLayerImpl(layer->paintDevice(), parentCommand, transactionText,
                     timedID, true, sharedWriteLock,
                     jobs);
}

void KisIndirectPaintingSupport::mergeToLayerImpl(KisPaintDeviceSP dst, KUndo2Command *parentCommand, const KUndo2MagicString &transactionText, int timedID, bool cleanResources,
                                                  WriteLockerSP sharedWriteLock, QVector<KisRunnableStrokeJobData*> *jobs)
{
    struct SharedState {
        QScopedPointer<KisTransaction> transaction;
    };

    QSharedPointer<SharedState> sharedState(new SharedState());

    KritaUtils::addJobSequential(*jobs,
        [sharedState, sharedWriteLock, dst, parentCommand, transactionText, timedID] () {
            Q_UNUSED(sharedWriteLock); // just a RAII holder object for the lock

            /**
             * Move tool may not have an undo adapter
             */
             if (parentCommand) {
                 sharedState->transaction.reset(
                     new KisTransaction(transactionText, dst, parentCommand, timedID));
             }
        }
    );

    KisPaintDeviceSP src = d->temporaryTarget;
    Q_FOREACH (const QRect &rc, src->region().rects()) {
        KritaUtils::addJobConcurrent(*jobs,
            [this, rc, src, dst, sharedState, sharedWriteLock] () {
                Q_UNUSED(sharedWriteLock); // just a RAII holder object for the lock

                /**
                 * Brushes don't apply the selection, we apply that during the indirect
                 * painting merge operation. It is cheaper calculation-wise.
                 */

                KisPainter gc(dst);
                setupTemporaryPainter(&gc);
                this->writeMergeData(&gc, wetEdgeSource(src, rc), rc);  // CONCEPTFORGE: patch 18
            }
        );
    }

    KritaUtils::addJobSequential(*jobs,
        [this, sharedState, sharedWriteLock, cleanResources] () {
            Q_UNUSED(sharedWriteLock); // just a RAII holder object for the lock

            if (cleanResources) {
                releaseResources();
            }

            if (sharedState->transaction) {
                // the internal command is stored using
                // the link to the parent command
                (void) sharedState->transaction->endAndTake();
            }
        }
    );
}

void KisIndirectPaintingSupport::writeMergeData(KisPainter *painter, KisPaintDeviceSP src, const QRect &rc)
{
    painter->bitBlt(rc.topLeft(), src, rc);
}

void KisIndirectPaintingSupport::releaseResources()
{
    d->temporaryTarget = 0;
    d->selection = 0;
    d->compositeOp = COMPOSITE_OVER;
    d->compositeOpacity = OPACITY_OPAQUE_F;
    d->channelFlags.clear();
}

KisIndirectPaintingSupport::FinalMergeSuspender::FinalMergeSuspender(KisIndirectPaintingSupport *indirect)
    : m_lock(indirect)
{
    m_lock->unlockTemporaryTarget();
}

KisIndirectPaintingSupport::FinalMergeSuspender::~FinalMergeSuspender()
{
    m_lock->lockTemporaryTargetForWrite();
}
