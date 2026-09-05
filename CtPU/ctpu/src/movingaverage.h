// SPDX-License-Identifier: GPL-2.0-or-later
// Last edited: 2026-08-11 11:31:17 UTC
//
// MovingAverage — shared N-sample boxcar averaging core.
//
// Step 1 of the confirmed plan (chat): a single, reusable primitive used by
// both pre-trigger noise reduction (Step 2, feeds Triggering::searchTriggerPoint
// with smoothed samples so trigger-point detection is more noise-immune) and
// Digital Zoom (Step 4, feeds the display with a smoother trace at higher
// magnification). This header only provides the math; neither caller is
// wired up yet.
//
// Design law confirmed in chat for Digital Zoom (kept here, not in the UI
// layer, so it isn't reinvented/misremembered when Step 4 is built):
//
//     zoom = sqrt(N)      <=>      N = zoom^2      <=>      bits = log2(zoom)
//
// ЭТО НЕ САМОДЕЛЬНАЯ ФОРМУЛА — она совпадает с опубликованной практикой
// признанных производителей измерительной техники:
//
//   * Tektronix, режим HiRes (FAQ "increase in bits in resolution for HiRes
//     mode", а также app note "Tools to Boost Oscilloscope Measurement
//     Resolution to More than 11 Bits"):
//         Enhanced Resolution Bits = 0.5 * log2(D)
//     где D — коэффициент прореживания. Это в точности наш
//     effectiveBitsGained(N) = 0.5*log2(N).
//     Там же: "The HiRes smoothing algorithm performs a digital boxcar
//     filter on the decimated acquisition" — то есть используется именно
//     boxcar (скользящее среднее), как здесь;
//     и "The 3 dB bandwidth for HiRes mode is approximately 0.44 of the
//     sample rate" — наш cutoffHz() использует 0.443*SR/N, тот же
//     коэффициент.
//
//   * Tektronix 5 Series MSO: "the acquisition badge indicates the number of
//     vertical bits of resolution and the vertical badges for active
//     channels indicate the -3 dB bandwidth" — то есть промышленная практика
//     ровно та, что задумана здесь: пользователю показывают ПОЛУЧЕННЫЕ биты
//     и полосу как индикацию, а не как органы управления.
//
//   * US Patent 10534019 (Variable resolution oscilloscope) — каноническая
//     формулировка сетки: 8 делений по вертикали, диапазон +-4*VDIV,
//     256 кодов у 8-битного АЦП, ошибка квантования как шум.
//
// ВАЖНАЯ ОГОВОРКА (Teledyne LeCroy, app note "Differences Between ERES and
// HiRes", LAB 767 "ERES vs. Boxcar Averaging"): boxcar — не оптимальный
// фильтр. "The bandwidth reduction from a boxcar shaped FIR filter is
// larger than for a bell shaped filter of the same length", поэтому в ERES
// применяют линейно-фазовые FIR с лучшим соотношением "биты/полоса" и
// лучшей переходной характеристикой. То есть за те же добавленные биты
// boxcar отнимает БОЛЬШЕ полосы, чем сглаженный FIR. Это осознанный
// компромисс в пользу простоты и предсказуемости; замена на линейно-фазовый
// FIR — понятный путь улучшения, а не переделка архитектуры.
//
// И предупреждение оттуда же, совпадающее с тем, что мы намерили
// самостоятельно на медианном предфильтре (см. bintape.h): "if the scope now
// applies a boxcar average that decimates the sample record by a factor of
// 10 then there remains only one sample on the edge" — при большом N узкие
// детали (вершины, фронты) теряются. Контролировать по остатку/пику, а не
// по доле энергии.
//
// N-sample averaging shrinks uncorrelated (white) noise by sqrt(N), so
// displaying the trace at sqrt(N)x magnification keeps the *apparent* noise
// amplitude unchanged from 1x -- i.e. this is exactly the relationship that
// keeps visual smoothness constant as zoom increases. Effective-bit gain is
// bits = 0.5*log2(N) = log2(zoom): doubling zoom always costs exactly one
// bit, whether going 1x->2x or 64x->128x. This is why a *linear* zoom slider
// feels wrong (1x->2x is a huge jump, 99x->100x is nothing) and why a UI
// control should step linearly in bits (or log2(zoom)) instead -- that part
// is Step 4's job, this header just provides the conversion functions so
// Step 4 doesn't have to re-derive them.
//
// N, resulting cutoff frequency, and resulting effective bits are NOT meant
// to be separate user-facing controls -- only zoom is (see chat). The other
// values are computed and shown read-only (a label), never set directly.
//
// All functions here are inline and allocation-light so they're safe to
// call from a hot path.

#pragma once

#include <cmath>
#include <vector>

namespace MovingAverage {

/// \brief Centered N-sample moving average.
///
/// Returns a vector shorter by (N-1) samples: the first valid *centered*
/// output sample needs N/2 samples of context on each side, so the caller
/// should treat output[0] as corresponding to input index margin(N), i.e.
/// the time axis shifts right by margin(N) samples relative to the input.
/// The computation itself is a single O(n) sliding-sum scan (not O(n*N));
/// margin() tells the caller how much extra to over-capture on each side of
/// the region of interest so nothing needed ends up trimmed away.
///
/// N <= 1, or input shorter than N: returns the input unchanged (identity;
/// this is exactly the zoom=1x / bits=0 case, so no special-casing is
/// needed at the call site).
static inline std::vector< double > apply( const std::vector< double > &in, unsigned N ) {
    if ( N <= 1 || in.size() < N )
        return in;
    std::vector< double > out;
    out.reserve( in.size() - ( N - 1 ) );
    double sum = 0.0;
    for ( unsigned i = 0; i < N; ++i )
        sum += in[ i ];
    out.push_back( sum / N );
    for ( size_t i = N; i < in.size(); ++i ) {
        sum += in[ i ] - in[ i - N ];
        out.push_back( sum / N );
    }
    return out;
}

/// \brief Samples of "runway" the caller must over-capture on EACH side of
/// the region of interest so that, after apply(..., N), the full region of
/// interest is still covered by valid centered samples. Matches the "frame
/// is always larger than the minimum needed, trimmed by half the averaging
/// window at each end" behaviour confirmed in chat.
static inline unsigned margin( unsigned N ) {
    return N <= 1 ? 0 : ( N - 1 ) / 2;
}

/// \brief -3dB cutoff frequency of an N-sample moving average, in Hz, for a
/// given sample rate. Standard boxcar-filter approximation:
///     f_c ~= 0.443 * sampleRate / N
/// Совпадает с публикуемым Tektronix значением для HiRes ("approximately
/// 0.44 of the sample rate" при N=1, далее обратно пропорционально N).
/// Meant to be shown to the user as read-only information (a label), never
/// as an input control -- see the design note at the top of this file.
static inline double cutoffHz( unsigned N, double sampleRateHz ) {
    if ( N <= 1 || sampleRateHz <= 0.0 )
        return sampleRateHz;
    return 0.443 * sampleRateHz / double( N );
}

/// \brief Effective-bit gain from N-sample averaging of uncorrelated (white)
/// noise: bits = 0.5*log2(N). Equal to log2(zoom) under the zoom=sqrt(N) law
/// below -- kept as a separate function since Step 2 (trigger) cares about
/// this without any notion of "zoom" at all.
static inline double effectiveBitsGained( unsigned N ) {
    return N <= 1 ? 0.0 : 0.5 * std::log2( double( N ) );
}

/// \name Zoom <-> window-size conversion (the law confirmed in chat for Step 4)
///@{

/// \brief Window size N for a given zoom factor (zoom >= 1), rounded to the
/// nearest integer. Callers should also sanity-check N against the
/// available record length (via margin()) before using it.
static inline unsigned windowForZoom( double zoom ) {
    if ( zoom <= 1.0 )
        return 1;
    return unsigned( std::lround( zoom * zoom ) );
}

/// \brief Zoom factor that keeps visual smoothness constant for a given
/// window N (inverse of windowForZoom()).
static inline double zoomForWindow( unsigned N ) {
    return N <= 1 ? 1.0 : std::sqrt( double( N ) );
}
///@}

} // namespace MovingAverage
