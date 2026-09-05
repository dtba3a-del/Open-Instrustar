// SPDX-License-Identifier: GPL-2.0-or-later

#include "controlsettings.h"
#include "hantekprotocol/definitions.h"
#include "scopesettings.h"

namespace Dso {

ControlSettings::ControlSettings( const ControlSamplerateLimits *limits, size_t channelCount ) : cmdGetCalibration() {
    samplerate.limits = limits;
    // [MOD] TZ §5 — size for real channels + full math-stack (was channelCount + 1).
    // trigger.level[channel] and voltage[channel] must be indexable for all
    // unified channel indices up to channelCount + maxMathChannels - 1.
    const size_t totalChannels = channelCount + DsoSettingsScope::maxMathChannels;
    trigger.level.resize( totalChannels ); // two physical + 4 math channels
    voltage.resize( totalChannels );       // two physical + 4 math channels
    calibrationValues = new Hantek::CalibrationValues;
    correctionValues = new Hantek::CalibrationValues;
}

ControlSettings::~ControlSettings() {
    delete calibrationValues;
    delete correctionValues;
}

} // namespace Dso
