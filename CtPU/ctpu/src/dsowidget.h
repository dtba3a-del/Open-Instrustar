// SPDX-License-Identifier: GPL-2.0-or-later
// Last edited: 2026-08-11 11:31:17 UTC

#pragma once

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QList>
#include <array>
#include <memory>

#include "cameralayer.h"
#include "glscope.h"
#include "hantekdso/controlspecification.h"
#include "levelslider.h"
#include "viewsettings.h"
#include "xyrecorder.h"

class SpectrumGenerator;
struct DsoSettingsScope;
struct DsoSettingsView;
class DataGrid;

/// \brief The widget for the oszilloscope-screen
/// This widget contains the scopes and all level sliders.
class DsoWidget : public QWidget {
    Q_OBJECT

  public:
    struct Sliders {
        LevelSlider *voltageOffsetSlider;   ///< The sliders for the graph offsets
        LevelSlider *triggerPositionSlider; ///< The slider for the pretrigger
        LevelSlider *triggerLevelSlider;    ///< The sliders for the trigger level
        LevelSlider *markerSlider;          ///< The sliders for the markers
    };

    /// \brief Initializes the components of the oszilloscope-screen.
    /// \param settings The settings object containing the oscilloscope settings.
    /// \param dataAnalyzer The data analyzer that should be used as data source.
    /// \param parent The parent widget.
    /// \param flags Flags for the window manager.
    DsoWidget( DsoSettingsScope *scope, DsoSettingsView *view, const Dso::ControlSpecification *spec, QWidget *parent = nullptr );

    ~DsoWidget() override;

    // Data arrived
    /// \brief Data-bearing part of frame consumption: feeds every enabled
    /// XY recorder (BinTape decimation). MUST be called once per frame —
    /// unlike the display, recorded data may never be displaced (D-05,
    /// REALTIME-FEEL.md). Cheap: decimation only, no widgets touched.
    void ingestNew( std::shared_ptr< PPresult > analysedData );
    /// \brief Display part: scopes, labels, measurements. Safe to coalesce —
    /// MainWindow keeps only the newest unrendered frame (mailbox-1) and
    /// calls this once per GUI-queue drain. Does NOT feed recorders.
    void showNew( std::shared_ptr< PPresult > analysedData );

    void switchToPrintColors();
    void restoreScreenColors();

    /// \brief Access the XY recorder for export (curve 0, legacy).
    XYRecorder *xyRecorder() { return &m_xyRecorder; }
    /// \brief Access the XY recorder for curve `i` (TZ §7.4 multi-curve).
    /// Curve 0 returns the legacy m_xyRecorder slot; curves 1..3 return
    /// entries from m_xyRecorders[]. Returns nullptr if i is out of range.
    XYRecorder *xyRecorder( int i ) {
        if ( i == 0 )
            return &m_xyRecorder;
        if ( i > 0 && i < DsoSettingsScope::maxXYCurves )
            return &m_xyRecorders[ i ];
        return nullptr;
    }
    /// \name Камера: слой холста и два снимка
    ///@{
    /// \brief Слой камеры — окну настройки нужен он сам, а не его копия.
    CameraLayer *camera() { return cameraLayer; }
    /// \brief Почему кадра нет НА ХОЛСТЕ, даже когда камера отдаёт кадры.
    /// Пустая строка — слой холста построен. Отказ шейдера слоя не мешает ни
    /// сборке, ни работе камеры, поэтому он обязан быть назван словами.
    QString cameraLayerError() const;

    /// \brief Привести слой камеры к текущим настройкам вида.
    ///
    /// Вызывается окном настройки после смены прибора, включённости или
    /// прозрачности. Камера открывается ТОЛЬКО при явно выбранном приборе.
    void applyCameraSettings();

    /// \brief Чистый кадр камеры без сетки и кривых. Пусто — кадров не было.
    QImage cameraPhoto() const;

    /// \brief Холст целиком, как он на экране: фон, кадр камеры, сетка, кривые.
    ///
    /// Второй из двух вариантов сохранения, названных автором: «чистое фото и
    /// эта область, где под сеткой находится слой изображения камеры».
    QImage canvasWithCamera();
    ///@}

    /// \brief Access the scope settings (for menu handlers / export).
    DsoSettingsScope *settingsScope() { return scope; }

  protected:
    virtual void showEvent( QShowEvent *event ) override;
    void setupSliders( Sliders &sliders );
    void adaptTriggerLevelSlider( DsoWidget::Sliders &sliders, ChannelID channel );
    void adaptTriggerPositionSlider();
    void setMeasurementVisible( ChannelID channel );
    void updateMarkerDetails();
    void updateSpectrumDetails( ChannelID channel );
    void updateTriggerDetails();
    void updateVoltageDetails( ChannelID channel );

    double mainToZoom( double position ) const;
    double zoomToMain( double position ) const;

    Sliders mainSliders;
    Sliders zoomSliders;

    QGridLayout *mainLayout; ///< The main layout for this widget

    QHBoxLayout *settingsLayout;        ///< The table for the settings info
    QLabel *settingsTriggerLabel;       ///< The trigger details
    QLabel *settingsSamplesOnScreen;    ///< The displayed dots on screen
    QLabel *settingsSamplerateLabel;    ///< The samplerate
    QLabel *settingsOversampleLabel;    ///< The oversample factor
    QLabel *settingsTimebaseLabel;      ///< The timebase of the main scope
    QLabel *settingsFrequencybaseLabel; ///< The frequencybase of the main scope

    QLabel *swTriggerStatus; ///< The status of SW trigger
    QLabel *frameAgeLabel;   ///< Age of the displayed frame in ms (D-05 HUD metric)

    QHBoxLayout *markerLayout;        ///< The table for the marker details
    QLabel *markerInfoLabel;          ///< The info about the zoom factor
    QLabel *markerTimeLabel;          ///< The time period between the markers
    QLabel *markerFrequencyLabel;     ///< The frequency for the time period
    QLabel *markerTimebaseLabel;      ///< The timebase for the zoomed scope
    QLabel *markerFrequencybaseLabel; ///< The frequencybase for the zoomed scope

    QGridLayout *measurementLayout;                    ///< The table for the signal details
    std::vector< QLabel * > measurementNameLabel;      ///< The name of the channel
    std::vector< QLabel * > measurementGainLabel;      ///< The gain for the voltage (V/div)
    std::vector< QLabel * > measurementMagnitudeLabel; ///< The magnitude for the spectrum (dB/div)
    std::vector< QLabel * > measurementMiscLabel;      ///< Coupling or math mode
    std::vector< QLabel * > measurementVppLabel;       ///< Peak-to-peak amplitude of the signal (V)
    std::vector< QLabel * > measurementRMSLabel;       ///< RMS Amplitude of the signal (V) = sqrt( DC² + AC² )
    std::vector< QLabel * > measurementDCLabel;        ///< DC Amplitude of the signal (V)
    std::vector< QLabel * > measurementACLabel;        ///< AC Amplitude of the signal (V)
    std::vector< QLabel * > measurementdBLabel;        ///< AC Amplitude in dB
    std::vector< QLabel * > measurementFrequencyLabel; ///< Frequency of the signal (Hz)
    std::vector< QLabel * > measurementNoteLabel;      ///< Note value of the signal
    std::vector< QLabel * > measurementRMSPowerLabel;  ///< RMS Power in Watts
    std::vector< QLabel * > measurementTHDLabel;       ///< THD of the signal in Watts

    DataGrid *cursorDataGrid = nullptr;

    DsoSettingsScope *scope;
    DsoSettingsView *view;
    const Dso::ControlSpecification *spec;

    GlScope *mainScope; ///< The main scope screen
    /// Задание 3 очереди: источник кадров для самого заднего слоя холста.
    /// Живёт здесь, потому что холст здесь; измерения по кадру тут нет.
    CameraLayer *cameraLayer = nullptr;
    GlScope *zoomScope; ///< The optional magnified scope screen

  private:
    double samplerate;
    unsigned oversample = 1;
    double timebase;
    double pulseWidth1 = 0.0;
    double pulseWidth2 = 0.0;
    double zoomFactor = 1.0;
    int mainScopeRow = 0;
    int zoomScopeRow = 0;
    void setColors();
    // [MOD] TZ §9.1.1 (Variant B) — `voltageUnits` and `physicalUnitStrings`
    // removed entirely. They were caches that had to be kept perfectly in
    // sync with `scope->voltage.size()` by hand; the empty-cache crash fixed
    // earlier was exactly that invariant breaking once. Per TZ, the target
    // architecture reads the unit at the point of use instead of caching it.
    //
    // `voltageUnits` (legacy `Unit` enum) turned out to be write-only dead
    // code across the whole file — every assignment to voltageUnits[...] was
    // followed by nothing that ever read it back for display (confirmed by
    // the `Unit voltageUnit = voltageUnits[channel]` compiler warning:
    // "variable set but not used"). It is not replaced by anything.
    //
    // `physicalUnitStrings` (per-channel QString) is replaced by two
    // equivalent direct reads, both always in sync by construction:
    //   - Inside showNew(), where a `const DataChannel *data` for the
    //     current channel is already in hand, use `data->physicalUnit`
    //     directly — PostProcessing::convertData() already derived it from
    //     `scope->voltage[channel].ctpuUnit` / `scope->mathStack[i].ctpuUnit`
    //     for that exact frame, so no extra cache is needed.
    //   - Everywhere else (no live frame data available — e.g. trigger
    //     details, gain label, marker/cursor deltas), use unitFor(channel)
    //     below, which reads the same source of truth from settings.
    /// \brief Physical unit string for a unified channel index (TZ §3.5.3,
    /// §9.1.1). Real channels (channel < spec->channels) store their unit
    /// directly on `scope->voltage[channel].ctpuUnit`. Math channels
    /// (channel >= spec->channels) do NOT — the Math config page writes the
    /// unit to `scope->mathStack[mathIndex].ctpuUnit` only; `scope->voltage[
    /// mathChannel].ctpuUnit` is never updated by the UI and stays at its
    /// "V" default. Using `scope->voltage[channel].ctpuUnit` unconditionally
    /// for every channel (a literal reading of the TZ snippet) would
    /// therefore silently show "V" for every math channel's gain/cursor/
    /// trigger labels regardless of its configured unit — this resolves the
    /// correct source for both cases.
    QString unitFor( ChannelID channel ) const;
    bool cursorMeasurementValid = false;
    QPoint cursorGlobalPosition = QPoint();
    QPointF cursorMeasurementPosition = QPointF();
    ChannelID selectedCursor = 0;
    void switchToMarker();
    void showCursorMessage( QPoint globalPos = QPoint(), const QString &message = QString() );
    void updateItem( ChannelID index, bool switchOn = false );

    XYRecorder m_xyRecorder; ///< Continuous XY recorder buffer (legacy, curve 0)
    /// \brief Multi-curve XY recorders (TZ §7.4).
    /// One recorder per curve slot; each is independently configured via
    /// `scope->xyCurves[i]`. Curve 0 reuses `m_xyRecorder` for backward
    /// compatibility with code that reads `xyRecorder()`.
    std::array< XYRecorder, DsoSettingsScope::maxXYCurves > m_xyRecorders;

  public slots:
    // Horizontal axis
    // void horizontalFormatChanged(HorizontalFormat format);
    void updateFrequencybase( double frequencybase );
    void updateSamplerate( double samplerate );
    void updateOversample( unsigned oversample );
    void updateTimebase( double timebase );

    // Trigger
    void updateTriggerMode();
    void updateTriggerSlope();
    void updateTriggerSource();

    // Spectrum
    void updateSpectrumMagnitude( ChannelID channel );
    void updateSpectrumUsed( ChannelID channel, bool used );

    // Vertical axis
    void updateVoltageCoupling( ChannelID channel );
    void updateMathMode();
    void updateVoltageGain( ChannelID channel );
    void updateVoltageUsed( ChannelID channel, bool used );

    // Menus
    void updateRecordLength( int size );

    // Scope control
    void updateZoom( bool enabled );
    void updateCursorGrid( bool enabled );
    void wheelEvent( QWheelEvent *event ) override;

    // Scope control
    void updateSlidersSettings();

    // XY Recorder
    void updateXYContinuous( bool enabled );
    void configureXYRecorder( XYRecorder::Config cfg );

  private slots:
    // Sliders
    void updateOffset( ChannelID channel, double value, bool pressed, QPoint globalPos );
    void updateTriggerPosition( int index, double value, bool pressed, QPoint globalPos, bool mainView = true );
    void updateTriggerLevel( ChannelID channel, double value, bool pressed, QPoint globalPos );
    void updateMarker( unsigned marker, double value );

  signals:
    // Sliders
    void voltageOffsetChanged( ChannelID channel, double value ); ///< A graph offset has been changed
    void triggerPositionChanged( double value );                  ///< The pretrigger has been changed
    void triggerLevelChanged( ChannelID channel, double value );  ///< A trigger level has been changed
};
