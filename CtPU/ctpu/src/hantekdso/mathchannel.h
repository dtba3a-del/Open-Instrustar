// SPDX-License-Identifier: GPL-2.0-or-later
//
// Thin backward-compatibility wrapper around MathStack (TZ §5).
//
// The legacy MathChannel class exposed a single hard-wired math slot (MATH=2)
// driven by `DsoSettingsScopeVoltage::couplingOrMathIndex` (a Dso::MathMode).
// New code should use MathStack directly — it supports up to 4 configurable
// math channels with arbitrary (srcA, srcB, op) tuples. This wrapper keeps the
// old API alive for any external callers that still construct a MathChannel.
//
// The wrapper populates scope->mathStack[0] from the legacy settings on
// construction and forwards calculate() to an internal MathStack instance.

#pragma once

#include "dsosamples.h"
#include "mathstack.h"
#include "scopesettings.h"

class MathChannel {
  public:
    explicit MathChannel( const DsoSettingsScope *scope );
    void calculate( DSOsamples &result );

  private:
    const DsoSettingsScope *scope;
    MathStack mathStack;
};
