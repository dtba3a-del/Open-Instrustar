// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QtGui/QColor>

#include <QCoreApplication>
#include <QPointF>

#include "ctpu.h"
#include "hantekdso/controlspecification.h"
#include "hantekdso/enums.h"
#include "hantekprotocol/definitions.h"
#include "viewconstants.h"
#include <array>
#include <vector>


/// \brief Holds the cursor parameters
struct DsoSettingsScopeCursor {
    enum CursorShape { NONE, HORIZONTAL, VERTICAL, RECTANGULAR } shape = NONE;
    QPointF pos[ 2 ] = { { -1.0, -1.0 }, { 1.0, 1.0 } }; ///< Position in div
};

/// \brief Holds the settings for the horizontal axis.
struct DsoSettingsScopeHorizontal {
    Dso::GraphFormat format = Dso::GraphFormat::TY; ///< Graph drawing mode of the scope
    double frequencybase = 1e3;                     ///< Frequencybase in Hz/div
    DsoSettingsScopeCursor cursor;

    int recordLength = 0;   ///< Sample count
    double timebase = 1e-3; ///< Timebase in s/div
    double maxTimebase = 1; ///< Allow very slow timebases 0.1 ... 10.0 s/div
#ifdef Q_PROCESSOR_ARM
    // RPi: Not more often than every 10 ms
    double acquireInterval = 0.010; ///< Minimal time between captured frames
#else
    // other PC: Not more often than every 1 ms
    double acquireInterval = 0.001; ///< Minimal time between captured frames
#endif
    double samplerate = 1e6; ///< The samplerate of the oscilloscope in S
    int dotsOnScreen = 0;
    double calfreq = 1e3; ///< The frequency of the calibration output
    bool xyContinuous = false; ///< Continuous XY chart-recorder mode enabled
    /// У3 — TimeChannel: чувствительность оси времени, с/дел (детенты из
    /// TimeChannel::divisionSteps(), ряд 1-2-5 от нс/дел до ч/дел).
    double xyTimeSecPerDiv = 1.0;
    /// \name XY chart-recorder configuration (У1/У2/У8, ENGINEERING_LOG §17)
    /// Persisted so a session restart keeps the recorder set up; the docks
    /// write through on every change, HorizontalDock::buildXYConfig() maps
    /// these into XYRecorder::Config when a recording is armed.
    ///@{
    bool xyUseBinTape = true;      ///< true = BinTape (streaming bins), false = CIC cascade (legacy)
    unsigned xyBinCount = 2000;    ///< bins on the tape, from BinTape::standardBinCounts()
    unsigned xyMedianWindow = 0;
    /// Поле XY (настоящая XY-запись): ячеек по каждой оси НЕЗАВИСИМО —
    /// одно общее число дало бы только квадратную сетку (замечание
    /// пользователя 2026-08-21).
    unsigned xyFieldBinsX = 512;
    unsigned xyFieldBinsY = 512;   ///< median prefilter: 0/1 = off; odd; ≤15 (C9: residual peak, W≥31 clips peaks)
    unsigned xyMasterAxis = 0;     ///< cascade model: 0 = X, 1 = Y sizes the decimation
    unsigned xySheetMode = 0;      ///< 0 = finite sheet (RAM), 1 = tape (streamed to disk)
    double xySlewRateX = 1.0;      ///< cascade model: expected X slew rate [V/s]
    double xySlewRateY = 1.0;      ///< cascade model: expected Y slew rate [V/s]
    unsigned xyTargetPoints = 2000;   ///< cascade model, finite sheet: points per sweep
    double xyTargetDensity = 2000.0;  ///< cascade model, tape: points per second
    bool xyTrackSigma = false;        ///< cascade model: store per-point std-dev
    unsigned xyExtractMode = 0;       ///< cascade model: 0 = mean, 1 = peak envelope
    ///@}
};

/// \brief Holds the settings for the trigger.
/// TODO Use ControlSettingsTrigger
struct DsoSettingsScopeTrigger {
    Dso::TriggerMode mode = Dso::TriggerMode::AUTO; ///< Automatic, normal or single trigger
    double position = 0.5;                          ///< Horizontal position for pretrigger (middle of screen)
    Dso::Slope slope = Dso::Slope::Positive;        ///< Rising or falling edge causes trigger
    int source = 0;                                 ///< Channel that is used as trigger source
    int smooth = 0;                                 ///< Don't trigger on glitches
};

/// \brief Base for DsoSettingsScopeSpectrum and DsoSettingsScopeVoltage
struct DsoSettingsScopeChannel {
    QString name;         ///< Name of this channel
    bool used = false;    ///< true if the channel is used (either visible or input for math etc.)
    bool visible = false; ///< true if the channel is turned on
    DsoSettingsScopeCursor cursor;
};

/// \brief Holds the settings for the spectrum analysis.
struct DsoSettingsScopeSpectrum : public DsoSettingsScopeChannel {
    double offset = 0.0;     ///< Vertical offset in divs
    double magnitude = 20.0; ///< The vertical resolution in dB/div
};

/// \brief Holds the settings for the power and frequency analysis.
struct DsoSettingsScopeAnalysis {
    double spectrumReference = 0.0; ///< Reference level for spectrum in dBV
    bool calculateDummyLoad = false;
    unsigned dummyLoad = 50; ///< Dummy load in  Ohms
    QString dBsuffixStrings[ 3 ] = { QCoreApplication::translate( "DsoSettingsScopeAnalysis", "V" ),
                                     QCoreApplication::translate( "DsoSettingsScopeAnalysis", "u" ),
                                     QCoreApplication::translate( "DsoSettingsScopeAnalysis", "m" ) };
    int dBsuffixIndex = 0; // dBV is default
    QString dBsuffix() {   // use current index
        return dBsuffixStrings[ dBsuffixIndex ];
    };
    QString dBsuffix( int index ) {
        if ( index >= 0 && index < 3 )       // valid suffix index
            return dBsuffixStrings[ index ]; // show this value
        else
            return QString();
    };
    bool calculateTHD = false;
    bool showNoteValue = false;
};

/// \brief Holds the settings for the normal voltage graphs.
/// TODO Use ControlSettingsVoltage
struct DsoSettingsScopeVoltage : public DsoSettingsScopeChannel {
    double offset = 0.0;              ///< Vertical offset in divs
    double trigger = 0.0;             ///< Trigger level in physical units (V if CtPU OFF)
    /// \brief Автоматический уровень триггера по гистограмме — СВОЙ у канала.
    /// Задание 9 очереди: «автотриггер должен быть у каждого канала свой».
    /// Разбор приёма и его границы — `autotrigger.h`.
    bool triggerAuto = false;
    unsigned gainStepIndex = 6;       ///< The vertical resolution in V/div (default = 1.0)
    unsigned couplingOrMathIndex = 0; ///< Different index: coupling for real- and mode for math-channels
    bool inverted = false;            ///< true if the channel is inverted (mirrored on cross-axis)
    double probeAttn = 1.0;           ///< attenuation of probe
    /// У4а — РАЗРЯДНОСТЬ: окно скользящего среднего N (1 = выкл).
    /// Даёт +0.5·log₂(N) эффективных бит (закон Tektronix HiRes,
    /// movingaverage.h) ценой полосы f₋₃dB ≈ 0.443·SR/N.
    /// НЕ меняет величину: среднее сохраняет уровень, поэтому ни отображение,
    /// ни сетка В/дел от этого параметра не зависят.
    /// Только степени двойки: 1,2,4,…,64.
    unsigned resolutionN = 1;

    /// У4б — ЭКРАННОЕ УВЕЛИЧЕНИЕ: во сколько раз растянута трасса по вертикали
    /// (1.0 = выкл). Входные уровни не меняются; увеличено только изображение,
    /// поэтому сетка В/дел перенормируется в 1/screenZoom раз.
    /// Разделено с разрядностью по распоряжению автора 2026-08-29
    /// (`docs/PROTOTYPE-QUEUE.md`, задание 7): прежде обе функции сидели в
    /// одном параметре, и порог триггера расходился с трассой ровно в √N раз.
    double screenZoom = 1.0;

    // -- CtPU (Conversion to Physical Units) — TZ §3 -------------------------
    CtPU::Mode ctpuMode = CtPU::Mode::OFF; ///< OFF (volts) | FORMULA (P=kV+b) | CCTPU (calibrated)
    QString ctpuUnit = "V";               ///< User-defined unit string (1–8 chars UTF-8)
    double ctpuK = 1.0;                   ///< Sensitivity [physical/V]
    double ctpuB = 0.0;                   ///< Offset [physical]
    // -- CCtPU calibration points (TZ §4) ------------------------------------
    double ccptuZeroV = 0.0;          ///< Voltage at physical zero (V)
    double ccptuSpanV = 1.0;          ///< Voltage at physical span (V)
    double ccptuSpanPhysical = 1.0;   ///< Physical value at span
};

/// \brief Configuration of a single math-stack virtual channel. TZ §5.
/// Each math channel computes `sign × (srcA op srcB)` and may have its own
/// CtPU formula applied afterwards (CCtPU is NOT available for math channels).
struct MathChannelConfig {
    uint8_t srcA = 0;                ///< Unified index of operand A (< spec->channels + mathIndex)
    uint8_t srcB = 1;                ///< Unified index of operand B (< spec->channels + mathIndex)
    Dso::MathOp op = Dso::MathOp::ADD; ///< ADD | SUB | MUL | DIV
    bool invert = false;             ///< Multiply result by −1
    bool enabled = false;            ///< If false, this slot produces no output
    QString ctpuUnit = "V";          ///< Physical unit for this math channel (manual formula only)
    double ctpuK = 1.0;              ///< CtPU sensitivity [physical/V] (FORMULA only)
    double ctpuB = 0.0;              ///< CtPU offset [physical]
};

/// \brief Configuration of one XY multi-curve trajectory. TZ §7.
/// Up to `DsoSettingsScope::maxXYCurves` curves share a common grid but may
/// map any (xChannel, yChannel) pair onto the screen.
struct XYCurveConfig {
    uint8_t xChannel = 0;            ///< Unified channel index for the X axis
    uint8_t yChannel = 1;            ///< Unified channel index for the Y axis
    bool enabled = false;            ///< If false, this curve is not drawn
    /// \brief Явно назначенный цвет кривой. Недействительный = по каналу Y.
    ///
    /// Задание 4 очереди прототипа: прежде цвет брался ТОЛЬКО от канала Y,
    /// поэтому две кривые с общим Y выходили одинаковыми, причина была не
    /// видна, а способа изменить не существовало. Умолчание раздаётся
    /// неповторяющимся (`defaultCurveColor`), явное назначение перекрывает.
    QColor explicitColor;
};

/// \brief Цвет кривой XY по умолчанию — различный для каждой из кривых.
///
/// Умолчания разнесены по кругу оттенков, чтобы соседние кривые не совпадали:
/// совпавший цвет прячет одну кривую под другой и молча вводит в заблуждение.
inline QColor defaultCurveColor( int curveIndex ) {
    static const int hue[] = { 200, 40, 320, 100 }; // синий, оранжевый, малиновый, зелёный
    const int n = int( sizeof( hue ) / sizeof( hue[ 0 ] ) );
    return QColor::fromHsv( hue[ ( curveIndex % n + n ) % n ], 220, 245 );
}

/// \brief Holds the settings for the oscilloscope.
struct DsoSettingsScope {
    /// Maximum number of math-stack virtual channels (TZ §5.2.1).
    static constexpr int maxMathChannels = 4;
    /// Maximum number of XY multi-curve trajectories (TZ §7.1.2).
    static constexpr int maxXYCurves = 4;
    /// У3 — TimeChannel: специальный индекс «канала времени» в осях XY-кривой.
    /// Время — полноценный канал (timechannel.h): T-Y — частный случай XY,
    /// где одна ось — время. Значение вне диапазона voltage.size(), чтобы
    /// существующие проверки `ch < voltage.size()` естественно отсекали его
    /// от вольтовых путей.
    static constexpr uint8_t timeChannelIndex = 0xF0;

    std::vector< double > gainSteps = { ///< The selectable voltage gain steps in V/div
        2e-2, 5e-2, 1e-1, 2e-1, 5e-1, 1e0, 2e0, 5e0, 1e1 };
    std::vector< double > mathGainSteps = { ///< The selectable voltage math gain steps in V/div
        2e-2, 5e-2, 1e-1, 2e-1, 5e-1, 1e0, 2e0, 5e0, 1e1, 2e1, 5e1, 1e2, 2e2, 5e2, 1e3 };
    std::vector< DsoSettingsScopeSpectrum > spectrum; ///< Spectrum analysis settings
    std::vector< DsoSettingsScopeVoltage > voltage;   ///< Settings for the normal graphs
    DsoSettingsScopeHorizontal horizontal;            ///< Settings for the horizontal axis
    DsoSettingsScopeTrigger trigger;                  ///< Settings for the trigger
    DsoSettingsScopeAnalysis analysis;                ///< Settings for the analysis

    /// Per-math-channel configuration (size = maxMathChannels). TZ §5.
    std::vector< MathChannelConfig > mathStack;
    /// Per-XY-curve configuration (size = maxXYCurves). TZ §7.
    std::vector< XYCurveConfig > xyCurves;

    int verboseLevel = 0;
    int toolTipVisible = 1; // show hints for beginners, can be disabled in settings dialog
    bool doNotTranslate = false;
    bool histogram = false;
    bool hasACcoupling = false;
    bool hasACmodification = false;
    bool liveCalibrationActive = false;

    double gain( unsigned channel ) const {
        // The last `maxMathChannels` entries of `voltage` are math channels.
        // All entries before that are real hardware channels using `gainSteps`;
        // math entries use the wider `mathGainSteps` table.
        if ( channel < voltage.size() - unsigned( maxMathChannels ) ) // Voltage channel
            return gainSteps[ voltage[ channel ].gainStepIndex ] * voltage[ channel ].probeAttn;
        else
            return mathGainSteps[ voltage[ channel ].gainStepIndex ] * voltage[ channel ].probeAttn;
    }

    /// \brief Effective vertical gain in physical units per div.
    /// Combines the V/div gain with the CtPU sensitivity k. When CtPU is OFF
    /// (k == 1.0), this is identical to gain(). TZ §6.1.1.
    double physicalGain( unsigned channel ) const {
        return gain( channel ) * voltage[ channel ].ctpuK;
    }

    /// \brief Цена деления НА ЭКРАНЕ с учётом экранного увеличения (У4б).
    ///
    /// Единственное место пересчёта «деление экрана ↔ физическая величина».
    /// Всё, что переводит экранные координаты в величину и обратно — трасса,
    /// порог триггера, курсоры, подпись В/дел — обязано ходить сюда, иначе
    /// масштабы расходятся. Именно так и было: `graphgenerator` делил на zoom,
    /// а порог триггера — нет (`docs/PROTOTYPE-QUEUE.md`, задание 7).
    double displayGain( unsigned channel ) const {
        const double z = voltage[ channel ].screenZoom;
        return physicalGain( channel ) / ( z > 0.0 ? z : 1.0 );
    }

    /// \brief True if `channel` is a math-stack virtual channel (TZ §5).
    /// Real channels are indices [0, spec->channels); math channels are the
    /// remaining `maxMathChannels` slots in `voltage`.
    bool isMathChannel( unsigned channel, unsigned realChannelCount ) const {
        return channel >= realChannelCount && channel < voltage.size();
    }

    bool anyUsed( ChannelID channel ) const { return voltage[ channel ].used || spectrum[ channel ].used; }

    Dso::Coupling coupling( ChannelID channel, const Dso::ControlSpecification *deviceSpecification ) const {
        return deviceSpecification->couplings[ voltage[ channel ].couplingOrMathIndex ];
    }
    // Channels, including math channels
    ChannelID countChannels() const { return ChannelID( voltage.size() ); }

    double getMarker( int marker ) const {
        double x = qBound( MARGIN_LEFT, marker < 2 ? horizontal.cursor.pos[ marker ].x() : 0.0, MARGIN_RIGHT );
        return x;
    }

    void setMarker( unsigned int marker, double value ) {
        if ( marker < 2 )
            horizontal.cursor.pos[ marker ].setX( value );
    }
};
