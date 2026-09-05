// SPDX-License-Identifier: GPL-3.0-or-later
// Last edited: 2026-09-03 UTC

#include "ivdsounits.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace IVdso {

bool acDcSupported( int isSupportAcDcReturn ) { return isSupportAcDcReturn == 0; }

RangeMv rangeMvFromVoltsPerDiv( double voltsPerDiv, int divsVoltage ) {
    if ( voltsPerDiv <= 0.0 || divsVoltage <= 0 )
        return RangeMv{ 0, 0 };
    // Половина экрана вверх и половина вниз: полный размах — divsVoltage
    // делений. ×1000 — перевод в милливольты, единицу настроек вендора.
    const double halfMv = voltsPerDiv * divsVoltage / 2.0 * 1000.0;
    const int mv = int( std::lround( halfMv ) );
    return RangeMv{ -mv, mv };
}

double voltsPerDivFromRangeMv( const RangeMv &range, int divsVoltage ) {
    if ( divsVoltage <= 0 )
        return 0.0;
    const double spanMv = double( range.maxMv ) - double( range.minMv );
    if ( spanMv <= 0.0 )
        return 0.0;
    return spanMv / 1000.0 / double( divsVoltage );
}

unsigned int pointsFromKb( unsigned int kb ) { return kb * 1024u; }

unsigned int kbFromPoints( unsigned int points ) { return ( points + 1023u ) / 1024u; }

unsigned int nearestSupportedSamplerate( unsigned int requested,
                                         const std::vector< unsigned int > &supported ) {
    if ( supported.empty() )
        return 0;
    unsigned int best = supported.front();
    // Расстояние берётся по логарифму: ряд прибора кратный (1M, 4M, 8M,
    // 16M, 48M), и по линейной разности запрос 2 MSps ушёл бы к 1 MSps
    // так же уверенно, как к 4 MSps, хотя по относительной ошибке они
    // равноудалены, а вниз промахиваться дороже — теряется полоса.
    double bestDist = 1e300;
    const double want = std::log( double( requested ? requested : 1u ) );
    for ( unsigned int s : supported ) {
        if ( !s )
            continue;
        const double d = std::fabs( std::log( double( s ) ) - want );
        // Сравнение с допуском, а не точное: у кратного ряда запрос
        // ровно посередине (2 MSps между 1 и 4) даёт две логарифмически
        // равные ветки, и точное `==` на них зависело бы от последних
        // битов мантиссы. Ничья разрешается ВВЕРХ: промах вниз теряет
        // полосу, промах вверх — только память.
        const double eps = 1e-12 * ( bestDist > 1.0 ? bestDist : 1.0 );
        if ( d < bestDist - eps || ( d <= bestDist + eps && s > best ) ) {
            bestDist = d;
            best = s;
        }
    }
    return best;
}

uint64_t deviceId( unsigned int id0, unsigned int id1 ) {
    return ( uint64_t( id1 ) << 32 ) | uint64_t( id0 );
}

std::string modelName( unsigned int sampleRateMax, bool hasDds ) {
    switch ( sampleRateMax ) {
    case 48000000u:
        return hasDds ? "ISDS205B" : "ISDS205A";
    case 60000000u:
        return hasDds ? "ISDS2062B" : "ISDS2062A";
    case 100000000u:
        return "ISDS210B";
    case 200000000u:
        return hasDds ? "ISDS220B" : "ISDS220A";
    default:
        break;
    }
    std::ostringstream os;
    os << "UNKNOWN(sr_max=" << sampleRateMax << ")";
    return os.str();
}

int adcBits( const std::string &model ) {
    return model.find( "2062" ) != std::string::npos ? 12 : 8;
}

bool clipped( int isVoltageDatasOutRangeReturn ) { return isVoltageDatasOutRangeReturn == 1; }

unsigned int commonLength( unsigned int nCh1, unsigned int nCh2 ) { return std::min( nCh1, nCh2 ); }

double codeDifference( double v1, double v2, double resolution ) {
    if ( resolution <= 0.0 )
        return 0.0;
    return ( v1 - v2 ) / resolution;
}

bool resolutionPlausible( double resolution, const RangeMv &range, int bits, double sampleToVolt,
                          double tolerance ) {
    if ( !( resolution > 0.0 ) || bits <= 0 || tolerance < 1.0 )
        return false;
    const double spanV = ( double( range.maxMv ) - double( range.minMv ) ) / 1000.0;
    if ( !( spanV > 0.0 ) )
        return false;
    const double fullScaleV = resolution * std::pow( 2.0, bits ) * sampleToVolt;
    const double ratio = fullScaleV / spanV;
    return ratio >= 1.0 / tolerance && ratio <= tolerance;
}

std::string calibrationFileName( const std::string &model, uint64_t id ) {
    // Метка "pathA" не украшение: заводская калибровка пути A и наша
    // калибровка пути B — разные величины одного прибора, и файл с
    // общим именем подменил бы одну другой без единого сообщения.
    std::ostringstream os;
    os << "pathA-" << ( model.empty() ? "UNKNOWN" : model ) << '-' << id << ".conf";
    return os.str();
}

} // namespace IVdso
