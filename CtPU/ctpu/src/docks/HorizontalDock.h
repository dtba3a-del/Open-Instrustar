// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QDockWidget>
#include <QGridLayout>

#include <vector>

#include "hantekdso/controlspecification.h"
#include "hantekdso/enums.h"
#include "xyrecorder.h"

class QLabel;
class QCheckBox;
class QComboBox;
class QSpinBox;

class SiSpinBox;

struct DsoSettingsScope;
// struct ControlSpecification;

Q_DECLARE_METATYPE( std::vector< unsigned > )
Q_DECLARE_METATYPE( std::vector< double > )

/// \brief Dock window for the horizontal axis.
/// It contains the settings for the timebase and the display format.
class HorizontalDock : public QDockWidget {
    Q_OBJECT

  public:
    /// \brief Initializes the horizontal axis docking window.
    /// \param settings The target settings object.
    /// \param parent The parent widget.
    /// \param flags Flags for the window manager.
    HorizontalDock( DsoSettingsScope *scope, const Dso::ControlSpecification *spec, QWidget *parent );

    /// \brief Changes the samplerate.
    /// \param samplerate The samplerate in seconds.
    double setSamplerate( double samplerate );
    /// \brief Changes the timebase.
    /// \param timebase The timebase in seconds.
    double setTimebase( double timebase );
    /// \brief Changes the record length if the new value is supported.
    /// \param recordLength The record length in samples.
    void setRecordLength( int recordLength );
    /// \brief Changes the format if the new value is supported.
    /// \param format The format for the horizontal axis.
    /// \return Index of format-value, -1 on error.
    int setFormat( Dso::GraphFormat format );
    /// \brief Updates the minimum and maximum of the samplerate spin box.
    /// \param minimum The minimum value the spin box should accept.
    /// \param maximum The minimum value the spin box should accept.
    void setSamplerateLimits( double minimum, double maximum );
    /// \brief Updates the mode and steps of the samplerate spin box.
    /// \param steps The steps value the spin box should accept.
    void setSamplerateSteps( int mode, const QList< double > steps );
    void calculateSamplerateSteps( double timebase );
    /// \brief Changes the calibration frequency.
    /// \param calfreq The calibration frequency in hertz.
    double setCalfreq( double calfreq );

  public slots:
    /// \brief Loads settings into GUI
    /// \param scope Settings to load
    void loadSettings( DsoSettingsScope *scope );
    void triggerModeChanged( Dso::TriggerMode mode );

  protected:
    void closeEvent( QCloseEvent *event ) override;
    QGridLayout *dockLayout;        ///< The main layout for the dock window
    QWidget *dockWidget;            ///< The main widget for the dock window
    QLabel *samplerateLabel;        ///< The label for the samplerate spinbox
    QLabel *timebaseLabel;          ///< The label for the timebase spinbox
    QLabel *formatLabel;            ///< The label for the format combobox
    QLabel *calfreqLabel;           ///< The label for the calibration frequency spinbox
    QCheckBox *xyContinuousCheckBox; ///< Enable continuous XY recorder mode
    SiSpinBox *samplerateSiSpinBox; ///< Selects the samplerate for acquisitions
    SiSpinBox *timebaseSiSpinBox;   ///< Selects the timebase for voltage graphs
    QComboBox *formatComboBox;      ///< Selects the way the sampled data is
                                    ///  interpreted and shown
    QComboBox *calfreqComboBox;     ///< Selects the calibration frequency

    // XY recorder configuration (visible only in XY format, like a
    // trigger-source/sensitivity pair: master axis picks which axis sizes
    // the cascade, both slew rates are always independently editable)
    QLabel *decimationModelLabel;    ///< У1: decimation model is an explicit, live control
    QComboBox *decimationModelComboBox; ///< BinTape (streaming bins) / CIC cascade
    QLabel *fieldBinsXLabel;         ///< Поле XY: ячеек по оси X (независимо от Y)
    QSpinBox *fieldBinsXSpinBox;
    QLabel *fieldBinsYLabel;         ///< Поле XY: ячеек по оси Y
    QSpinBox *fieldBinsYSpinBox;
    QLabel *binCountLabel;           ///< У2: bins on the tape (BinTape model)
    QComboBox *binCountComboBox;     ///< values from BinTape::standardBinCounts()
    QLabel *medianWindowLabel;       ///< У2: median prefilter window (BinTape model)
    QComboBox *medianWindowComboBox; ///< Off,3,5,...,15 — odd, capped by C9 (residual peak)
    QLabel *xyTimeDivLabel;          ///< У3: time-axis sensitivity for Time-axis XY curves
    QComboBox *xyTimeDivComboBox;    ///< detents from TimeChannel::divisionSteps() (1-2-5, ns..h)
    QLabel *masterAxisLabel;
    QComboBox *masterAxisComboBox;
    QLabel *sheetModeLabel;
    QComboBox *sheetModeComboBox;
    QLabel *slewRateXLabel;
    SiSpinBox *slewRateXSiSpinBox;
    QLabel *slewRateYLabel;
    SiSpinBox *slewRateYSiSpinBox;
    QLabel *targetPointsLabel;
    QSpinBox *targetPointsSpinBox;
    QLabel *targetDensityLabel;
    QSpinBox *targetDensitySpinBox;
    QCheckBox *trackSigmaCheckBox;
    QLabel *extractModeLabel;
    QComboBox *extractModeComboBox;
    QString lastTapeFilePath;

    DsoSettingsScope *scope;         ///< The settings provided by the parent class
    QList< double > timebaseSteps;   ///< Steps for the timebase spinbox
    QList< double > calfreqSteps;    ///< Steps for the calfreq spinbox
    QList< double > samplerateSteps; ///< Possible sampe rates

    QStringList formatStrings; ///< Strings for the formats

    void updateXYControlsVisibility();
    XYRecorder::Config buildXYConfig() const;

  protected slots:
    void samplerateSelected( double samplerate );
    void timebaseSelected( double timebase );
    void formatSelected( int index );
    void calfreqIndexSelected( int index );
    void xyContinuousToggled( bool checked );

  private:
    double samplerateRequest = 0;

  signals:
    void samplerateChanged( double samplerate );   ///< The samplerate has been changed
    void timebaseChanged( double timebase );       ///< The timebase has been changed
    void recordLengthChanged( int recordLength );  ///< The recordd length has been changed
    void formatChanged( Dso::GraphFormat format ); ///< The viewing format has been changed
    void calfreqChanged( double calfreq );         ///< The timebase has been changed
    void xyContinuousChanged( bool enabled );      ///< Continuous XY recorder mode toggled
    void xyConfigureRequested( XYRecorder::Config cfg ); ///< Emitted right before xyContinuousChanged(true) - must be handled synchronously by DsoWidget before the next addFrame()
};
