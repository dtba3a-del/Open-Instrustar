// SPDX-License-Identifier: GPL-2.0-or-later
//
// Math-stack: configurable virtual channels that replace the monolithic
// MathChannel (TZ §5). Up to `DsoSettingsScope::maxMathChannels` (4) math
// channels can be configured; each computes `sign × (srcA op srcB)` where
// srcA/srcB are unified channel indices and op ∈ {ADD, SUB, MUL, DIV}.
//
// The math-stack runs in the HantekDsoControl state machine *after* CtPU has
// been applied to the physical channels and *before* triggering. It writes its
// results into `DSOsamples::data[spec->channels + mathIndex]`. After the stack
// finishes, CtPU is applied to each math channel's output using that channel's
// own (k, b) formula — CCtPU is not available for math channels (TZ §5.4.1).
//
// Hot-path safety: `calculate()` uses no allocations inside the per-sample
// loop (vectors are resized once up-front), no virtual calls, and a plain
// switch/case for the operation. It is safe to call from
// HantekDsoControl::stateMachine().

#pragma once

#include "dsosamples.h"
#include "scopesettings.h"

class MathStack {
  public:
    explicit MathStack( const DsoSettingsScope *scope, unsigned realChannelCount );
    /// Compute all enabled math channels into `result.data[realChannels + i]`.
    /// Assumes physical channels (0..realChannelCount-1) are already populated
    /// and have had CtPU applied.
    void calculate( DSOsamples &result );

  private:
    const DsoSettingsScope *scope;
    unsigned realChannelCount; ///< Number of physical hardware channels (= spec->channels)
};
