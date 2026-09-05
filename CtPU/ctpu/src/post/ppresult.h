// SPDX-License-Identifier: GPL-2.0-or-later
// Last edited: 2026-08-19 06:48:51 UTC

#pragma once

#include <QReadWriteLock>
#include <QVector3D>

#include "hantekprotocol/types.h"
#include "utils/printutils.h"
#include <vector>

/// \brief Struct for a array of sample values.
struct SampleValues {
    std::vector< double > samples; ///< Vector holding the sampling data
    double interval = 0.0;         ///< The interval between two sample values
};

/// \brief Struct for the analyzed data.
struct DataChannel {
    SampleValues voltage;          ///< The time-domain voltage levels (V)
    SampleValues spectrum;         ///< The frequency-domain power levels (dB)
    bool valid = true;             ///< Not clipped, distorted, dropouts etc.
    double vmin = 0.0;             ///< The minimum sample value of _displayed_ part of trace
    double vmax = 0.0;             ///< The maximum sample value of _displayed_ part of trace
    double rms = 0.0;              ///< The DC + AC rms value of the signal = sqrt( dc * dc + acc * ac )
    double dBmin = 0.0;            ///< The minimum magnitude value
    double dBmax = 0.0;            ///< The maximum magnitude value
    double dc = 0.0;               ///< The DC bias of the signal
    double ac = 0.0;               ///< The AC rms value of the signal
    double dB = 0.0;               ///< The AC rms value as dB (dBV or other depending on config)
    double frequency = 0.0;        ///< The frequency of the signal
    QString note = "";             ///< The note value of the frequency
    double thd = 0.0;              ///< The THD value
    double pulseWidth1 = 0.0;      ///< The width of the triggered pulse
    double pulseWidth2 = 0.0;      ///< The width of the following pulse
    /// \name CCtPU measurement set (calibration source candidates)
    /// v1 scope confirmed in chat: 10-90% band convention for Top/Base,
    /// per-edge Overshoot+/Overshoot-, single-cycle window from the trigger
    /// (triggeredPosition -> triggeredPosition + (pulseWidth1+pulseWidth2)*samplerate).
    /// Computed in SpectrumGenerator::process() alongside dc/ac/vmin/vmax.
    ///@{
    double top = 0.0;              ///< Mean of samples in the upper 10% band of [vmin,vmax] (robust vs bare vmax)
    double base = 0.0;             ///< Mean of samples in the lower 10% band of [vmin,vmax] (robust vs bare vmin)
    double amplitude = 0.0;        ///< top - base (NOT the same as vmax-vmin/Vpp, which includes overshoot)
    double overshootRise = 0.0;    ///< Local peak above `top` just after the rising edge of one cycle
    double overshootFall = 0.0;    ///< Local dip below `base` just after the falling edge of one cycle
    double dcCycle = 0.0;          ///< DC mean over exactly one trigger-gated cycle (falls back to `dc` if untriggered)
    double stdDevCycle = 0.0;      ///< Std-dev/AC-rms over exactly one trigger-gated cycle (falls back to `ac` if untriggered)
    ///@}
    /// \brief Per-channel physical unit string (TZ §2.4.2).
    /// Set by PostProcessing::convertData from scope->voltage[channel].ctpuUnit.
    /// Replaces the previous `Unit voltageUnit` enum, since CtPU units are
    /// arbitrary user strings (°C, kPa, A, W, Ω, …) and cannot be captured by
    /// the fixed `Unit` enum.
    QString physicalUnit = "V";
    /// \deprecated Backward-compat alias kept for code that still reads
    /// `voltageUnit`. Reflects `physicalUnit` at the time it was last written
    /// (UNIT_VOLTS for "V"/"" and UNIT_VOLTSQUARE for "V²", otherwise UNIT_NONE).
    Unit voltageUnit = UNIT_VOLTS;
};

typedef std::vector< QVector3D > ChannelGraph;
typedef std::vector< ChannelGraph > ChannelsGraphs;

/// Post processing results
class PPresult {
  public:
    explicit PPresult( unsigned int channelCount );

    /// \brief Returns the analyzed data (RO).
    /// \param channel Channel, whose data should be returned.
    const DataChannel *data( ChannelID channel ) const;
    /// \brief Returns the analyzed data (RW). The data structure can be modified.
    /// \param channel Channel, whose data should be returned.
    DataChannel *modifiableData( ChannelID channel );
    /// \return The maximum sample count of the last analyzed data. This assumes there is at least one channel.
    unsigned int sampleCount() const;
    unsigned int channelCount() const;

    /// sw trigger status
    bool softwareTriggerTriggered = false;
    /// skip samples at start of channel to get triggered trace on screen
    int triggeredPosition = 0; ///< Not triggered
    double pulseWidth1 = 0.0;  ///< The width of the triggered pulse
    double pulseWidth2 = 0.0;  ///< The width of the following pulse
    unsigned tag;              ///< track individual sample blocks (debug support)
    /// Capture wall-clock ms, copied from DSOsamples::captureTimestampMs.
    /// 0 = unknown (e.g. synthetic test data). GUI shows now-minus-this as
    /// the frame's age (D-05): a number on screen is only trustworthy
    /// together with how stale it is.
    qint64 captureTimestampMs = 0;

    ChannelsGraphs vaChannelSpectrum;
    ChannelsGraphs vaChannelVoltage;
    ChannelsGraphs vaChannelHistogram;
    /// \brief Кривые XY-режима, по одной на слот `DsoSettingsScope::xyCurves`.
    /// [FIX] Индексируется НОМЕРОМ КРИВОЙ, а не каналом. Раньше кривые
    /// складывались в `vaChannelVoltage[yChannel]`, из-за чего две кривые с
    /// общим Y-каналом сливались в одну ломаную (вторая дописывалась в хвост
    /// первой), а кривая с неактивным каналом своим clear() стирала уже
    /// построенную чужую. Каналов 6, слотов 4 — совпадение Y неизбежно.
    ChannelsGraphs vaXYCurves;

  private:
    std::vector< DataChannel > analyzedData; ///< The analyzed data for each channel
};
