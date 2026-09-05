// SPDX-License-Identifier: GPL-2.0-or-later

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDebug>
#include <QDockWidget>
#include <QFileDialog>
#include <QLabel>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QThread>

#include <cmath>

#include "HorizontalDock.h"
#include "timechannel.h"
#include "dockwindows.h"

#include "scopesettings.h"
#include "sispinbox.h"
#include "utils/printutils.h"

static int row = 0;

template < typename... Args > struct SELECT {
    template < typename C, typename R > static constexpr auto OVERLOAD_OF( R ( C::*pmf )( Args... ) ) -> decltype( pmf ) {
        return pmf;
    }
};

HorizontalDock::HorizontalDock( DsoSettingsScope *scope, const Dso::ControlSpecification *spec, QWidget *parent )
    : QDockWidget( tr( "Horizontal" ), parent ), scope( scope ) {

    if ( scope->verboseLevel > 1 )
        qDebug() << " HorizontalDock::HorizontalDock()";

    // Initialize elements
    samplerateLabel = new QLabel( tr( "Samplerate" ) );
    samplerateSiSpinBox = new SiSpinBox( UNIT_SAMPLES );
    if ( scope->toolTipVisible )
        samplerateSiSpinBox->setToolTip( tr( "Effective samplerate, automatically selected from 'Timebase' setting" ) );
    samplerateSiSpinBox->setMinimum( 1 );
    samplerateSiSpinBox->setMaximum( 1e8 );
    samplerateSiSpinBox->setUnitPostfix( tr( "/s" ) );

    timebaseSteps << 1.0 << 2.0 << 5.0 << 10.0;

    timebaseLabel = new QLabel( tr( "Timebase" ) );
    timebaseSiSpinBox = new SiSpinBox( UNIT_SECONDS );
    if ( scope->toolTipVisible )
        timebaseSiSpinBox->setToolTip( tr( "Time per horizontal screen division" ) );
    timebaseSiSpinBox->setSteps( timebaseSteps );
    timebaseSiSpinBox->setMinimum( 1e-9 );
    timebaseSiSpinBox->setMaximum( 1e3 );

    formatLabel = new QLabel( tr( "Format" ) );
    formatComboBox = new QComboBox();
    if ( scope->toolTipVisible )
        formatComboBox->setToolTip( tr( "Select signal over time or XY display" ) );
    for ( Dso::GraphFormat format : Dso::GraphFormatEnum )
        formatComboBox->addItem( Dso::graphFormatString( format ) );

    // XY Continuous recorder checkbox
    // Честное имя органа (замечание пользователя 2026-08-21): галка включает
    // НЕПРЕРЫВНУЮ ЗАПИСЬ, а её лента даёт усреднение по времени. Настоящая
    // XY-запись (поле плотности по значениям осей) ведётся одновременно и
    // экспортируется отдельным файлом — см. Export XY Data.
    xyContinuousCheckBox = new QCheckBox( tr( "Continuous recording" ) );
    if ( scope->toolTipVisible )
        xyContinuousCheckBox->setToolTip( tr( "Continuous acquisition into two records at once:\n"
                                               "  • XY field — value bins on both axes (the XY record proper,\n"
                                               "    keeps the waveform shape, resolution set per axis below);\n"
                                               "  • tape — long-time average along the record order (BinTape/CIC),\n"
                                               "    good for slow signals, shape not recoverable from it." ) );

    // XY recorder configuration. У1/У2 (ENGINEERING_LOG §17): every visible
    // control must be live — the decimation model is an explicit choice, and
    // each model shows only the fields it actually reads.
    decimationModelLabel = new QLabel( tr( "Decimation model" ) );
    decimationModelComboBox = new QComboBox();
    decimationModelComboBox->addItem( tr( "BinTape (streaming bins)" ) );
    decimationModelComboBox->addItem( tr( "CIC cascade (legacy)" ) );
    if ( scope->toolTipVisible )
        decimationModelComboBox->setToolTip( tr( "BinTape: fixed number of bins, neighbours merge pairwise on "
                                                  "overflow, nothing is discarded.\n"
                                                  "CIC cascade: box-car mean before every decimation stage "
                                                  "(previous model, kept for comparison)." ) );

    // Разрешение поля XY — ДВА независимых числа (замечание пользователя
    // 2026-08-21): одно общее число на плоскость дало бы только квадратную
    // сетку, а оси физически разные — у них свои пределы и своя динамика.
    fieldBinsXLabel = new QLabel( tr( "XY field bins, X" ) );
    fieldBinsXSpinBox = new QSpinBox();
    fieldBinsXSpinBox->setRange( 2, 8192 );
    fieldBinsXSpinBox->setValue( 512 );
    fieldBinsYLabel = new QLabel( tr( "XY field bins, Y" ) );
    fieldBinsYSpinBox = new QSpinBox();
    fieldBinsYSpinBox->setRange( 2, 8192 );
    fieldBinsYSpinBox->setValue( 512 );
    if ( scope->toolTipVisible ) {
        const QString tip = tr( "Resolution of the XY field along this axis. The two axes are independent:\n"
                                "one common number could only ever give a square grid." );
        fieldBinsXSpinBox->setToolTip( tip );
        fieldBinsYSpinBox->setToolTip( tip );
    }

    binCountLabel = new QLabel( tr( "Bins on tape" ) );
    binCountComboBox = new QComboBox();
    for ( std::size_t n : BinTape::standardBinCounts() )
        binCountComboBox->addItem( QString::number( qulonglong( n ) ), qulonglong( n ) );
    binCountComboBox->setCurrentIndex( binCountComboBox->findData( qulonglong( 2000 ) ) );
    if ( scope->toolTipVisible )
        binCountComboBox->setToolTip( tr( "Sets the tape length in bins — the only parameter that defines "
                                           "the point count in the BinTape model. Fixed choices: the count "
                                           "must halve cleanly when neighbouring bins merge." ) );

    medianWindowLabel = new QLabel( tr( "Median prefilter" ) );
    medianWindowComboBox = new QComboBox();
    medianWindowComboBox->addItem( tr( "Off" ), 0u );
    for ( unsigned w = 3; w <= 15; w += 2 )
        medianWindowComboBox->addItem( QString::number( w ), w );
    if ( scope->toolTipVisible )
        medianWindowComboBox->setToolTip( tr( "Median window before binning: removes switching spikes and "
                                               "ringing so bin min/max reflect the real signal span.\n"
                                               "Capped at 15: measured on test data, windows ≥31 clip signal "
                                               "peaks while the residual energy share stays near zero — "
                                               "control by residual peak, not energy." ) );

    xyTimeDivLabel = new QLabel( tr( "Time axis" ) );
    xyTimeDivComboBox = new QComboBox();
    for ( double sec : TimeChannel::divisionSteps() )
        xyTimeDivComboBox->addItem( QString::fromStdString( TimeChannel::divisionLabel( sec ) ) + tr( "/div" ), sec );
    if ( scope->toolTipVisible )
        xyTimeDivComboBox->setToolTip( tr( "Sensitivity of the Time axis for XY curves that use Time as X or Y "
                                            "(1-2-5 detents, ns/div .. h/div). T-Y is just an XY curve whose "
                                            "X axis is Time." ) );

    masterAxisLabel = new QLabel( tr( "Master axis" ) );
    masterAxisComboBox = new QComboBox();
    masterAxisComboBox->addItem( tr( "X (CH1)" ) );
    masterAxisComboBox->addItem( tr( "Y (CH2)" ) );
    if ( scope->toolTipVisible )
        masterAxisComboBox->setToolTip( tr( "Axis whose full-scale range and slew rate size the decimation "
                                             "cascade (like choosing a trigger source, but for the recorder)" ) );

    sheetModeLabel = new QLabel( tr( "Sheet mode" ) );
    sheetModeComboBox = new QComboBox();
    sheetModeComboBox->addItem( tr( "Finite sheet (fixed point count)" ) );
    sheetModeComboBox->addItem( tr( "Tape (fixed density, streamed to disk)" ) );

    slewRateXLabel = new QLabel( tr( "Expected X slew rate" ) );
    slewRateXSiSpinBox = new SiSpinBox( UNIT_VOLTS );
    slewRateXSiSpinBox->setUnitPostfix( tr( "/s" ) );
    slewRateXSiSpinBox->setMinimum( 1e-4 );
    slewRateXSiSpinBox->setMaximum( 1e7 );
    slewRateXSiSpinBox->setValue( 1.0 );

    slewRateYLabel = new QLabel( tr( "Expected Y slew rate" ) );
    slewRateYSiSpinBox = new SiSpinBox( UNIT_VOLTS );
    slewRateYSiSpinBox->setUnitPostfix( tr( "/s" ) );
    slewRateYSiSpinBox->setMinimum( 1e-4 );
    slewRateYSiSpinBox->setMaximum( 1e7 );
    slewRateYSiSpinBox->setValue( 1.0 );

    targetPointsLabel = new QLabel( tr( "Target points" ) );
    targetPointsSpinBox = new QSpinBox();
    targetPointsSpinBox->setRange( 10, 1000000 );
    targetPointsSpinBox->setValue( 2000 );

    targetDensityLabel = new QLabel( tr( "Target density (pts/s)" ) );
    targetDensitySpinBox = new QSpinBox();
    targetDensitySpinBox->setRange( 1, 1000000 );
    targetDensitySpinBox->setValue( 2000 );

    trackSigmaCheckBox = new QCheckBox( tr( "Track sigma (envelope width)" ) );
    if ( scope->toolTipVisible )
        trackSigmaCheckBox->setToolTip( tr( "Store the std-dev of the final cascade block with each point, "
                                             "for drawing an uncertainty band" ) );

    extractModeLabel = new QLabel( tr( "Extraction" ) );
    extractModeComboBox = new QComboBox();
    extractModeComboBox->addItem( tr( "Cascade mean" ) );
    extractModeComboBox->addItem( tr( "Peak envelope (min/max)" ) );

    calfreqLabel = new QLabel( tr( "Calibration out" ) );
    calfreqSteps = spec->calfreqSteps;
    std::reverse( calfreqSteps.begin(), calfreqSteps.end() ); // put highest value on top of the list
    calfreqComboBox = new QComboBox();
    if ( scope->toolTipVisible )
        calfreqComboBox->setToolTip( tr( "Select the frequency of the calibration output, scroll for fast change" ) );
    for ( double calfreqStep : std::as_const( calfreqSteps ) )
        calfreqComboBox->addItem( valueToString( calfreqStep, UNIT_HERTZ, calfreqStep < 10e3 ? 2 : 0 ) );

    dockLayout = new QGridLayout();
    dockLayout->setColumnMinimumWidth( 0, 64 );
    dockLayout->setColumnStretch( 1, 1 );
    dockLayout->setSpacing( DOCK_LAYOUT_SPACING );

    row = 0; // allows flexible shift up/down
    dockLayout->addWidget( timebaseLabel, row, 0 );
    dockLayout->addWidget( timebaseSiSpinBox, row++, 1 );
    dockLayout->addWidget( samplerateLabel, row, 0 );
    dockLayout->addWidget( samplerateSiSpinBox, row++, 1 );
    dockLayout->addWidget( formatLabel, row, 0 );
    dockLayout->addWidget( formatComboBox, row++, 1 );
    dockLayout->addWidget( xyContinuousCheckBox, row, 0, 1, 2 );
    ++row;
    dockLayout->addWidget( decimationModelLabel, row, 0 );
    dockLayout->addWidget( decimationModelComboBox, row++, 1 );
    dockLayout->addWidget( fieldBinsXLabel, row, 0 );
    dockLayout->addWidget( fieldBinsXSpinBox, row++, 1 );
    dockLayout->addWidget( fieldBinsYLabel, row, 0 );
    dockLayout->addWidget( fieldBinsYSpinBox, row++, 1 );
    dockLayout->addWidget( binCountLabel, row, 0 );
    dockLayout->addWidget( binCountComboBox, row++, 1 );
    dockLayout->addWidget( medianWindowLabel, row, 0 );
    dockLayout->addWidget( medianWindowComboBox, row++, 1 );
    dockLayout->addWidget( xyTimeDivLabel, row, 0 );
    dockLayout->addWidget( xyTimeDivComboBox, row++, 1 );
    dockLayout->addWidget( masterAxisLabel, row, 0 );
    dockLayout->addWidget( masterAxisComboBox, row++, 1 );
    dockLayout->addWidget( sheetModeLabel, row, 0 );
    dockLayout->addWidget( sheetModeComboBox, row++, 1 );
    dockLayout->addWidget( slewRateXLabel, row, 0 );
    dockLayout->addWidget( slewRateXSiSpinBox, row++, 1 );
    dockLayout->addWidget( slewRateYLabel, row, 0 );
    dockLayout->addWidget( slewRateYSiSpinBox, row++, 1 );
    dockLayout->addWidget( targetPointsLabel, row, 0 );
    dockLayout->addWidget( targetPointsSpinBox, row++, 1 );
    dockLayout->addWidget( targetDensityLabel, row, 0 );
    dockLayout->addWidget( targetDensitySpinBox, row++, 1 );
    dockLayout->addWidget( trackSigmaCheckBox, row, 0, 1, 2 );
    ++row;
    dockLayout->addWidget( extractModeLabel, row, 0 );
    dockLayout->addWidget( extractModeComboBox, row++, 1 );
    dockLayout->addWidget( calfreqLabel, row, 0 );
    dockLayout->addWidget( calfreqComboBox, row++, 1 );

    dockWidget = new QWidget();
    SetupDockWidget( this, dockWidget, dockLayout );

    // Load settings into GUI
    loadSettings( scope );
    updateXYControlsVisibility();

    // Connect signals and slots
    connect( samplerateSiSpinBox, SELECT< double >::OVERLOAD_OF( &QDoubleSpinBox::valueChanged ), this,
             [ this ]( double samplerate ) { this->samplerateSelected( samplerate ); } );
    connect( timebaseSiSpinBox, SELECT< double >::OVERLOAD_OF( &QDoubleSpinBox::valueChanged ), this,
             [ this ]( double timebase ) { this->timebaseSelected( timebase ); } );
    connect( formatComboBox, SELECT< int >::OVERLOAD_OF( &QComboBox::currentIndexChanged ), this,
             [ this ]( int index ) { this->formatSelected( index ); } );
    connect( calfreqComboBox, SELECT< int >::OVERLOAD_OF( &QComboBox::currentIndexChanged ), this,
             [ this ]( int index ) { this->calfreqIndexSelected( index ); } );
    connect( xyContinuousCheckBox, &QCheckBox::toggled, this, &HorizontalDock::xyContinuousToggled );
    connect( sheetModeComboBox, SELECT< int >::OVERLOAD_OF( &QComboBox::currentIndexChanged ), this, [ this ]( int index ) {
        this->scope->horizontal.xySheetMode = unsigned( index );
        this->updateXYControlsVisibility();
    } );
    // У1/У8 — write-through: every recorder control persists its value into
    // scope->horizontal the moment it changes, so a session restart restores
    // the panel and buildXYConfig() always reflects what the operator sees.
    connect( decimationModelComboBox, SELECT< int >::OVERLOAD_OF( &QComboBox::currentIndexChanged ), this, [ this ]( int index ) {
        this->scope->horizontal.xyUseBinTape = ( index == 0 );
        this->updateXYControlsVisibility();
    } );
    connect( fieldBinsXSpinBox, SELECT< int >::OVERLOAD_OF( &QSpinBox::valueChanged ), this,
             [ this ]( int v ) { this->scope->horizontal.xyFieldBinsX = unsigned( v ); } );
    connect( fieldBinsYSpinBox, SELECT< int >::OVERLOAD_OF( &QSpinBox::valueChanged ), this,
             [ this ]( int v ) { this->scope->horizontal.xyFieldBinsY = unsigned( v ); } );
    connect( binCountComboBox, SELECT< int >::OVERLOAD_OF( &QComboBox::currentIndexChanged ), this, [ this ]( int ) {
        this->scope->horizontal.xyBinCount = unsigned( binCountComboBox->currentData().toUInt() );
    } );
    connect( medianWindowComboBox, SELECT< int >::OVERLOAD_OF( &QComboBox::currentIndexChanged ), this, [ this ]( int ) {
        this->scope->horizontal.xyMedianWindow = unsigned( medianWindowComboBox->currentData().toUInt() );
    } );
    connect( xyTimeDivComboBox, SELECT< int >::OVERLOAD_OF( &QComboBox::currentIndexChanged ), this, [ this ]( int ) {
        this->scope->horizontal.xyTimeSecPerDiv = xyTimeDivComboBox->currentData().toDouble();
    } );
    connect( masterAxisComboBox, SELECT< int >::OVERLOAD_OF( &QComboBox::currentIndexChanged ), this,
             [ this ]( int index ) { this->scope->horizontal.xyMasterAxis = unsigned( index ); } );
    connect( slewRateXSiSpinBox, SELECT< double >::OVERLOAD_OF( &QDoubleSpinBox::valueChanged ), this,
             [ this ]( double v ) { this->scope->horizontal.xySlewRateX = v; } );
    connect( slewRateYSiSpinBox, SELECT< double >::OVERLOAD_OF( &QDoubleSpinBox::valueChanged ), this,
             [ this ]( double v ) { this->scope->horizontal.xySlewRateY = v; } );
    connect( targetPointsSpinBox, SELECT< int >::OVERLOAD_OF( &QSpinBox::valueChanged ), this,
             [ this ]( int v ) { this->scope->horizontal.xyTargetPoints = unsigned( v ); } );
    connect( targetDensitySpinBox, SELECT< int >::OVERLOAD_OF( &QSpinBox::valueChanged ), this,
             [ this ]( int v ) { this->scope->horizontal.xyTargetDensity = double( v ); } );
    connect( trackSigmaCheckBox, &QCheckBox::toggled, this,
             [ this ]( bool checked ) { this->scope->horizontal.xyTrackSigma = checked; } );
    connect( extractModeComboBox, SELECT< int >::OVERLOAD_OF( &QComboBox::currentIndexChanged ), this,
             [ this ]( int index ) { this->scope->horizontal.xyExtractMode = unsigned( index ); } );
}


void HorizontalDock::loadSettings( DsoSettingsScope *scope ) {
    // Set values
    setSamplerate( scope->horizontal.samplerate );
    setTimebase( scope->horizontal.timebase );
    setFormat( scope->horizontal.format );
    setCalfreq( scope->horizontal.calfreq );
    QSignalBlocker blocker( xyContinuousCheckBox );
    xyContinuousCheckBox->setChecked( scope->horizontal.xyContinuous );
    xyContinuousCheckBox->setEnabled( scope->horizontal.format == Dso::GraphFormat::XY );
    // У8 — restore the recorder panel from persisted settings. Blockers keep
    // the write-through lambdas from re-writing the same values while we set
    // the widgets programmatically.
    {
        const auto &h = scope->horizontal;
        QSignalBlocker b1( decimationModelComboBox ), b2( binCountComboBox ), b3( medianWindowComboBox );
        QSignalBlocker b4( masterAxisComboBox ), b5( sheetModeComboBox ), b6( slewRateXSiSpinBox );
        QSignalBlocker b7( slewRateYSiSpinBox ), b8( targetPointsSpinBox ), b9( targetDensitySpinBox );
        QSignalBlocker b10( trackSigmaCheckBox ), b11( extractModeComboBox );
        QSignalBlocker b12( fieldBinsXSpinBox ), b13( fieldBinsYSpinBox );
        decimationModelComboBox->setCurrentIndex( h.xyUseBinTape ? 0 : 1 );
        fieldBinsXSpinBox->setValue( int( h.xyFieldBinsX ) );
        fieldBinsYSpinBox->setValue( int( h.xyFieldBinsY ) );
        int binIdx = binCountComboBox->findData( qulonglong( h.xyBinCount ) );
        if ( binIdx >= 0 )
            binCountComboBox->setCurrentIndex( binIdx );
        int medIdx = medianWindowComboBox->findData( h.xyMedianWindow );
        medianWindowComboBox->setCurrentIndex( medIdx >= 0 ? medIdx : 0 );
        {
            QSignalBlocker bt( xyTimeDivComboBox );
            int tIdx = xyTimeDivComboBox->findData( h.xyTimeSecPerDiv );
            if ( tIdx < 0 ) // непопадание в ряд: ближайший детент не меньше значения
                tIdx = xyTimeDivComboBox->findData( TimeChannel::snapDivision( h.xyTimeSecPerDiv ) );
            xyTimeDivComboBox->setCurrentIndex( tIdx >= 0 ? tIdx : xyTimeDivComboBox->findData( 1.0 ) );
        }
        masterAxisComboBox->setCurrentIndex( h.xyMasterAxis ? 1 : 0 );
        sheetModeComboBox->setCurrentIndex( h.xySheetMode ? 1 : 0 );
        slewRateXSiSpinBox->setValue( h.xySlewRateX );
        slewRateYSiSpinBox->setValue( h.xySlewRateY );
        targetPointsSpinBox->setValue( int( h.xyTargetPoints ) );
        targetDensitySpinBox->setValue( int( h.xyTargetDensity ) );
        trackSigmaCheckBox->setChecked( h.xyTrackSigma );
        extractModeComboBox->setCurrentIndex( h.xyExtractMode ? 1 : 0 );
    }
}


void HorizontalDock::triggerModeChanged( Dso::TriggerMode mode ) {
    if ( mode == Dso::TriggerMode::ROLL )
        timebaseSiSpinBox->setMinimum( 0.2 );
    else
        timebaseSiSpinBox->setMinimum( 1e-9 );
}


/// \brief Don't close the dock, just hide it.
/// \param event The close event that should be handled.
void HorizontalDock::closeEvent( QCloseEvent *event ) {
    hide();
    event->accept();
}


double HorizontalDock::setSamplerate( double samplerate ) {
    if ( scope->verboseLevel > 2 )
        qDebug() << "  HDock::setSamplerate()" << samplerate;
    if ( scope->verboseLevel > 3 )
        qDebug() << "   ThreadID:" << QThread::currentThreadId();
    samplerateRequest = samplerate;
    QSignalBlocker blocker( timebaseSiSpinBox );
    timebaseSiSpinBox->setMaximum( scope->horizontal.maxTimebase );
    blocker = QSignalBlocker( samplerateSiSpinBox );
    samplerateSiSpinBox->setValue( samplerate );
    return samplerateSiSpinBox->value();
}


double HorizontalDock::setTimebase( double timebase ) {
    QSignalBlocker blocker( timebaseSiSpinBox );
    // timebaseSteps are repeated in each decade
    double decade = pow( 10, floor( log10( timebase ) ) );
    double vNorm = timebase / decade;
    for ( int i = 0; i < timebaseSteps.size() - 1; ++i ) {
        if ( timebaseSteps.at( i ) <= vNorm && vNorm < timebaseSteps.at( i + 1 ) ) {
            timebaseSiSpinBox->setValue( decade * timebaseSteps.at( i ) );
            break;
        }
    }
    calculateSamplerateSteps( timebase );
    if ( scope->verboseLevel > 2 )
        qDebug() << "  HDock::setTimebase()" << timebase << "return" << timebaseSiSpinBox->value();
    return timebaseSiSpinBox->value();
}


int HorizontalDock::setFormat( Dso::GraphFormat format ) {
    if ( scope->verboseLevel > 2 )
        qDebug() << "  HDock::setFormat()" << format;
    QSignalBlocker blocker( formatComboBox );
    if ( format >= Dso::GraphFormat::TY && format <= Dso::GraphFormat::XY ) {
        formatComboBox->setCurrentIndex( format );
        xyContinuousCheckBox->setEnabled( format == Dso::GraphFormat::XY );
        updateXYControlsVisibility();
        return format;
    }
    return -1;
}


double HorizontalDock::setCalfreq( double calfreq ) {
    if ( scope->verboseLevel > 2 )
        qDebug() << "  HDock::setCalfreq()" << calfreq;
    auto indexIt = std::find( calfreqSteps.begin(), calfreqSteps.end(), calfreq );
    if ( indexIt == calfreqSteps.end() )
        return -1;
    int index = int( std::distance( calfreqSteps.begin(), indexIt ) );
    QSignalBlocker blocker( calfreqComboBox );
    calfreqComboBox->setCurrentIndex( index );
    return calfreq;
}


void HorizontalDock::setSamplerateLimits( double minimum, double maximum ) {
    if ( scope->verboseLevel > 2 )
        qDebug() << "  HDock::setSamplerateLimits()" << minimum << maximum;
    QSignalBlocker blocker( samplerateSiSpinBox );
    if ( bool( minimum ) )
        samplerateSiSpinBox->setMinimum( minimum );
    if ( bool( maximum ) )
        samplerateSiSpinBox->setMaximum( maximum );
}


void HorizontalDock::setSamplerateSteps( int mode, const QList< double > steps ) {
    if ( samplerateSteps.size() == steps.size() ) // no action needed
        return;
    if ( scope->verboseLevel > 3 )
        qDebug() << "   HDock::setSamplerateSteps()" << steps;
    else if ( scope->verboseLevel > 2 )
        qDebug() << "  HDock::setSamplerateSteps()" << steps.first() << "..." << steps.last();
    samplerateSteps = steps;
    // Assume that method is invoked for fixed samplerate devices only
    QSignalBlocker samplerateBlocker( samplerateSiSpinBox );
    samplerateSiSpinBox->setMode( mode );
    samplerateSiSpinBox->setSteps( steps );
    samplerateSiSpinBox->setMinimum( steps.first() );
    samplerateSiSpinBox->setMaximum( steps.last() );
    // Make reasonable adjustments to the timebase spinbox
    QSignalBlocker timebaseBlocker( timebaseSiSpinBox );
    timebaseSiSpinBox->setMinimum( pow( 10, floor( log10( 1.0 / steps.last() ) ) ) );
    calculateSamplerateSteps( timebaseSiSpinBox->value() );
}


/// \brief Called when the samplerate spinbox changes its value.
/// \param samplerate The samplerate in samples/second.
void HorizontalDock::samplerateSelected( double samplerate ) {
    if ( scope->verboseLevel > 2 )
        qDebug() << "  HDock::samplerateSelected()" << samplerate;
    scope->horizontal.samplerate = samplerate;
    emit samplerateChanged( samplerate );
}


/// \brief Called when the timebase spinbox changes its value.
/// \param timebase The timebase in seconds.
void HorizontalDock::timebaseSelected( double timebase ) {
    if ( scope->verboseLevel > 2 )
        qDebug() << "  HDock::timebaseSelected()" << timebase;
    if ( scope->verboseLevel > 3 )
        qDebug() << "   ThreadID:" << QThread::currentThreadId();
    scope->horizontal.timebase = timebase;
    calculateSamplerateSteps( timebase );
    emit timebaseChanged( timebase );
}


void HorizontalDock::calculateSamplerateSteps( double timebase ) {
    int size = samplerateSteps.size();
    if ( size ) {
        // search appropriate min & max sample rate
        double min = samplerateSteps[ 0 ];
        double max = samplerateSteps[ 0 ];
        for ( int id = 0; id < size; ++id ) {
            double sRate = samplerateSteps[ id ];
            if ( scope->verboseLevel > 3 )
                qDebug() << "   sRate, sRate*timebase" << sRate << sRate * timebase;
            // min must be < maxRate
            // find minimal samplerate to get at least this number of samples per div
            if ( id < size - 1 && sRate * timebase <= 10 ) { // 10 samples/div
                min = sRate;
            }
            // max must be > minRate
            // find max samplesrate to get not more then this number of samples per div
            // number should be <= 1000 to get enough samples for two full screens (to ensure triggering)
            if ( id && sRate * timebase <= 1000 ) { // 1000 samples/div
                max = sRate;
            }
        }
        min = qMax( min, qMin( 10e3, max ) ); // not less than 10kS unless max is smaller
        if ( scope->verboseLevel > 2 )
            qDebug() << "  HDock::calculateSamplerateSteps()" << timebase << min << max;
        setSamplerateLimits( min, max );
        // update samplerate if the requested value was limited
        if ( samplerateRequest > samplerateSiSpinBox->value() )
            setSamplerate( samplerateRequest );
    }
}


/// \brief Called when the format combo box changes its value.
/// \param index The index of the combo box item.
void HorizontalDock::formatSelected( int index ) {
    if ( scope->verboseLevel > 2 )
        qDebug() << "  HDock::formatSelected()" << index;
    scope->horizontal.format = Dso::GraphFormat( index );
    xyContinuousCheckBox->setEnabled( scope->horizontal.format == Dso::GraphFormat::XY );
    // Leaving XY while the recorder is still checked would otherwise leave
    // it "armed" with no way to fire updateXYContinuous(false) again ->
    // stops mid-recording cleanup (label restore, finalize()/clear()) from
    // ever running. Stop it explicitly instead.
    if ( scope->horizontal.format != Dso::GraphFormat::XY && xyContinuousCheckBox->isChecked() )
        xyContinuousCheckBox->setChecked( false ); // synchronously triggers xyContinuousToggled(false)
    updateXYControlsVisibility();
    emit formatChanged( scope->horizontal.format );
}


/// \brief Called when the calfreq combobox changes its value.
/// \param index The item index.
void HorizontalDock::calfreqIndexSelected( int index ) {
    double calfreq = calfreqSteps[ index ];
    if ( scope->verboseLevel > 2 )
        qDebug() << "  HDock::calfreqIndex Selected()" << index << calfreq;
    scope->horizontal.calfreq = calfreq;
    emit calfreqChanged( calfreq );
}


XYRecorder::Config HorizontalDock::buildXYConfig() const {
    XYRecorder::Config cfg;
    // У1 (ENGINEERING_LOG §17): the model choice and the BinTape parameters
    // come from live controls now — no field on the panel is silently unread.
    cfg.useBinTape = decimationModelComboBox->currentIndex() == 0;
    cfg.binCount = std::size_t( binCountComboBox->currentData().toUInt() );
    cfg.medianWindow = unsigned( medianWindowComboBox->currentData().toUInt() );
    cfg.buildField = true;
    cfg.fieldBinsX = std::size_t( fieldBinsXSpinBox->value() );
    cfg.fieldBinsY = std::size_t( fieldBinsYSpinBox->value() );
    cfg.fieldAutoRange = true; // границы по данным; полная шкала — кандидат в опции
    cfg.masterAxis = masterAxisComboBox->currentIndex() == 0 ? XYRecorder::MasterAxis::X : XYRecorder::MasterAxis::Y;
    cfg.sheetMode =
        sheetModeComboBox->currentIndex() == 0 ? XYRecorder::SheetMode::FINITE : XYRecorder::SheetMode::TAPE;
    cfg.slewRateX = slewRateXSiSpinBox->value();
    cfg.slewRateY = slewRateYSiSpinBox->value();
    cfg.targetPoints = std::size_t( targetPointsSpinBox->value() );
    cfg.targetDensity = double( targetDensitySpinBox->value() );
    cfg.trackSigma = trackSigmaCheckBox->isChecked();
    cfg.extractMode = extractModeComboBox->currentIndex() == 0 ? XYRecorder::ExtractMode::CASCADE
                                                                : XYRecorder::ExtractMode::PEAK_ENVELOPE;
    return cfg;
}


void HorizontalDock::updateXYControlsVisibility() {
    const bool xyMode = ( scope->horizontal.format == Dso::GraphFormat::XY );
    const bool recording = xyContinuousCheckBox->isChecked();
    // У1: fields follow the selected decimation model — a control is visible
    // only if the active model actually reads it (no dead controls on the
    // panel, ENGINEERING_LOG §17).
    const bool binTape = decimationModelComboBox->currentIndex() == 0;
    const bool cascade = !binTape;

    decimationModelLabel->setVisible( xyMode );
    decimationModelComboBox->setVisible( xyMode );
    fieldBinsXLabel->setVisible( xyMode );   // поле XY ведётся при любой модели ленты
    fieldBinsXSpinBox->setVisible( xyMode );
    fieldBinsYLabel->setVisible( xyMode );
    fieldBinsYSpinBox->setVisible( xyMode );
    binCountLabel->setVisible( xyMode && binTape );
    binCountComboBox->setVisible( xyMode && binTape );
    medianWindowLabel->setVisible( xyMode && binTape );
    medianWindowComboBox->setVisible( xyMode && binTape );
    xyTimeDivLabel->setVisible( xyMode );
    xyTimeDivComboBox->setVisible( xyMode );

    masterAxisLabel->setVisible( xyMode && cascade );
    masterAxisComboBox->setVisible( xyMode && cascade );
    sheetModeLabel->setVisible( xyMode ); // both models: TAPE opens the stream file
    sheetModeComboBox->setVisible( xyMode );
    slewRateXLabel->setVisible( xyMode && cascade );
    slewRateXSiSpinBox->setVisible( xyMode && cascade );
    slewRateYLabel->setVisible( xyMode && cascade );
    slewRateYSiSpinBox->setVisible( xyMode && cascade );
    trackSigmaCheckBox->setVisible( xyMode && cascade );
    extractModeLabel->setVisible( xyMode && cascade );
    extractModeComboBox->setVisible( xyMode && cascade );

    const bool finite = sheetModeComboBox->currentIndex() == 0;
    targetPointsLabel->setVisible( xyMode && cascade && finite );
    targetPointsSpinBox->setVisible( xyMode && cascade && finite );
    targetDensityLabel->setVisible( xyMode && cascade && !finite );
    targetDensitySpinBox->setVisible( xyMode && cascade && !finite );

    // configure() isn't safe mid-recording -> lock the inputs while armed
    decimationModelComboBox->setEnabled( !recording );
    fieldBinsXSpinBox->setEnabled( !recording );
    fieldBinsYSpinBox->setEnabled( !recording );
    binCountComboBox->setEnabled( !recording );
    medianWindowComboBox->setEnabled( !recording );
    xyTimeDivComboBox->setEnabled( true ); // чувствительность оси — отрисовка, менять безопасно и во время записи
    masterAxisComboBox->setEnabled( !recording );
    sheetModeComboBox->setEnabled( !recording );
    slewRateXSiSpinBox->setEnabled( !recording );
    slewRateYSiSpinBox->setEnabled( !recording );
    targetPointsSpinBox->setEnabled( !recording );
    targetDensitySpinBox->setEnabled( !recording );
    trackSigmaCheckBox->setEnabled( !recording );
    extractModeComboBox->setEnabled( !recording );
}


/// \brief Called when XY Recorder checkbox is toggled.
/// \param checked The new state.
void HorizontalDock::xyContinuousToggled( bool checked ) {
    if ( scope->verboseLevel > 2 )
        qDebug() << "  HDock::xyContinuousToggled()" << checked;

    if ( checked ) {
        XYRecorder::Config cfg = buildXYConfig();
        if ( cfg.sheetMode == XYRecorder::SheetMode::TAPE ) {
            const QString path = QFileDialog::getSaveFileName( this, tr( "XY tape recording target" ),
                                                                 lastTapeFilePath, tr( "CSV files (*.csv)" ) );
            if ( path.isEmpty() ) {
                // User cancelled -> abort the start, leave everything as it was
                QSignalBlocker blocker( xyContinuousCheckBox );
                xyContinuousCheckBox->setChecked( false );
                return;
            }
            lastTapeFilePath = path;
            cfg.tapeFilePath = path;
        }
        emit xyConfigureRequested( cfg ); // must reach DsoWidget before the next addFrame()
    }

    scope->horizontal.xyContinuous = checked;
    updateXYControlsVisibility();
    emit xyContinuousChanged( checked );
}
