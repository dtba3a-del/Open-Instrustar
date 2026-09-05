// SPDX-License-Identifier: GPL-2.0-or-later
//
// CtPU — Conversion to Physical Units.
// Linear conversion of ADC samples (in volts) into arbitrary physical units
// (°C, kPa, A, W, Ω, ...) using P = k·V + b.
//
// All functions in this header are inline and allocation-free so they can be
// called from the hot path of the pipeline (HantekDsoControl::stateMachine).
// See TZ section 3 for the specification.

#pragma once

#include <cmath>
#include <vector>

namespace CtPU {

/// \brief CtPU operating mode.
/// OFF     — samples stay in volts (default).
/// FORMULA — apply P = k·V + b with user-supplied k and b.
/// CCTPU   — two-point calibration: zero V → physical 0, span V → physical P1.
///           CCtPU is only available for physical channels; math channels use FORMULA.
enum class Mode { OFF, FORMULA, CCTPU };

/// \brief Apply CtPU to a single sample.
static inline double apply( double sample, double k, double b ) {
    return k * sample + b;
}

/// \brief Apply CtPU in-place to a vector of samples.
/// Hot-path safe: no allocations beyond the existing vector, no virtual calls.
static inline void applyToVector( std::vector< double > &samples, double k, double b ) {
    if ( k == 1.0 && b == 0.0 )
        return; // identity, nothing to do
    for ( double &s : samples )
        s = k * s + b;
}

/// \brief Calculate (k, b) from a two-point CCtPU calibration.
/// \param zeroV          Voltage measured at the physical zero point (V).
/// \param spanV          Voltage measured at the physical span point (V).
/// \param spanPhysical   Physical value at the span point.
/// \param[out] k         Computed sensitivity [physical/V].
/// \param[out] b         Computed offset [physical].
/// \return true on success; false if dV is too small (k, b left unchanged on failure).
static inline bool calculateFromCalibration( double zeroV, double spanV, double spanPhysical, double &k, double &b ) {
    double dV = spanV - zeroV;
    if ( std::abs( dV ) < 1e-12 ) {
        return false; // insufficient dynamic range
    }
    k = spanPhysical / dV;
    b = -k * zeroV;
    return true;
}

} // namespace CtPU
