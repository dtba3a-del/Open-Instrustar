// SPDX-License-Identifier: GPL-2.0-or-later

#include <QDebug>

#include "mathstack.h"
#include "ctpu.h"

#include <algorithm>
#include <cmath>


MathStack::MathStack( const DsoSettingsScope *scope, unsigned realChannelCount )
    : scope( scope ), realChannelCount( realChannelCount ) {
    if ( scope && scope->verboseLevel > 1 )
        qDebug() << " MathStack::MathStack()";
}


void MathStack::calculate( DSOsamples &result ) {
    if ( !scope )
        return;

    const int maxMath = DsoSettingsScope::maxMathChannels;
    const int neededSlots = int( realChannelCount ) + maxMath;
    if ( int( result.data.size() ) < neededSlots )
        result.data.resize( neededSlots );

    // Also keep the per-channel physicalUnits vector in sync.
    if ( int( result.physicalUnits.size() ) < neededSlots )
        result.physicalUnits.resize( neededSlots, QStringLiteral( "V" ) );

    for ( int i = 0; i < maxMath; ++i ) {
        const auto &cfg = scope->mathStack[ i ];
        const int mathIndex = int( realChannelCount ) + i;

        if ( !cfg.enabled ) {
            // Disabled math channel — clear its output so stale data doesn't leak.
            result.data[ mathIndex ].clear();
            result.physicalUnits[ mathIndex ] = cfg.ctpuUnit;
            continue;
        }

        const int srcA = int( cfg.srcA );
        const int srcB = int( cfg.srcB );
        // Stack dependency: srcA and srcB must be strictly before mathIndex (TZ §5.2.2).
        if ( srcA >= mathIndex || srcB >= mathIndex || srcA < 0 || srcB < 0 ) {
            result.data[ mathIndex ].clear();
            continue;
        }
        if ( result.data[ srcA ].empty() || result.data[ srcB ].empty() ) {
            result.data[ mathIndex ].clear();
            continue;
        }

        const size_t n = std::min( result.data[ srcA ].size(), result.data[ srcB ].size() );
        result.data[ mathIndex ].resize( n );

        const double sign = cfg.invert ? -1.0 : 1.0;
        const auto &a = result.data[ srcA ];
        const auto &b = result.data[ srcB ];
        auto &out = result.data[ mathIndex ];

        switch ( cfg.op ) {
        case Dso::MathOp::ADD:
            for ( size_t j = 0; j < n; ++j )
                out[ j ] = sign * ( a[ j ] + b[ j ] );
            break;
        case Dso::MathOp::SUB:
            for ( size_t j = 0; j < n; ++j )
                out[ j ] = sign * ( a[ j ] - b[ j ] );
            break;
        case Dso::MathOp::MUL:
            for ( size_t j = 0; j < n; ++j )
                out[ j ] = sign * ( a[ j ] * b[ j ] );
            break;
        case Dso::MathOp::DIV:
            // Guard against divide-by-zero (TZ §5.5.1) — emit 0 when |b| < 1e-15.
            for ( size_t j = 0; j < n; ++j )
                out[ j ] = sign * ( std::abs( b[ j ] ) < 1e-15 ? 0.0 : a[ j ] / b[ j ] );
            break;
        }

        // Clipping propagation (TZ §5.5.1) — bit mathIndex mirrors srcA/srcB clipping.
        const unsigned mathMask = 1u << mathIndex;
        if ( ( result.clipped & ( 1u << srcA ) ) || ( result.clipped & ( 1u << srcB ) ) )
            result.clipped |= mathMask;
        else
            result.clipped &= ~mathMask;

        // Apply CtPU to the math channel output (manual FORMULA only — TZ §5.4).
        CtPU::applyToVector( out, cfg.ctpuK, cfg.ctpuB );
        result.physicalUnits[ mathIndex ] = cfg.ctpuUnit;
    }
}
