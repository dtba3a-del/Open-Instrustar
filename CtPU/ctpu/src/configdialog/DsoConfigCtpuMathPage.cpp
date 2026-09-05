// SPDX-License-Identifier: GPL-2.0-or-later
// Last edited: 2026-08-11 11:31:17 UTC

#include "DsoConfigCtpuMathPage.h"

#include <QColorDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QScrollArea>
#include <QHBoxLayout>
#include <QVBoxLayout>

#include "ctpu.h"
#include "dsosettings.h"
#include "post/ppresult.h"
#include "scopesettings.h"


DsoConfigCtpuMathPage::DsoConfigCtpuMathPage( DsoSettings *settings, QWidget *parent )
    : QWidget( parent ), settings( settings ) {
    // Правило GUI (распоряжение автора 2026-08-29): ОДИН ЗНАЧОК — ОДНО ОКНО,
    // БЕЗ ВКЛАДОК. Страница может выйти длинной — тогда её прокручивают
    // колесом, стрелками или PgUp/PgDn, но не прячут за корешками вкладок.
    // Вкладка скрывает существование раздела: пока на неё не нажали, её
    // содержимого как бы нет.
    QVBoxLayout *mainLayout = new QVBoxLayout( this );
    mainLayout->setContentsMargins( 0, 0, 0, 0 );

    // Determine real channel count from the device spec.
    const unsigned realChannels = unsigned( settings->scope.voltage.size() - DsoSettingsScope::maxMathChannels );

    ctpuTab = new QWidget();
    mathTab = new QWidget();
    xyTab = new QWidget();

    buildCtpuTab( realChannels );
    buildMathTab();
    buildXYTab();

    // Прежние вкладки становятся озаглавленными разделами одной страницы:
    // сохраняется группировка, исчезает прятанье.
    QWidget *page = new QWidget();
    QVBoxLayout *pageLayout = new QVBoxLayout( page );
    struct Section {
        QWidget *body;
        QString title;
    };
    for ( const Section &sec : { Section{ ctpuTab, tr( "CtPU" ) }, Section{ mathTab, tr( "Math" ) },
                                 Section{ xyTab, tr( "XY" ) } } ) {
        QGroupBox *box = new QGroupBox( sec.title, page );
        QVBoxLayout *boxLayout = new QVBoxLayout( box );
        boxLayout->setContentsMargins( 6, 6, 6, 6 );
        sec.body->setParent( box );
        boxLayout->addWidget( sec.body );
        pageLayout->addWidget( box );
    }
    pageLayout->addStretch( 1 );

    scrollArea = new QScrollArea( this );
    scrollArea->setWidget( page );
    scrollArea->setWidgetResizable( true );
    // Прокрутка колесом, стрелками и PgUp/PgDn — поведение QScrollArea по
    // умолчанию, но фокус нужен, иначе клавиши уйдут мимо.
    scrollArea->setFocusPolicy( Qt::StrongFocus );
    scrollArea->setFrameShape( QFrame::NoFrame );
    mainLayout->addWidget( scrollArea );
    setLayout( mainLayout );
}


void DsoConfigCtpuMathPage::buildCtpuTab( unsigned realChannelCount ) {
    QVBoxLayout *outer = new QVBoxLayout( ctpuTab );
    QGridLayout *grid = new QGridLayout();
    grid->setSpacing( 6 );

    // Header row
    grid->addWidget( new QLabel( tr( "Channel" ) ), 0, 0 );
    grid->addWidget( new QLabel( tr( "Mode" ) ), 0, 1 );
    grid->addWidget( new QLabel( tr( "Unit" ) ), 0, 2 );
    grid->addWidget( new QLabel( tr( "k" ) ), 0, 3 );
    grid->addWidget( new QLabel( tr( "b" ) ), 0, 4 );
    grid->addWidget( new QLabel( tr( "Zero V" ) ), 0, 5 );
    grid->addWidget( new QLabel( tr( "Span V" ) ), 0, 6 );
    grid->addWidget( new QLabel( tr( "Span Phys" ) ), 0, 7 );
    grid->addWidget( new QLabel( tr( "Actions" ) ), 0, 8 );
    grid->addWidget( new QLabel( tr( "Live" ) ), 0, 9 );

    ctpuUI.clear();
    ctpuUI.resize( realChannelCount );
    for ( unsigned ch = 0; ch < realChannelCount; ++ch ) {
        const int row = int( ch ) + 1;
        auto &ui = ctpuUI[ ch ];

        grid->addWidget( new QLabel( settings->scope.voltage[ ch ].name ), row, 0 );

        ui.modeCombo = new QComboBox();
        ui.modeCombo->addItem( tr( "OFF" ), int( CtPU::Mode::OFF ) );
        ui.modeCombo->addItem( tr( "FORMULA" ), int( CtPU::Mode::FORMULA ) );
        ui.modeCombo->addItem( tr( "CCTPU" ), int( CtPU::Mode::CCTPU ) );
        grid->addWidget( ui.modeCombo, row, 1 );

        ui.unitEdit = new QLineEdit();
        ui.unitEdit->setMaxLength( 8 );
        ui.unitEdit->setPlaceholderText( QStringLiteral( "V" ) );
        ui.unitEdit->setToolTip( tr( "Enter the BASE unit only (e.g. V, A, kg, Pa) without an SI prefix "
                                     "-- the app adds m/µ/k/M automatically based on the displayed value. "
                                     "Typing an already-prefixed unit (e.g. 'mV') will show a doubled prefix ('mmV')." ) );
        grid->addWidget( ui.unitEdit, row, 2 );

        ui.kSpin = new QDoubleSpinBox();
        ui.kSpin->setRange( -1e9, 1e9 );
        ui.kSpin->setDecimals( 6 );
        ui.kSpin->setSingleStep( 0.001 );
        grid->addWidget( ui.kSpin, row, 3 );

        ui.bSpin = new QDoubleSpinBox();
        ui.bSpin->setRange( -1e9, 1e9 );
        ui.bSpin->setDecimals( 6 );
        ui.bSpin->setSingleStep( 0.001 );
        grid->addWidget( ui.bSpin, row, 4 );

        ui.zeroVSpin = new QDoubleSpinBox();
        ui.zeroVSpin->setRange( -1e3, 1e3 );
        ui.zeroVSpin->setDecimals( 6 );
        grid->addWidget( ui.zeroVSpin, row, 5 );

        ui.spanVSpin = new QDoubleSpinBox();
        ui.spanVSpin->setRange( -1e3, 1e3 );
        ui.spanVSpin->setDecimals( 6 );
        grid->addWidget( ui.spanVSpin, row, 6 );

        ui.spanPhysSpin = new QDoubleSpinBox();
        ui.spanPhysSpin->setRange( -1e9, 1e9 );
        ui.spanPhysSpin->setDecimals( 6 );
        grid->addWidget( ui.spanPhysSpin, row, 7 );

        ui.zeroButton = new QPushButton( tr( "Zero" ) );
        ui.spanButton = new QPushButton( tr( "Span" ) );
        ui.kLabel = new QLabel();
        ui.bLabel = new QLabel();
        QHBoxLayout *actionLayout = new QHBoxLayout();
        actionLayout->addWidget( ui.zeroButton );
        actionLayout->addWidget( ui.spanButton );
        actionLayout->addWidget( ui.kLabel );
        actionLayout->addWidget( ui.bLabel );
        QWidget *actionWidget = new QWidget();
        actionWidget->setLayout( actionLayout );
        grid->addWidget( actionWidget, row, 8 );

        // Wire Zero/Span buttons: they store the current spin values and
        // recompute (k, b) via CtPU::calculateFromCalibration().
        connect( ui.zeroButton, &QPushButton::clicked, this, [ this, ch ]() {
            if ( ch >= ctpuUI.size() )
                return;
            // For the stub we just take zeroVSpin's current value as V0.
            // A real implementation would average the live channel data here.
            double k = settings->scope.voltage[ ch ].ctpuK;
            double b = settings->scope.voltage[ ch ].ctpuB;
            const double zeroV = ctpuUI[ ch ].zeroVSpin->value();
            const double spanV = ctpuUI[ ch ].spanVSpin->value();
            const double spanP = ctpuUI[ ch ].spanPhysSpin->value();
            if ( CtPU::calculateFromCalibration( zeroV, spanV, spanP, k, b ) ) {
                ctpuUI[ ch ].kSpin->setValue( k );
                ctpuUI[ ch ].bSpin->setValue( b );
                updateCtpuLabels();
            }
        } );
        connect( ui.spanButton, &QPushButton::clicked, this, [ this, ch ]() {
            // Same as Zero — both buttons trigger (re)computation of k, b.
            if ( ch >= ctpuUI.size() )
                return;
            double k = settings->scope.voltage[ ch ].ctpuK;
            double b = settings->scope.voltage[ ch ].ctpuB;
            const double zeroV = ctpuUI[ ch ].zeroVSpin->value();
            const double spanV = ctpuUI[ ch ].spanVSpin->value();
            const double spanP = ctpuUI[ ch ].spanPhysSpin->value();
            if ( CtPU::calculateFromCalibration( zeroV, spanV, spanP, k, b ) ) {
                ctpuUI[ ch ].kSpin->setValue( k );
                ctpuUI[ ch ].bSpin->setValue( b );
                updateCtpuLabels();
            }
        } );

        // --- Live measurement source (CCtPU v1, discussed in chat) ---
        // Manual entry (typing a datasheet value straight into Zero V/Span V)
        // remains fully available and untouched above. This adds an
        // *alternative* live-capture path: pick which of the 12 measurements
        // to track, watch it update in real time, then copy it into Zero V
        // or Span V with one click instead of reading the main scope screen
        // and typing the number by hand. It intentionally does NOT touch k/b
        // itself — that still only happens when the existing Zero/Span
        // buttons above are pressed, so "get a number into the box" and
        // "compute from the two boxes" stay two separate, explicit steps.
        ui.sourceCombo = new QComboBox();
        ui.sourceCombo->addItem( tr( "DC (segment)" ), 0 );
        ui.sourceCombo->addItem( tr( "DC (one cycle)" ), 1 );
        ui.sourceCombo->addItem( tr( "Std Dev" ), 2 );
        ui.sourceCombo->addItem( tr( "Vmax (peak)" ), 3 );
        ui.sourceCombo->addItem( tr( "Vpp (peak-to-peak)" ), 4 );
        ui.sourceCombo->addItem( tr( "AC RMS" ), 5 );
        ui.sourceCombo->addItem( tr( "Level, dB" ), 6 );
        ui.sourceCombo->addItem( tr( "Top" ), 7 );
        ui.sourceCombo->addItem( tr( "Base" ), 8 );
        ui.sourceCombo->addItem( tr( "Amplitude (Top-Base)" ), 9 );
        ui.sourceCombo->addItem( tr( "Overshoot+" ), 10 );
        ui.sourceCombo->addItem( tr( "Overshoot-" ), 11 );

        ui.liveValueLabel = new QLabel( tr( "no data yet" ) );
        ui.liveValueLabel->setMinimumWidth( 90 );

        ui.captureZeroButton = new QPushButton( QStringLiteral( "\u2192 Zero V" ) ); // "→ Zero V"
        ui.captureSpanButton = new QPushButton( QStringLiteral( "\u2192 Span V" ) ); // "→ Span V"
        ui.captureZeroButton->setEnabled( false );
        ui.captureSpanButton->setEnabled( false );
        ui.captureZeroButton->setToolTip(
            tr( "Copy the current live value into Zero V (does not recompute k/b by itself)" ) );
        ui.captureSpanButton->setToolTip(
            tr( "Copy the current live value into Span V (does not recompute k/b by itself)" ) );

        QHBoxLayout *liveLayout = new QHBoxLayout();
        liveLayout->addWidget( ui.sourceCombo );
        liveLayout->addWidget( ui.liveValueLabel );
        liveLayout->addWidget( ui.captureZeroButton );
        liveLayout->addWidget( ui.captureSpanButton );
        QWidget *liveWidget = new QWidget();
        liveWidget->setLayout( liveLayout );
        grid->addWidget( liveWidget, row, 9 );

        connect( ui.captureZeroButton, &QPushButton::clicked, this, [ this, ch ]() {
            if ( ch >= ctpuUI.size() || !ctpuUI[ ch ].haveLiveValue )
                return;
            ctpuUI[ ch ].zeroVSpin->setValue( ctpuUI[ ch ].lastLiveValue );
        } );
        connect( ui.captureSpanButton, &QPushButton::clicked, this, [ this, ch ]() {
            if ( ch >= ctpuUI.size() || !ctpuUI[ ch ].haveLiveValue )
                return;
            ctpuUI[ ch ].spanVSpin->setValue( ctpuUI[ ch ].lastLiveValue );
        } );
    }
    outer->addLayout( grid );
    outer->addStretch();

    // Pre-fill UI from settings.
    for ( unsigned ch = 0; ch < realChannelCount && ch < settings->scope.voltage.size(); ++ch ) {
        const auto &v = settings->scope.voltage[ ch ];
        ctpuUI[ ch ].modeCombo->setCurrentIndex( int( v.ctpuMode ) );
        ctpuUI[ ch ].unitEdit->setText( v.ctpuUnit );
        ctpuUI[ ch ].kSpin->setValue( v.ctpuK );
        ctpuUI[ ch ].bSpin->setValue( v.ctpuB );
        ctpuUI[ ch ].zeroVSpin->setValue( v.ccptuZeroV );
        ctpuUI[ ch ].spanVSpin->setValue( v.ccptuSpanV );
        ctpuUI[ ch ].spanPhysSpin->setValue( v.ccptuSpanPhysical );
    }
    updateCtpuLabels();
}


void DsoConfigCtpuMathPage::buildMathTab() {
    QVBoxLayout *outer = new QVBoxLayout( mathTab );
    QGridLayout *grid = new QGridLayout();
    grid->setSpacing( 6 );

    // Header row
    grid->addWidget( new QLabel( tr( "Math" ) ), 0, 0 );
    grid->addWidget( new QLabel( tr( "Enabled" ) ), 0, 1 );
    grid->addWidget( new QLabel( tr( "srcA" ) ), 0, 2 );
    grid->addWidget( new QLabel( tr( "srcB" ) ), 0, 3 );
    grid->addWidget( new QLabel( tr( "Op" ) ), 0, 4 );
    grid->addWidget( new QLabel( tr( "Invert" ) ), 0, 5 );
    grid->addWidget( new QLabel( tr( "Unit" ) ), 0, 6 );
    grid->addWidget( new QLabel( tr( "k" ) ), 0, 7 );
    grid->addWidget( new QLabel( tr( "b" ) ), 0, 8 );

    mathUI.clear();
    mathUI.resize( DsoSettingsScope::maxMathChannels );
    for ( int i = 0; i < DsoSettingsScope::maxMathChannels; ++i ) {
        const int row = i + 1;
        auto &ui = mathUI[ i ];

        grid->addWidget( new QLabel( QStringLiteral( "M%1" ).arg( i + 1 ) ), row, 0 );

        ui.enabledCheck = new QCheckBox();
        grid->addWidget( ui.enabledCheck, row, 1 );

        ui.srcASpin = new QSpinBox();
        ui.srcASpin->setRange( 0, int( settings->scope.voltage.size() ) - 1 );
        grid->addWidget( ui.srcASpin, row, 2 );

        ui.srcBSpin = new QSpinBox();
        ui.srcBSpin->setRange( 0, int( settings->scope.voltage.size() ) - 1 );
        grid->addWidget( ui.srcBSpin, row, 3 );

        ui.opCombo = new QComboBox();
        ui.opCombo->addItem( QStringLiteral( "+" ), int( Dso::MathOp::ADD ) );
        ui.opCombo->addItem( QStringLiteral( "-" ), int( Dso::MathOp::SUB ) );
        ui.opCombo->addItem( QStringLiteral( "*" ), int( Dso::MathOp::MUL ) );
        ui.opCombo->addItem( QStringLiteral( "/" ), int( Dso::MathOp::DIV ) );
        grid->addWidget( ui.opCombo, row, 4 );

        ui.invertCheck = new QCheckBox();
        grid->addWidget( ui.invertCheck, row, 5 );

        ui.unitEdit = new QLineEdit();
        ui.unitEdit->setMaxLength( 8 );
        ui.unitEdit->setPlaceholderText( QStringLiteral( "V" ) );
        ui.unitEdit->setToolTip( tr( "Enter the BASE unit only (e.g. V, A, kg, Pa) without an SI prefix "
                                     "-- the app adds m/µ/k/M automatically based on the displayed value. "
                                     "Typing an already-prefixed unit (e.g. 'mV') will show a doubled prefix ('mmV')." ) );
        grid->addWidget( ui.unitEdit, row, 6 );

        ui.kSpin = new QDoubleSpinBox();
        ui.kSpin->setRange( -1e9, 1e9 );
        ui.kSpin->setDecimals( 6 );
        ui.kSpin->setSingleStep( 0.001 );
        grid->addWidget( ui.kSpin, row, 7 );

        ui.bSpin = new QDoubleSpinBox();
        ui.bSpin->setRange( -1e9, 1e9 );
        ui.bSpin->setDecimals( 6 );
        ui.bSpin->setSingleStep( 0.001 );
        grid->addWidget( ui.bSpin, row, 8 );
    }
    outer->addLayout( grid );
    outer->addStretch();

    // Pre-fill from settings.
    for ( int i = 0; i < DsoSettingsScope::maxMathChannels && i < int( settings->scope.mathStack.size() ); ++i ) {
        const auto &m = settings->scope.mathStack[ i ];
        mathUI[ i ].enabledCheck->setChecked( m.enabled );
        mathUI[ i ].srcASpin->setValue( m.srcA );
        mathUI[ i ].srcBSpin->setValue( m.srcB );
        mathUI[ i ].opCombo->setCurrentIndex( int( m.op ) );
        mathUI[ i ].invertCheck->setChecked( m.invert );
        mathUI[ i ].unitEdit->setText( m.ctpuUnit );
        mathUI[ i ].kSpin->setValue( m.ctpuK );
        mathUI[ i ].bSpin->setValue( m.ctpuB );
    }
}


void DsoConfigCtpuMathPage::buildXYTab() {
    QVBoxLayout *outer = new QVBoxLayout( xyTab );
    QGridLayout *grid = new QGridLayout();
    grid->setSpacing( 6 );

    grid->addWidget( new QLabel( tr( "Curve" ) ), 0, 0 );
    grid->addWidget( new QLabel( tr( "Enabled" ) ), 0, 1 );
    grid->addWidget( new QLabel( tr( "X channel" ) ), 0, 2 );
    grid->addWidget( new QLabel( tr( "Y channel" ) ), 0, 3 );
    grid->addWidget( new QLabel( tr( "Color" ) ), 0, 4 );
    grid->addWidget( new QLabel( tr( "Preview" ) ), 0, 5 );

    xyUI.clear();
    xyUI.resize( DsoSettingsScope::maxXYCurves );
    for ( int i = 0; i < DsoSettingsScope::maxXYCurves; ++i ) {
        const int row = i + 1;
        auto &ui = xyUI[ i ];

        grid->addWidget( new QLabel( QStringLiteral( "Curve %1" ).arg( i ) ), row, 0 );

        ui.enabledCheck = new QCheckBox();
        grid->addWidget( ui.enabledCheck, row, 1 );

        // У3: имена каналов вместо голых индексов + ось времени в общем
        // списке (T-Y — частный случай XY, timechannel.h).
        auto fillAxisCombo = [ this ]( QComboBox *combo ) {
            for ( std::size_t ch = 0; ch < settings->scope.voltage.size(); ++ch )
                combo->addItem( settings->scope.voltage[ ch ].name, uint( ch ) );
            combo->addItem( tr( "Time" ), uint( DsoSettingsScope::timeChannelIndex ) );
        };
        ui.xChannelCombo = new QComboBox();
        fillAxisCombo( ui.xChannelCombo );
        grid->addWidget( ui.xChannelCombo, row, 2 );

        ui.yChannelCombo = new QComboBox();
        fillAxisCombo( ui.yChannelCombo );
        grid->addWidget( ui.yChannelCombo, row, 3 );

        // Задание 4 очереди: явное назначение цвета. Прежде цвет брался
        // только от канала Y — две кривые с общим Y выходили одинаковыми,
        // причина была не видна, изменить было нечем.
        ui.colorButton = new QToolButton();
        ui.colorButton->setToolTip( tr( "Curve color. Explicit assignment wins over the Y-channel "
                                        "color; defaults are spread over the hue circle so that "
                                        "two curves never start out identical." ) );
        ui.colorButton->setAutoRaise( false );
        connect( ui.colorButton, &QToolButton::clicked, this, [ this, i ]() {
            const QColor chosen = QColorDialog::getColor( xyUI[ i ].color, this, tr( "Curve color" ) );
            if ( chosen.isValid() ) {
                xyUI[ i ].color = chosen;
                setCurveColorSwatch( i );
            }
        } );
        grid->addWidget( ui.colorButton, row, 4 );

        ui.previewLabel = new QLabel();
        grid->addWidget( ui.previewLabel, row, 5 );

        // Update the preview label whenever the channel selections change.
        auto updatePreview = [ this, i ]() {
            if ( i >= int( xyUI.size() ) )
                return;
            const uint x = xyUI[ i ].xChannelCombo->currentData().toUInt();
            const uint y = xyUI[ i ].yChannelCombo->currentData().toUInt();
            const bool timeX = ( x == DsoSettingsScope::timeChannelIndex );
            const bool timeY = ( y == DsoSettingsScope::timeChannelIndex );
            if ( ( timeX || x < settings->scope.voltage.size() ) && ( timeY || y < settings->scope.voltage.size() ) ) {
                const QString xName = timeX ? tr( "T" ) : settings->scope.voltage[ x ].name;
                const QString yName = timeY ? tr( "T" ) : settings->scope.voltage[ y ].name;
                const QString xUnit = timeX ? QStringLiteral( "s" ) : settings->scope.voltage[ x ].ctpuUnit;
                const QString yUnit = timeY ? QStringLiteral( "s" ) : settings->scope.voltage[ y ].ctpuUnit;
                const double xGain = timeX ? settings->scope.horizontal.xyTimeSecPerDiv : settings->scope.physicalGain( x );
                const double yGain = timeY ? settings->scope.horizontal.xyTimeSecPerDiv : settings->scope.physicalGain( y );
                // [FIX] Two bugs: (1) the label used yName for the whole
                // string regardless of which channel was actually assigned
                // to X — every curve showed "CH2(...)" even when X was CH1,
                // M1, or anything else. (2) "дел" was a hardcoded Russian
                // literal bypassing tr() entirely — an English/German/etc.
                // build would show Russian text mixed into an otherwise
                // translated UI. Now shows both channel names explicitly and
                // reuses the same tr("/div") suffix already used in
                // dsowidget.cpp's gain label, so it participates in the
                // normal .ts translation files instead of hardcoding one language.
                xyUI[ i ].previewLabel->setText(
                    tr( "%1 vs %2  (x=%3 %4%5; y=%6 %7%5)" )
                        .arg( xName )
                        .arg( yName )
                        .arg( xGain )
                        .arg( xUnit )
                        .arg( tr( "/div" ) )
                        .arg( yGain )
                        .arg( yUnit ) );
            }
        };
        connect( ui.xChannelCombo, QOverload< int >::of( &QComboBox::currentIndexChanged ), this, updatePreview );
        connect( ui.yChannelCombo, QOverload< int >::of( &QComboBox::currentIndexChanged ), this, updatePreview );
    }
    outer->addLayout( grid );
    outer->addStretch();

    // Pre-fill from settings.
    for ( int i = 0; i < DsoSettingsScope::maxXYCurves && i < int( settings->scope.xyCurves.size() ); ++i ) {
        const auto &c = settings->scope.xyCurves[ i ];
        xyUI[ i ].enabledCheck->setChecked( c.enabled );
        int xi = xyUI[ i ].xChannelCombo->findData( uint( c.xChannel ) );
        int yi = xyUI[ i ].yChannelCombo->findData( uint( c.yChannel ) );
        xyUI[ i ].xChannelCombo->setCurrentIndex( xi >= 0 ? xi : 0 );
        xyUI[ i ].yChannelCombo->setCurrentIndex( yi >= 0 ? yi : 0 );
        // Недействительный цвет означает «не назначен» — берётся умолчание,
        // разное для каждой кривой.
        xyUI[ i ].color = c.explicitColor.isValid() ? c.explicitColor : defaultCurveColor( i );
        setCurveColorSwatch( i );
    }
}


void DsoConfigCtpuMathPage::updateCtpuLabels() {
    for ( size_t ch = 0; ch < ctpuUI.size(); ++ch ) {
        if ( !ctpuUI[ ch ].kLabel || !ctpuUI[ ch ].bLabel )
            continue;
        const double k = ctpuUI[ ch ].kSpin->value();
        const double b = ctpuUI[ ch ].bSpin->value();
        const QString unit = ctpuUI[ ch ].unitEdit->text().isEmpty() ? QStringLiteral( "V" ) : ctpuUI[ ch ].unitEdit->text();
        ctpuUI[ ch ].kLabel->setText( QStringLiteral( "k=%1 %2/V" ).arg( k, 0, 'f', 3 ).arg( unit ) );
        ctpuUI[ ch ].bLabel->setText( QStringLiteral( "b=%1 %2" ).arg( b, 0, 'f', 3 ).arg( unit ) );
    }
}


void DsoConfigCtpuMathPage::updateLiveData( std::shared_ptr< PPresult > data ) {
    if ( !data )
        return;
    // NOTE (important caveat, documented rather than silently hidden): these
    // values come from `DataChannel`, which is filled from samples that
    // already had CtPU applied upstream (HantekDsoControl::stateMachine())
    // if this channel's Mode is not OFF. While actively (re)calibrating a
    // channel, the numbers shown here (and the "Zero V"/"Span V" columns in
    // general, including manual entry) are only raw volts if Mode is OFF or
    // this is the channel's first calibration. This mirrors the existing
    // manual-entry workflow, which has the same characteristic (e.g. the
    // main scope's Vpp label is also post-CtPU once a channel is active) —
    // not a regression introduced here, but worth knowing before relying on
    // "Live" during a re-calibration of an already-calibrated channel.
    for ( size_t ch = 0; ch < ctpuUI.size(); ++ch ) {
        auto &ui = ctpuUI[ ch ];
        if ( !ui.sourceCombo || !ui.liveValueLabel )
            continue;
        if ( ch >= data->channelCount() ) {
            ui.liveValueLabel->setText( tr( "no data yet" ) );
            ui.captureZeroButton->setEnabled( false );
            ui.captureSpanButton->setEnabled( false );
            ui.haveLiveValue = false;
            continue;
        }
        const DataChannel *dc = data->data( ChannelID( ch ) );
        if ( !dc || !dc->valid ) {
            ui.liveValueLabel->setText( tr( "no data yet" ) );
            ui.captureZeroButton->setEnabled( false );
            ui.captureSpanButton->setEnabled( false );
            ui.haveLiveValue = false;
            continue;
        }
        double value = 0.0;
        switch ( ui.sourceCombo->currentData().toInt() ) {
        case 0: value = dc->dc; break;
        case 1: value = dc->dcCycle; break;
        case 2: value = dc->ac; break; // Std Dev == AC rms, same formula (see chat)
        case 3: value = dc->vmax; break;
        case 4: value = dc->vmax - dc->vmin; break; // Vpp
        case 5: value = dc->ac; break;              // AC RMS
        case 6: value = dc->dB; break;
        case 7: value = dc->top; break;
        case 8: value = dc->base; break;
        case 9: value = dc->amplitude; break;
        case 10: value = dc->overshootRise; break;
        case 11: value = dc->overshootFall; break;
        default: value = dc->dc; break;
        }
        ui.lastLiveValue = value;
        ui.haveLiveValue = true;
        ui.liveValueLabel->setText( QStringLiteral( "%1 V" ).arg( value, 0, 'f', 6 ) );
        ui.captureZeroButton->setEnabled( true );
        ui.captureSpanButton->setEnabled( true );
    }
}


void DsoConfigCtpuMathPage::saveSettings() {
    // CtPU
    for ( size_t ch = 0; ch < ctpuUI.size() && ch < settings->scope.voltage.size(); ++ch ) {
        auto &v = settings->scope.voltage[ ch ];
        v.ctpuMode = CtPU::Mode( ctpuUI[ ch ].modeCombo->currentData().toInt() );
        v.ctpuUnit = ctpuUI[ ch ].unitEdit->text().isEmpty() ? QStringLiteral( "V" ) : ctpuUI[ ch ].unitEdit->text();
        v.ctpuK = ctpuUI[ ch ].kSpin->value();
        v.ctpuB = ctpuUI[ ch ].bSpin->value();
        v.ccptuZeroV = ctpuUI[ ch ].zeroVSpin->value();
        v.ccptuSpanV = ctpuUI[ ch ].spanVSpin->value();
        v.ccptuSpanPhysical = ctpuUI[ ch ].spanPhysSpin->value();
    }
    // Math
    settings->scope.mathStack.resize( DsoSettingsScope::maxMathChannels );
    for ( int i = 0; i < DsoSettingsScope::maxMathChannels; ++i ) {
        auto &m = settings->scope.mathStack[ i ];
        m.enabled = mathUI[ i ].enabledCheck->isChecked();
        m.srcA = uint8_t( mathUI[ i ].srcASpin->value() );
        m.srcB = uint8_t( mathUI[ i ].srcBSpin->value() );
        m.op = Dso::MathOp( mathUI[ i ].opCombo->currentData().toUInt() );
        m.invert = mathUI[ i ].invertCheck->isChecked();
        m.ctpuUnit = mathUI[ i ].unitEdit->text().isEmpty() ? QStringLiteral( "V" ) : mathUI[ i ].unitEdit->text();
        m.ctpuK = mathUI[ i ].kSpin->value();
        m.ctpuB = mathUI[ i ].bSpin->value();
    }
    // XY
    settings->scope.xyCurves.resize( DsoSettingsScope::maxXYCurves );
    for ( int i = 0; i < DsoSettingsScope::maxXYCurves; ++i ) {
        auto &c = settings->scope.xyCurves[ i ];
        c.enabled = xyUI[ i ].enabledCheck->isChecked();
        c.xChannel = uint8_t( xyUI[ i ].xChannelCombo->currentData().toUInt() );
        c.explicitColor = xyUI[ i ].color;
        c.yChannel = uint8_t( xyUI[ i ].yChannelCombo->currentData().toUInt() );
    }
}


/// Красит кнопку выбора в действующий цвет кривой и подписывает его кодом.
/// Совпадение двух кривых видно немедленно — ровно та жалоба, ради которой
/// задание 4 и заведено.
void DsoConfigCtpuMathPage::setCurveColorSwatch( int curve ) {
    if ( curve < 0 || curve >= int( xyUI.size() ) || !xyUI[ curve ].colorButton )
        return;
    const QColor &c = xyUI[ curve ].color;
    xyUI[ curve ].colorButton->setStyleSheet(
        QStringLiteral( "QToolButton { background-color: %1; border: 1px solid #808080; }" ).arg( c.name() ) );
    xyUI[ curve ].colorButton->setText( c.name().toUpper() );
}
