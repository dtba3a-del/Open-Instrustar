// SPDX-License-Identifier: GPL-2.0-or-later
// Last edited: 2026-08-11 11:31:17 UTC

#pragma once

#include "controlsettings.h"
#include "dsosamples.h"
#include "scopesettings.h"

class Triggering {
  public:
    explicit Triggering( const DsoSettingsScope *scope, const Dso::ControlSettings &controlsettings );
    int searchTriggeredPosition( DSOsamples &result );
    bool provideTriggeredData( DSOsamples &result );
    int getTriggeredPositionRaw() { return triggeredPositionRaw; }
    void resetTriggeredPositionRaw() { triggeredPositionRaw = 0; }

  private:
    const DsoSettingsScope *scope;
    const Dso::ControlSettings &controlsettings;
    int searchTriggerPoint( DSOsamples &result, Dso::Slope dsoSlope, int startPos, const std::vector< double > &smoothed,
                             unsigned smoothOffset );
    Dso::Slope mirrorSlope( Dso::Slope slope ) {
        return ( slope == Dso::Slope::Positive ? Dso::Slope::Negative : Dso::Slope::Positive );
    }
    int triggeredPositionRaw = 0; // not triggered
};
