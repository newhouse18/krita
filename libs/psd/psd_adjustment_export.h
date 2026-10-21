// SPDX-License-Identifier: GPL-2.0-or-later
// CONCEPTFORGE: patch 31. The settings of a Krita filter layer as the tagged block Photoshop keeps for the matching adjustment layer
// (docs/spikes/psd-roundtrip.md, run 9), so that a PSD written by Concept Forge opens in Photoshop with its adjustments, not as empty layers.
#ifndef PSD_ADJUSTMENT_EXPORT_H
#define PSD_ADJUSTMENT_EXPORT_H

#include <QByteArray>
#include <QList>
#include <QPointF>
#include <kis_types.h>

#include "kritapsd_export.h"

namespace PsdAdjustmentExport
{
/**
 * The block key ("curv", "hue2", "nvrt", "post", "thrs", "blnc") and its data for ``config`` (a filter layer's settings), or false and an empty key
 * when Photoshop has no adjustment for that filter. The data is padded to a multiple of four bytes.
 */
KRITAPSD_EXPORT bool encode(KisFilterConfigurationSP config, QByteArray &key, QByteArray &data);

// the pieces, for tests
KRITAPSD_EXPORT QByteArray curvesBlock(const QList<QList<QPointF>> &channels);
KRITAPSD_EXPORT QByteArray hueSaturationBlock(bool colorize, int hue, int saturation, int lightness);
KRITAPSD_EXPORT QByteArray colorBalanceBlock(const int values[9], bool preserveLuminosity);
}

#endif
