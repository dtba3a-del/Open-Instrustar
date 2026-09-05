// SPDX-License-Identifier: GPL-2.0-or-later

#include <QCoreApplication>
#include <QDebug>

#include "mathchannel.h"
#include "mathmodes.h"


MathChannel::MathChannel( const DsoSettingsScope *scope )
    : scope( scope ), mathStack( scope, scope ? 2u : 0u ) {
    if ( scope && scope->verboseLevel > 1 )
        qDebug() << " MathChannel::MathChannel() (thin wrapper -> MathStack)";
}


void MathChannel::calculate( DSOsamples &result ) {
    // The new MathStack is driven entirely by scope->mathStack[]. The legacy
    // MathChannel class used scope->voltage[2].couplingOrMathIndex (a
    // Dso::MathMode) to pick among many built-in operations; with the
    // math-stack in place, only the four arithmetic ops (ADD/SUB/MUL/DIV) are
    // supported per TZ §5.3.2. We map the legacy mode to (srcA, srcB, op) here
    // so that old config files still produce a sensible M1 channel.
    if ( scope && !scope->mathStack.empty() ) {
        auto &m1 = const_cast< MathChannelConfig & >( scope->mathStack[ 0 ] );
        const Dso::MathMode legacyMode = Dso::getMathMode( scope->voltage[ scope->voltage.size() - DsoSettingsScope::maxMathChannels ] );
        switch ( legacyMode ) {
        case Dso::MathMode::ADD_CH1_CH2:
            m1.srcA = 0; m1.srcB = 1; m1.op = Dso::MathOp::ADD; break;
        case Dso::MathMode::SUB_CH2_FROM_CH1:
            m1.srcA = 0; m1.srcB = 1; m1.op = Dso::MathOp::SUB; break;
        case Dso::MathMode::SUB_CH1_FROM_CH2:
            m1.srcA = 1; m1.srcB = 0; m1.op = Dso::MathOp::SUB; break;
        case Dso::MathMode::MUL_CH1_CH2:
            m1.srcA = 0; m1.srcB = 1; m1.op = Dso::MathOp::MUL; break;
        default:
            // For all other legacy modes (LP/AC/DC/ABS/SIGN/AND/TRIG/...), fall
            // back to ADD_CH1_CH2 — these modes are out of scope for the
            // math-stack's four arithmetic ops (TZ §1.3.1).
            m1.srcA = 0; m1.srcB = 1; m1.op = Dso::MathOp::ADD; break;
        }
    }
    mathStack.calculate( result );
    // Preserve the legacy mathVoltageUnit field for any code that still reads it.
    if ( scope && !scope->mathStack.empty() ) {
        const auto &m1 = scope->mathStack[ 0 ];
        if ( m1.enabled && ( m1.op == Dso::MathOp::MUL ) )
            result.mathVoltageUnit = UNIT_VOLTSQUARE;
        else
            result.mathVoltageUnit = UNIT_VOLTS;
    }
}
