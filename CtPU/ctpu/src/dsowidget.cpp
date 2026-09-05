// SPDX-License-Identifier: GPL-2.0-or-later
// Last edited: 2026-08-11 11:31:17 UTC

#include <cmath>

#include <QApplication>
#include <QFileDialog>
#include <QGridLayout>
#include <QDateTime>
#include <QLabel>
#include <QPainter>
#include <QScreen>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolTip>
#include <QWheelEvent>

#include "dsowidget.h"

#include "hantekdso/mathmodes.h"

#include "glscope.h"
#include "scopesettings.h"
#include "utils/printutils.h"
#include "viewconstants.h"
#include "widgets/datagrid.h"
#include "widgets/levelslider.h"


DsoWidget::DsoWidget( DsoSettingsScope *scope, DsoSettingsView *view, const Dso::ControlSpecification *spec, QWidget *parent )
    : QWidget( parent ), scope( scope ), view( view ), spec( spec ), mainScope( GlScope::createNormal( scope, view ) ),
      zoomScope( GlScope::createZoomed( scope, view ) ) {
    // Задание 3 очереди: кадр веб-камеры самым задним слоем холста.
    // Слой включается настройкой; если камеры нет или сборка без Qt
    // Multimedia — start() честно вернёт false, и слой просто не появится.
    cameraLayer = new CameraLayer( this );
    cameraLayer->setOrientation( view->cameraRotation, view->cameraMirrorH, view->cameraMirrorV );
    connect( cameraLayer, &CameraLayer::frameReady, this, [ this, view ]() {
        if ( view->cameraLayerEnabled )
            mainScope->setCameraFrame( cameraLayer->frame() );
    } );
    // Камера открывается только при ЯВНО выбранном приборе (распоряжение
    // автора 2026-09-02). Пустой идентификатор - «не выбрана», и start() сам
    // вернёт false, ничего не открыв.
    if ( view->cameraLayerEnabled && !view->cameraDeviceId.isEmpty() )
        cameraLayer->start( view->cameraDeviceId );


    if ( scope->verboseLevel > 1 )
        qDebug() << " DsoWidget::DsoWidget()";

    // [MOD] TZ §9.1.1 (Variant B) — no cache vectors to size here anymore.
    // updateVoltageDetails()/updateSpectrumDetails() below (called for every
    // channel, including the default-`used=true` CH1/CH2) now read the unit
    // via unitFor(channel), which reads straight from `scope->voltage`/
    // `scope->mathStack` — both already correctly sized by DsoSettings
    // before DsoWidget is constructed. No out-of-bounds window remains.

    QSize screenSize = QGuiApplication::primaryScreen()->size();
    view->screenHeight = unsigned( screenSize.height() );
    view->screenWidth = unsigned( screenSize.width() );

    QPalette palette;
    palette.setColor( QPalette::Window, view->colors->background );
    palette.setColor( QPalette::WindowText, view->colors->text );

    setupSliders( mainSliders );
    setupSliders( zoomSliders );

    connect( mainScope, &GlScope::markerMoved, mainScope, [ this ]( int cursorIndex, int marker ) {
        mainSliders.markerSlider->setValue( marker, this->scope->getMarker( marker ) );
        mainScope->updateCursor( cursorIndex );
        zoomScope->updateCursor( cursorIndex );
    } );
    connect( zoomScope, &GlScope::markerMoved, mainScope, [ this ]( int cursorIndex, int marker ) {
        mainSliders.markerSlider->setValue( int( marker ), this->scope->getMarker( marker ) );
        mainScope->updateCursor( cursorIndex );
        zoomScope->updateCursor( cursorIndex );
    } );

    connect( mainScope, &GlScope::cursorMeasurement, this, [ this ]( QPointF mPos, QPoint gPos, bool status ) {
        cursorMeasurementPosition = mPos;
        cursorGlobalPosition = gPos;
        cursorMeasurementValid = status;
        if ( !status )
            showCursorMessage();
    } );
    connect( zoomScope, &GlScope::cursorMeasurement, this, [ this ]( QPointF mPos, QPoint gPos, bool status ) {
        cursorMeasurementPosition = mPos;
        cursorGlobalPosition = gPos;
        cursorMeasurementValid = status;
        if ( !status )
            showCursorMessage();
    } );

    settingsTriggerLabel = new QLabel();
    settingsTriggerLabel->setMinimumWidth( 320 );
    settingsTriggerLabel->setIndent( 5 );
    settingsSamplesOnScreen = new QLabel();
    settingsSamplesOnScreen->setPalette( palette );
    settingsSamplerateLabel = new QLabel();
    settingsSamplerateLabel->setPalette( palette );
    settingsOversampleLabel = new QLabel();
    settingsOversampleLabel->setPalette( palette );
    settingsTimebaseLabel = new QLabel();
    settingsTimebaseLabel->setPalette( palette );
    settingsFrequencybaseLabel = new QLabel();
    settingsFrequencybaseLabel->setPalette( palette );
    swTriggerStatus = new QLabel();
    swTriggerStatus->setMinimumWidth( 20 );
    swTriggerStatus->setAutoFillBackground( true );
    swTriggerStatus->setVisible( false );
    frameAgeLabel = new QLabel();
    frameAgeLabel->setPalette( palette );
    frameAgeLabel->setToolTip(
        tr( "Age of the displayed frame: time from raw-data conversion to this display update.\n"
            "A measurement is only trustworthy together with how stale it is (D-05)." ) );
    frameAgeLabel->setVisible( false );
    settingsLayout = new QHBoxLayout();
    settingsLayout->addWidget( swTriggerStatus, 0, Qt::AlignCenter );
    settingsLayout->addWidget( frameAgeLabel, 0, Qt::AlignLeft );
    settingsLayout->addWidget( settingsTriggerLabel, 0, Qt::AlignLeft );
    settingsLayout->addWidget( settingsSamplesOnScreen, 1, Qt::AlignRight );
    settingsLayout->addWidget( settingsSamplerateLabel, 1, Qt::AlignRight );
    settingsLayout->addWidget( settingsOversampleLabel, 1, Qt::AlignLeft );
    settingsLayout->addWidget( settingsTimebaseLabel, 1, Qt::AlignRight );
    settingsLayout->addWidget( settingsFrequencybaseLabel, 1, Qt::AlignRight );

    markerInfoLabel = new QLabel();
    markerInfoLabel->setPalette( palette );
    markerTimeLabel = new QLabel();
    markerTimeLabel->setPalette( palette );
    markerFrequencyLabel = new QLabel();
    markerFrequencyLabel->setPalette( palette );
    markerTimebaseLabel = new QLabel();
    markerTimebaseLabel->setPalette( palette );
    markerFrequencybaseLabel = new QLabel();
    markerFrequencybaseLabel->setPalette( palette );
    markerLayout = new QHBoxLayout();
    markerLayout->addWidget( markerInfoLabel, 0, Qt::AlignLeft );
    markerLayout->addWidget( markerTimeLabel, 1, Qt::AlignLeft );
    markerLayout->addWidget( markerFrequencyLabel, 1, Qt::AlignLeft );
    markerLayout->addWidget( markerTimebaseLabel, 1, Qt::AlignRight );
    markerLayout->addWidget( markerFrequencybaseLabel, 1, Qt::AlignRight );

    measurementLayout = new QGridLayout();
    measurementLayout->setColumnMinimumWidth( 0, 50 );
    measurementLayout->setColumnMinimumWidth( 1, 30 );
    measurementLayout->setColumnStretch( 2, 3 );
    measurementLayout->setColumnStretch( 3, 3 );
    measurementLayout->setColumnStretch( 4, 3 );
    measurementLayout->setColumnStretch( 5, 3 );
    measurementLayout->setColumnStretch( 6, 3 );
    measurementLayout->setColumnStretch( 7, 4 );
    measurementLayout->setColumnStretch( 8, 3 );
    measurementLayout->setColumnStretch( 9, 3 );
    measurementLayout->setColumnStretch( 10, 2 );
    measurementLayout->setColumnStretch( 11, 3 );
    measurementLayout->setColumnStretch( 12, 3 );
    for ( ChannelID channel = 0; channel < scope->voltage.size(); ++channel ) {
        QPalette voltagePalette = palette;
        QPalette spectrumPalette = palette;
        voltagePalette.setColor( QPalette::WindowText, view->colors->voltage[ channel ] );
        spectrumPalette.setColor( QPalette::WindowText, view->colors->spectrum[ channel ] );
        measurementNameLabel.push_back( new QLabel( scope->voltage[ channel ].name ) );
        measurementNameLabel[ channel ]->setPalette( voltagePalette );
        measurementNameLabel[ channel ]->setAutoFillBackground( true );
        measurementMiscLabel.push_back( new QLabel() );
        measurementMiscLabel[ channel ]->setPalette( voltagePalette );
        measurementGainLabel.push_back( new QLabel() );
        measurementGainLabel[ channel ]->setPalette( voltagePalette );
        measurementMagnitudeLabel.push_back( new QLabel() );
        measurementMagnitudeLabel[ channel ]->setPalette( spectrumPalette );
        measurementVppLabel.push_back( new QLabel() );
        measurementVppLabel[ channel ]->setPalette( voltagePalette );
        measurementDCLabel.push_back( new QLabel() );
        measurementDCLabel[ channel ]->setPalette( voltagePalette );
        measurementACLabel.push_back( new QLabel() );
        measurementACLabel[ channel ]->setPalette( voltagePalette );
        measurementRMSLabel.push_back( new QLabel() );
        measurementRMSLabel[ channel ]->setPalette( voltagePalette );
        measurementdBLabel.push_back( new QLabel() );
        measurementdBLabel[ channel ]->setPalette( voltagePalette );
        measurementRMSPowerLabel.push_back( new QLabel() );
        measurementRMSPowerLabel[ channel ]->setPalette( voltagePalette );
        measurementTHDLabel.push_back( new QLabel() );
        measurementTHDLabel[ channel ]->setPalette( voltagePalette );
        measurementFrequencyLabel.push_back( new QLabel() );
        measurementFrequencyLabel[ channel ]->setPalette( voltagePalette );
        measurementNoteLabel.push_back( new QLabel() );
        measurementNoteLabel[ channel ]->setIndent( view->fontSize );
        measurementNoteLabel[ channel ]->setPalette( voltagePalette );
        setMeasurementVisible( channel );
        int col = 0;
        measurementLayout->addWidget( measurementNameLabel[ channel ], int( channel ), col++, Qt::AlignLeft );
        measurementLayout->addWidget( measurementMiscLabel[ channel ], int( channel ), col++, Qt::AlignCenter );
        measurementLayout->addWidget( measurementGainLabel[ channel ], int( channel ), col++, Qt::AlignRight );
        measurementLayout->addWidget( measurementMagnitudeLabel[ channel ], int( channel ), col++, Qt::AlignRight );
        measurementLayout->addWidget( measurementVppLabel[ channel ], int( channel ), col++, Qt::AlignRight );
        measurementLayout->addWidget( measurementDCLabel[ channel ], int( channel ), col++, Qt::AlignRight );
        measurementLayout->addWidget( measurementACLabel[ channel ], int( channel ), col++, Qt::AlignRight );
        measurementLayout->addWidget( measurementRMSLabel[ channel ], int( channel ), col++, Qt::AlignRight );
        measurementLayout->addWidget( measurementdBLabel[ channel ], int( channel ), col++, Qt::AlignRight );
        measurementLayout->addWidget( measurementRMSPowerLabel[ channel ], int( channel ), col++, Qt::AlignRight );
        measurementLayout->addWidget( measurementTHDLabel[ channel ], int( channel ), col++, Qt::AlignRight );
        measurementLayout->addWidget( measurementFrequencyLabel[ channel ], int( channel ), col++, Qt::AlignRight );
        measurementLayout->addWidget( measurementNoteLabel[ channel ], int( channel ), col++, Qt::AlignLeft );
        if ( channel < spec->channels )
            updateVoltageCoupling( channel );
        else
            updateMathMode();
        updateVoltageDetails( channel );
        updateSpectrumDetails( channel );
    }

    cursorDataGrid = new DataGrid( this );
    cursorDataGrid->setBackgroundColor( view->colors->background );
    cursorDataGrid->setToolTipsVisible( scope->toolTipVisible );

    cursorDataGrid->addItem( tr( "Markers" ), view->colors->text );
    for ( ChannelID channel = 0; channel < scope->voltage.size(); ++channel ) {
        cursorDataGrid->addItem( scope->voltage[ channel ].name, view->colors->voltage[ channel ] );
    }
    for ( ChannelID channel = 0; channel < scope->spectrum.size(); ++channel ) {
        cursorDataGrid->addItem( scope->spectrum[ channel ].name, view->colors->spectrum[ channel ] );
    }
    cursorDataGrid->selectItem( 0 );

    connect( cursorDataGrid, &DataGrid::itemSelected, mainScope, [ this ]( int index ) {
        mainScope->selectCursor( index );
        zoomScope->selectCursor( index );
        updateItem( ChannelID( index ), true );
    } );

    connect( cursorDataGrid, &DataGrid::itemUpdated, this, [ this ]( int index ) { updateItem( ChannelID( index ) ); } );

    scope->horizontal.cursor.shape = DsoSettingsScopeCursor::VERTICAL;

    mainLayout = new QGridLayout();
    mainLayout->setColumnMinimumWidth( 2, mainSliders.triggerPositionSlider->preMargin() );
    mainLayout->setColumnMinimumWidth( 4, mainSliders.triggerPositionSlider->postMargin() );
    mainLayout->setSpacing( 0 );
    int row = 0;
    mainLayout->addLayout( settingsLayout, row, 0, 1, 7 );
    ++row;
    mainLayout->addWidget( mainSliders.triggerPositionSlider, row, 2, 2, 3, Qt::AlignBottom );
    ++row;
    mainLayout->setRowMinimumHeight( row, mainSliders.voltageOffsetSlider->preMargin() );
    mainLayout->addWidget( mainSliders.voltageOffsetSlider, row, 1, 3, 2, Qt::AlignRight );
    mainLayout->addWidget( mainSliders.triggerLevelSlider, row, 4, 3, 2, Qt::AlignLeft );
    mainScopeRow = ++row;
    const int scopeCol = 3;
    mainLayout->setColumnStretch( scopeCol, 1 );
    mainLayout->setRowStretch( mainScopeRow, 1 );
    mainLayout->addWidget( mainScope, mainScopeRow, scopeCol );
    ++row;
    mainLayout->setRowMinimumHeight( row, mainSliders.voltageOffsetSlider->postMargin() );
    mainLayout->addWidget( mainSliders.markerSlider, row, 2, 2, 3, Qt::AlignTop );
    row += 2;
    mainLayout->addLayout( markerLayout, row, scopeCol, 1, 3 );
    ++row;
    mainLayout->addWidget( zoomSliders.triggerPositionSlider, row, 2, 2, 3, Qt::AlignBottom );
    ++row;
    mainLayout->setRowMinimumHeight( row, zoomSliders.voltageOffsetSlider->preMargin() );
    mainLayout->addWidget( zoomSliders.voltageOffsetSlider, row, 1, 3, 2, Qt::AlignRight );
    mainLayout->addWidget( zoomSliders.triggerLevelSlider, row, 4, 3, 2, Qt::AlignLeft );
    zoomScopeRow = ++row;
    mainLayout->setRowStretch( zoomScopeRow, 0 );
    mainLayout->addWidget( zoomScope, zoomScopeRow, scopeCol );
    ++row;
    mainLayout->setRowMinimumHeight( row, zoomSliders.voltageOffsetSlider->postMargin() );
    const int measurementRow = ++row;
    mainLayout->addLayout( measurementLayout, measurementRow, 0, 1, -1 );
    updateCursorGrid( view->cursorsVisible );

    setPalette( palette );
    setBackgroundRole( QPalette::Window );
    setAutoFillBackground( true );
    setLayout( mainLayout );

    connect( mainSliders.voltageOffsetSlider, &LevelSlider::valueChanged, this, &DsoWidget::updateOffset );
    connect( zoomSliders.voltageOffsetSlider, &LevelSlider::valueChanged, this, &DsoWidget::updateOffset );

    connect( mainSliders.triggerPositionSlider, &LevelSlider::valueChanged, this,
             [ this ]( int index, double value, bool pressed, QPoint globalPos ) {
                 updateTriggerPosition( index, value, pressed, globalPos, true );
             } );
    connect( zoomSliders.triggerPositionSlider, &LevelSlider::valueChanged, this,
             [ this ]( int index, double value, bool pressed, QPoint globalPos ) {
                 updateTriggerPosition( index, value, pressed, globalPos, false );
             } );

    connect( mainSliders.triggerLevelSlider, &LevelSlider::valueChanged, this, &DsoWidget::updateTriggerLevel );
    connect( zoomSliders.triggerLevelSlider, &LevelSlider::valueChanged, this, &DsoWidget::updateTriggerLevel );

    connect( mainSliders.triggerLevelSlider, &LevelSlider::valueChanged, mainScope,
             [ this ]( int index, double value, bool pressed ) {
                 mainScope->generateGrid( index, value, pressed );
                 zoomScope->generateGrid( index, value, pressed );
             } );
    connect( zoomSliders.triggerLevelSlider, &LevelSlider::valueChanged, mainScope,
             [ this ]( int index, double value, bool pressed ) {
                 mainScope->generateGrid( index, value, pressed );
                 zoomScope->generateGrid( index, value, pressed );
             } );

    connect( mainSliders.markerSlider, &LevelSlider::valueChanged, [ this ]( int index, double value ) {
        updateMarker( unsigned( index ), value );
        mainScope->updateCursor();
        zoomScope->updateCursor();
    } );
    zoomSliders.markerSlider->setEnabled( false );
}


DsoWidget::~DsoWidget() {
    if ( scope->verboseLevel > 1 )
        qDebug() << " DsoWidget::~DsoWidget()";
}


void DsoWidget::switchToPrintColors() {
    if ( view->printerColorImages ) {
        view->colors = &view->print;
        setColors();
    }
}


void DsoWidget::restoreScreenColors() {
    if ( view->colors != &view->screen ) {
        view->colors = &view->screen;
        setColors();
    }
}


void DsoWidget::setColors() {
    if ( scope->verboseLevel > 2 )
        qDebug() << "  DsoWidget::setColors()";
    ChannelID numChannels = ChannelID( scope->voltage.size() );
    cursorDataGrid->setBackgroundColor( view->colors->background );
    cursorDataGrid->configureItem( 0, view->colors->text );
    QPalette paletteNow;
    paletteNow.setColor( QPalette::Window, view->colors->background );
    paletteNow.setColor( QPalette::WindowText, view->colors->text );
    settingsSamplesOnScreen->setPalette( paletteNow );
    settingsSamplerateLabel->setPalette( paletteNow );
    settingsTimebaseLabel->setPalette( paletteNow );
    settingsFrequencybaseLabel->setPalette( paletteNow );
    markerInfoLabel->setPalette( paletteNow );
    markerTimeLabel->setPalette( paletteNow );
    markerFrequencyLabel->setPalette( paletteNow );
    markerTimebaseLabel->setPalette( paletteNow );
    markerFrequencybaseLabel->setPalette( paletteNow );
    QPalette tablePalette = paletteNow;
    for ( ChannelID channel = 0; channel < numChannels; ++channel ) {
        tablePalette.setColor( QPalette::WindowText, view->colors->spectrum[ channel ] );
        measurementMagnitudeLabel[ channel ]->setPalette( tablePalette );
        tablePalette.setColor( QPalette::WindowText, view->colors->voltage[ channel ] );
        measurementNameLabel[ channel ]->setPalette( tablePalette );
        measurementMiscLabel[ channel ]->setPalette( tablePalette );
        measurementGainLabel[ channel ]->setPalette( tablePalette );
        mainSliders.voltageOffsetSlider->setColor( ( channel ), view->colors->voltage[ channel ] );
        zoomSliders.voltageOffsetSlider->setColor( ( channel ), view->colors->voltage[ channel ] );
        mainSliders.voltageOffsetSlider->setColor( channel + numChannels, view->colors->spectrum[ channel ] );
        zoomSliders.voltageOffsetSlider->setColor( channel + numChannels, view->colors->spectrum[ channel ] );
        measurementVppLabel[ channel ]->setPalette( tablePalette );
        measurementDCLabel[ channel ]->setPalette( tablePalette );
        measurementACLabel[ channel ]->setPalette( tablePalette );
        measurementRMSLabel[ channel ]->setPalette( tablePalette );
        measurementdBLabel[ channel ]->setPalette( tablePalette );
        measurementRMSPowerLabel[ channel ]->setPalette( tablePalette );
        measurementTHDLabel[ channel ]->setPalette( tablePalette );
        measurementFrequencyLabel[ channel ]->setPalette( tablePalette );
        measurementNoteLabel[ channel ]->setPalette( tablePalette );
        cursorDataGrid->configureItem( channel + 1, view->colors->voltage[ channel ] );
        cursorDataGrid->configureItem( channel + numChannels + 1, view->colors->spectrum[ channel ] );
    }

    tablePalette = palette();
    tablePalette.setColor( QPalette::WindowText, view->colors->voltage[ unsigned( scope->trigger.source ) ] );
    settingsTriggerLabel->setPalette( tablePalette );
    updateTriggerSource();
    setPalette( paletteNow );
    setBackgroundRole( QPalette::Window );
}


void DsoWidget::updateCursorGrid( bool enabled ) {
    if ( !enabled ) {
        cursorDataGrid->selectItem( 0 );
        cursorDataGrid->setParent( nullptr );
        mainScope->selectCursor( 0 );
        zoomScope->selectCursor( 0 );
        return;
    }
    const int rows = mainLayout->rowCount() - mainScopeRow - 1;
    const int leftColumn = 0;
    const int rightColumn = mainLayout->columnCount() - 1;
    switch ( view->cursorGridPosition ) {
    case Qt::LeftToolBarArea:
        if ( mainLayout->itemAtPosition( mainScopeRow, leftColumn ) == nullptr ) {
            cursorDataGrid->setParent( nullptr );
            mainLayout->addWidget( cursorDataGrid, mainScopeRow, leftColumn, rows, 1 );
        }
        break;
    case Qt::RightToolBarArea:
        if ( mainLayout->itemAtPosition( mainScopeRow, rightColumn ) == nullptr ) {
            cursorDataGrid->setParent( nullptr );
            mainLayout->addWidget( cursorDataGrid, mainScopeRow, rightColumn, rows, 1 );
        }
        break;
    default:
        if ( cursorDataGrid->parent() != nullptr ) {
            cursorDataGrid->setParent( nullptr );
        }
        break;
    }
}


/// \brief Physical unit string for a unified channel index (TZ §9.1.1, Variant B).
/// See the declaration comment in dsowidget.h for why this can't simply be
/// `scope->voltage[channel].ctpuUnit` for every channel: math-channel units
/// live in `scope->mathStack[mathIndex].ctpuUnit`, not on the voltage[]
/// entry (the Math config tab never writes it there).
/// \brief Единица канала для всех экранных подписей.
///
/// Задание 11 очереди: настройки PU обязаны действовать на экранные единицы.
/// Правило одно и здесь, и в списке В/дел (`VoltageDock::gainStringsForChannel`):
///
///   * PU включён и единица задана — берётся она;
///   * иначе вещественный канал показывает **V**;
///   * иначе математический канал показывает единицу СВОЕЙ операции: у
///     произведения это V², и подставлять вольт вместо неё нельзя — подпись
///     станет неверной.
///
/// Пустая строка не возвращается никогда: величина без единицы на экране
/// запрещена (`docs/СЛЫШИМОСТЬ.md` §5).
QString DsoWidget::unitFor( ChannelID channel ) const {
    if ( channel >= scope->voltage.size() )
        return QStringLiteral( "V" );

    // Единица PU: у математического канала своя запись в mathStack.
    QString ctpuUnit = scope->voltage[ channel ].ctpuUnit;
    bool ctpuOn = scope->voltage[ channel ].ctpuMode != CtPU::Mode::OFF;
    if ( channel >= spec->channels && !scope->mathStack.empty() ) {
        const unsigned mathIndex = unsigned( channel ) - spec->channels;
        if ( mathIndex < scope->mathStack.size() && !scope->mathStack[ mathIndex ].ctpuUnit.isEmpty() ) {
            ctpuUnit = scope->mathStack[ mathIndex ].ctpuUnit;
            ctpuOn = true;
        }
    }
    if ( ctpuOn && !ctpuUnit.isEmpty() )
        return ctpuUnit;

    if ( channel < spec->channels )
        return QStringLiteral( "V" );
    // Математический канал без PU: единица его операции. valueToString с
    // нулевым значением и нулевой точностью даёт «0 V» / «0 V²» — берётся
    // хвост после числа, чтобы не заводить второй таблицы единиц.
    const QString sample =
        valueToString( 0.0, Dso::mathModeUnit( Dso::MathMode( scope->voltage[ spec->channels ].couplingOrMathIndex ) ), 0 );
    const int sp = sample.indexOf( QLatin1Char( ' ' ) );
    return sp >= 0 ? sample.mid( sp + 1 ) : QStringLiteral( "V" );
}


void DsoWidget::updateItem( ChannelID index, bool switchOn ) {
    selectedCursor = index;
    ChannelID channelCount = scope->countChannels();
    if ( 0 < index && index < channelCount + 1 ) {
        ChannelID channel = ChannelID( index - 1 );
        if ( scope->voltage[ channel ].used ) {
            if ( switchOn || scope->voltage[ channel ].cursor.shape == DsoSettingsScopeCursor::NONE ) {
                scope->voltage[ channel ].cursor.shape = DsoSettingsScopeCursor::RECTANGULAR;
            } else {
                scope->voltage[ channel ].cursor.shape = DsoSettingsScopeCursor::NONE;
            }
        }
    } else if ( channelCount < index && index < 2 * channelCount + 1 ) {
        ChannelID channel = ChannelID( index - channelCount - 1 );
        if ( scope->spectrum[ channel ].used ) {
            if ( switchOn || scope->spectrum[ channel ].cursor.shape == DsoSettingsScopeCursor::NONE ) {
                scope->spectrum[ channel ].cursor.shape = DsoSettingsScopeCursor::RECTANGULAR;
            } else {
                scope->spectrum[ channel ].cursor.shape = DsoSettingsScopeCursor::NONE;
            }
        }
    }
    updateMarkerDetails();
    mainScope->updateCursor( int( index ) );
    zoomScope->updateCursor( int( index ) );
}


void DsoWidget::setupSliders( DsoWidget::Sliders &sliders ) {
    if ( scope->verboseLevel > 2 )
        qDebug() << "  DsoWidget::setupSliders()";
    sliders.voltageOffsetSlider = new LevelSlider( Qt::RightArrow );
    if ( scope->toolTipVisible )
        sliders.voltageOffsetSlider->setToolTip( tr( "Trace position, drag the channel name up or down" ) );
    for ( ChannelID channel = 0; channel < scope->voltage.size(); ++channel ) {
        sliders.voltageOffsetSlider->addSlider( scope->voltage[ channel ].name, int( channel ) );
        // [FIX] Смещение здесь — ЧИСТО ПРОГРАММНОЕ. В дорогих осциллографах
        // offset задаёт рабочую точку входного каскада, поэтому сигнал
        // физически подводится в центр диапазона АЦП и «уехать за экран» не
        // может — отсюда и традиционный лимит в пол-экрана (±DIVS_VOLTAGE/2).
        // В этом классе приборов рабочая точка жёстко привязана к GND, двигать
        // можно только ИЗОБРАЖЕНИЕ, а лимит унаследован от логики, где он имел
        // другой смысл.
        //
        // Для физических каналов это почти незаметно: АЦП по построению
        // покрывает ровно DIVS_VOLTAGE делений, так что неклиппированный
        // сигнал всегда лежит в пределах ±4 делений от GND. Но:
        //   - math-каналы (M1 = CH1 + CH2 и т.п.) достигают ±DIVS_VOLTAGE
        //     делений, то есть ВДВОЕ больше старого лимита;
        //   - CtPU-каналы после k*x+b масштабируются произвольно и к сетке
        //     в 8 делений вообще не привязаны.
        // В этих случаях данные полностью корректны, аппаратного клиппинга
        // нет (result.clipped взводится только по сырым 0x00/0xFF в
        // hantekdsocontrol.cpp), но докрутить изображение нечем — и выглядит
        // это обрезкой, хотя обрезки нет. Именно поэтому дефект годами не
        // вызывал жалоб: при сигнале около нуля в лимит никто не упирается.
        //
        // Норматив OFFSET_LIMIT_DIVS: полный ход — три высоты экрана, ровно
        // столько нужно, чтобы поставить самый нижний уровень сигнала к
        // верхней кромке, а самый верхний — к нижней. Помимо math/CtPU это
        // проявлялось и на обычной AC-связи: в DC 500 мВ/дел меандр
        // калибратора 0..+2 В виден полностью, а после переключения в AC
        // регулятора переставало хватать. Это лимит ПОЛЗУНКА, а не данных.
        // Ср. соседний слайдер уровня триггера (ниже в этом же файле), где
        // лимиты уже считаются динамически, а не константой — то есть в
        // проекте образец правильного подхода уже есть.
        sliders.voltageOffsetSlider->setLimits( int( channel ), -OFFSET_LIMIT_DIVS, OFFSET_LIMIT_DIVS );
        sliders.voltageOffsetSlider->setColor( ( channel ), view->colors->voltage[ channel ] );
        sliders.voltageOffsetSlider->setStep( int( channel ), 0.2 );
        sliders.voltageOffsetSlider->setValue( int( channel ), scope->voltage[ channel ].offset );
        sliders.voltageOffsetSlider->setIndexVisible( channel, scope->voltage[ channel ].used );
    }
    for ( ChannelID channel = 0; channel < scope->voltage.size(); ++channel ) {
        sliders.voltageOffsetSlider->addSlider( scope->spectrum[ channel ].name, int( scope->voltage.size() + channel ) );
        sliders.voltageOffsetSlider->setColor( unsigned( scope->voltage.size() ) + channel, view->colors->spectrum[ channel ] );
        // [FIX] та же причина, что и для канала напряжения выше: лимит хода
        // ползунка не должен мешать вывести на экран корректные данные.
        sliders.voltageOffsetSlider->setLimits( int( scope->voltage.size() + channel ), -OFFSET_LIMIT_DIVS,
                                                OFFSET_LIMIT_DIVS );
        sliders.voltageOffsetSlider->setStep( int( scope->voltage.size() + channel ), 0.2 );
        sliders.voltageOffsetSlider->setValue( int( scope->voltage.size() + channel ), scope->spectrum[ channel ].offset );
        sliders.voltageOffsetSlider->setIndexVisible( unsigned( scope->voltage.size() ) + channel,
                                                      scope->spectrum[ channel ].used );
    }

    sliders.triggerPositionSlider = new LevelSlider( Qt::DownArrow );
    if ( scope->toolTipVisible )
        sliders.triggerPositionSlider->setToolTip( tr( "Trigger position, drag the arrow left or right" ) );
    sliders.triggerPositionSlider->addSlider();
    sliders.triggerPositionSlider->setLimits( 0, 0.0, 1.0 );
    sliders.triggerPositionSlider->setStep( 0, 0.2 / double( DIVS_TIME ) );
    sliders.triggerPositionSlider->setValue( 0, scope->trigger.position );
    sliders.triggerPositionSlider->setIndexVisible( 0, true );

    sliders.triggerLevelSlider = new LevelSlider( Qt::LeftArrow );
    if ( scope->toolTipVisible )
        sliders.triggerLevelSlider->setToolTip( tr( "Trigger level, drag the arrow up or down" ) );
    for ( ChannelID channel = 0; channel < scope->voltage.size(); ++channel ) {
        sliders.triggerLevelSlider->addSlider( int( channel ) );
        sliders.triggerLevelSlider->setColor( channel, ( channel == ChannelID( scope->trigger.source ) )
                                                           ? view->colors->voltage[ channel ]
                                                           : view->colors->voltage[ channel ].darker() );
        adaptTriggerLevelSlider( sliders, channel );
        sliders.triggerLevelSlider->setValue( int( channel ), scope->voltage[ channel ].trigger );
        sliders.triggerLevelSlider->setIndexVisible( channel, scope->voltage[ channel ].used );
    }

    sliders.markerSlider = new LevelSlider( Qt::UpArrow );
    if ( scope->toolTipVisible )
        sliders.markerSlider->setToolTip( tr( "Measure or zoom marker '1' and '2', drag left or right" ) );
    for ( int marker = 0; marker < 2; ++marker ) {
        sliders.markerSlider->addSlider( QString::number( marker + 1 ), marker );
        sliders.markerSlider->setLimits( marker, MARGIN_LEFT, MARGIN_RIGHT );
        sliders.markerSlider->setStep( marker, MARKER_STEP );
        sliders.markerSlider->setValue( marker, scope->horizontal.cursor.pos[ marker ].x() );
        sliders.markerSlider->setIndexVisible( unsigned( marker ), true );
    }
}


void DsoWidget::adaptTriggerLevelSlider( DsoWidget::Sliders &sliders, ChannelID channel ) {
    // [MOD] TZ §9.3 — пределы бегунка идут через displayGain(), а не
    // physicalGain(): бегунок стоит в экранных делениях, и при экранном
    // увеличении (У4б) деление стоит меньше физических единиц. Прежде здесь
    // был physicalGain, а трасса рисовалась с делением на zoom — порог
    // расходился с трассой ровно в √N раз (`docs/PROTOTYPE-QUEUE.md` №7).
    sliders.triggerLevelSlider->setLimits( int( channel ),
                                           ( -DIVS_VOLTAGE / 2 - scope->voltage[ channel ].offset ) * scope->displayGain( channel ),
                                           ( DIVS_VOLTAGE / 2 - scope->voltage[ channel ].offset ) * scope->displayGain( channel ) );
    sliders.triggerLevelSlider->setStep( int( channel ), scope->displayGain( channel ) * 0.05 );
    double value = sliders.triggerLevelSlider->value( int( channel ) );
    if ( bool( value ) ) {
    }
}


void DsoWidget::setMeasurementVisible( ChannelID channel ) {
    bool visible = scope->voltage[ channel ].used || scope->spectrum[ channel ].used;
    if ( visible ) {
        measurementNameLabel[ channel ]->show();
        measurementMiscLabel[ channel ]->show();
        measurementGainLabel[ channel ]->show();
        measurementMagnitudeLabel[ channel ]->show();
        measurementVppLabel[ channel ]->show();
        measurementDCLabel[ channel ]->show();
        measurementACLabel[ channel ]->show();
        measurementRMSLabel[ channel ]->show();
        measurementdBLabel[ channel ]->show();
        measurementRMSPowerLabel[ channel ]->show();
        measurementTHDLabel[ channel ]->show();
        measurementFrequencyLabel[ channel ]->show();
        measurementNoteLabel[ channel ]->show();
        if ( scope->voltage[ channel ].used )
            measurementGainLabel[ channel ]->show();
        else
            measurementGainLabel[ channel ]->setText( QString() );
        if ( scope->spectrum[ channel ].used )
            measurementMagnitudeLabel[ channel ]->show();
        else
            measurementMagnitudeLabel[ channel ]->setText( QString() );
    } else {
        measurementNameLabel[ channel ]->hide();
        measurementMiscLabel[ channel ]->hide();
        measurementGainLabel[ channel ]->hide();
        measurementMagnitudeLabel[ channel ]->hide();
        measurementVppLabel[ channel ]->hide();
        measurementDCLabel[ channel ]->hide();
        measurementACLabel[ channel ]->hide();
        measurementRMSLabel[ channel ]->hide();
        measurementdBLabel[ channel ]->hide();
        measurementRMSPowerLabel[ channel ]->hide();
        measurementTHDLabel[ channel ]->hide();
        measurementFrequencyLabel[ channel ]->hide();
        measurementNoteLabel[ channel ]->hide();
    }
}


void DsoWidget::updateMarkerDetails() {
    if ( nullptr == cursorDataGrid )
        return;
    if ( scope->verboseLevel > 2 )
        qDebug() << "  DsoWidget::updateMarkerDetails()";
    double m1 = scope->horizontal.cursor.pos[ 0 ].x() + DIVS_TIME / 2;
    double m2 = scope->horizontal.cursor.pos[ 1 ].x() + DIVS_TIME / 2;
    if ( m1 > m2 )
        std::swap( m1, m2 );
    double divs = m2 - m1;
    double time0 = m1 * scope->horizontal.timebase;
    double time1 = m2 * scope->horizontal.timebase;
    double time = divs * scope->horizontal.timebase;
    double freq0 = m1 * scope->horizontal.frequencybase;
    double freq1 = m2 * scope->horizontal.frequencybase;
    double freq = divs * scope->horizontal.frequencybase;
    bool timeUsed = false;
    bool freqUsed = false;

    int index = 1;
    for ( ChannelID channel = 0; channel < scope->voltage.size(); ++channel ) {
        if ( scope->voltage[ channel ].used ) {
            timeUsed = true;
            QPointF p0 = scope->voltage[ channel ].cursor.pos[ 0 ];
            QPointF p1 = scope->voltage[ channel ].cursor.pos[ 1 ];
            if ( scope->voltage[ channel ].cursor.shape != DsoSettingsScopeCursor::NONE ) {
                cursorDataGrid->updateInfo(
                    unsigned( index ), true, tr( "ON" ),
                    valueToString( fabs( p1.x() - p0.x() ) * scope->horizontal.timebase, UNIT_SECONDS, 4 ),
                    valueToString( fabs( p1.y() - p0.y() ) * scope->displayGain( channel ), unitFor( channel ), 4 ) );
            } else {
                cursorDataGrid->updateInfo( unsigned( index ), true, tr( "OFF" ), "", "" );
            }
        } else {
            cursorDataGrid->updateInfo( unsigned( index ), false );
        }
        ++index;
    }
    for ( ChannelID channel = 0; channel < scope->spectrum.size(); ++channel ) {
        if ( scope->spectrum[ channel ].used ) {
            freqUsed = true;
            QPointF p0 = scope->spectrum[ channel ].cursor.pos[ 0 ];
            QPointF p1 = scope->spectrum[ channel ].cursor.pos[ 1 ];
            if ( scope->spectrum[ channel ].cursor.shape != DsoSettingsScopeCursor::NONE ) {
                cursorDataGrid->updateInfo(
                    unsigned( index ), true, tr( "ON" ),
                    valueToString( fabs( p1.x() - p0.x() ) * scope->horizontal.frequencybase, UNIT_HERTZ, 4 ),
                    valueToString( fabs( p1.y() - p0.y() ) * scope->spectrum[ channel ].magnitude, UNIT_DECIBEL, 4 ) +
                        scope->analysis.dBsuffix() );
            } else {
                cursorDataGrid->updateInfo( unsigned( index ), true, tr( "OFF" ), "", "" );
            }
        } else {
            cursorDataGrid->updateInfo( unsigned( index ), false );
        }
        ++index;
    }

    if ( divs >= DIVS_TIME || ( m1 <= 0 && m2 <= 0 ) || ( m1 >= DIVS_TIME && m2 >= DIVS_TIME ) ) {
        markerInfoLabel->setVisible( false );
        markerTimeLabel->setVisible( false );
        markerFrequencyLabel->setVisible( false );
        markerTimebaseLabel->setVisible( false );
        markerFrequencybaseLabel->setVisible( false );
    } else {
        markerInfoLabel->setVisible( true );
        markerTimeLabel->setVisible( true );
        markerFrequencyLabel->setVisible( true );
        markerTimebaseLabel->setVisible( view->zoom );
        markerFrequencybaseLabel->setVisible( view->zoom );
        QString mInfo( tr( "Markers  " ) );
        QString mTime( " t: " );
        QString mFreq( " f: " );
        if ( view->zoom ) {
            if ( divs != 0.0 ) {
                zoomFactor = DIVS_TIME / divs;
                mInfo = tr( "Zoom x%1  " ).arg( zoomFactor, -1, 'g', 3 );
            } else {
                zoomFactor = 1000;
                mInfo = tr( "Zoom ---  " );
            }
            markerTimebaseLabel->setText( "  " + valueToString( time / DIVS_TIME, UNIT_SECONDS, 3 ) + tr( "/div" ) );
            markerTimebaseLabel->setVisible( timeUsed );
            markerFrequencybaseLabel->setText( "  " + valueToString( freq / DIVS_TIME, UNIT_HERTZ, 3 ) + tr( "/div" ) );
            markerFrequencybaseLabel->setVisible( freqUsed );
        }
        markerInfoLabel->setText( mInfo );
        if ( timeUsed ) {
            mTime += QString( "%1" ).arg( valueToString( time0, UNIT_SECONDS, 3 ) );
            if ( time > 0 )
                mTime += QString( " ... %1,  Δt: %2 (%3) " )
                             .arg( valueToString( time1, UNIT_SECONDS, 3 ), valueToString( time, UNIT_SECONDS, 3 ),
                                   valueToString( 1 / time, UNIT_HERTZ, 3 ) );
            markerTimeLabel->setText( mTime );
        } else {
            markerTimeLabel->setText( "" );
        }
        if ( freqUsed ) {
            mFreq += QString( "%1" ).arg( valueToString( freq0, UNIT_HERTZ, 3 ) );
            if ( freq > 0 )
                mFreq += QString( " ... %2,  Δf: %3 " )
                             .arg( valueToString( freq1, UNIT_HERTZ, 3 ), valueToString( freq, UNIT_HERTZ, 3 ) );
            markerFrequencyLabel->setText( mFreq );
        } else
            markerFrequencyLabel->setText( "" );
    }

    QString markerMeasureTimeLabel;
    QString markerMeasureFreqLabel;
    if ( timeUsed ) {
        markerMeasureTimeLabel = QString( "Δt: %1" ).arg( valueToString( time, UNIT_SECONDS, 3 ) );
    }
    if ( freqUsed ) {
        markerMeasureFreqLabel = QString( "Δf: %1" ).arg( valueToString( freq, UNIT_HERTZ, 3 ) );
    }
    cursorDataGrid->updateInfo( unsigned( 0 ), true, QString(), markerMeasureTimeLabel, markerMeasureFreqLabel );
}


void DsoWidget::updateSpectrumDetails( ChannelID channel ) {
    setMeasurementVisible( channel );
    if ( scope->spectrum[ channel ].used )
        measurementMagnitudeLabel[ channel ]->setText( valueToString( scope->spectrum[ channel ].magnitude, UNIT_DECIBEL, 3 ) +
                                                       tr( "/div" ) );
    else
        measurementMagnitudeLabel[ channel ]->setText( QString() );
}


void DsoWidget::updateTriggerDetails() {
    QPalette tablePalette = palette();
    tablePalette.setColor( QPalette::WindowText, view->colors->voltage[ unsigned( scope->trigger.source ) ] );
    settingsTriggerLabel->setPalette( tablePalette );
    QString levelString = valueToString( scope->voltage[ unsigned( scope->trigger.source ) ].trigger,
                                         unitFor( ChannelID( scope->trigger.source ) ), 3 );
    QString pretriggerString = valueToString( scope->trigger.position * scope->horizontal.timebase * DIVS_TIME, UNIT_SECONDS, 3 );
    QString pre = Dso::slopeString( scope->trigger.slope );
    QString post = pre;
    if ( scope->trigger.slope == Dso::Slope::Positive )
        post = Dso::slopeString( Dso::Slope::Negative );
    else if ( scope->trigger.slope == Dso::Slope::Negative )
        post = Dso::slopeString( Dso::Slope::Positive );
    QString pulseWidthString = bool( pulseWidth1 ) ? pre + valueToString( pulseWidth1, UNIT_SECONDS, 3 ) + post : "";
    pulseWidthString += bool( pulseWidth2 ) ? valueToString( pulseWidth2, UNIT_SECONDS, 3 ) + pre : "";
    if ( bool( pulseWidth1 ) && bool( pulseWidth2 ) ) {
        int dutyCyle = int( 0.5 + ( 100.0 * pulseWidth1 ) / ( pulseWidth1 + pulseWidth2 ) );
        pulseWidthString += " (" + QString::number( dutyCyle ) + "%)";
    }
    if ( !scope->liveCalibrationActive && scope->trigger.mode != Dso::TriggerMode::ROLL ) {
        settingsTriggerLabel->setText( tr( "%1  %2  %3  %4  %5" )
                                           .arg( scope->voltage[ unsigned( scope->trigger.source ) ].name,
                                                 Dso::slopeString( scope->trigger.slope ), levelString, pretriggerString,
                                                 pulseWidthString ) );
    } else {
        settingsTriggerLabel->setText( "" );
    }
}


void DsoWidget::updateVoltageDetails( ChannelID channel ) {
    if ( channel >= scope->voltage.size() )
        return;

    setMeasurementVisible( channel );

    if ( scope->voltage[ channel ].used )
        // У4б: подпись В/дел показывает ПЕРЕНОРМИРОВАННУЮ сетку. Входные
        // уровни не меняются, увеличено только изображение, поэтому одно
        // деление экрана стоит displayGain, а не physicalGain.
        measurementGainLabel[ channel ]->setText( valueToString( scope->displayGain( channel ), unitFor( channel ), 3 ) +
                                                  tr( "/div" ) + " " );
    else
        measurementGainLabel[ channel ]->setText( QString() );
}


void DsoWidget::updateFrequencybase( double frequencybase ) {
    settingsFrequencybaseLabel->setText( valueToString( frequencybase, UNIT_HERTZ, -1 ) + tr( "/div" ) );
    updateMarkerDetails();
}


void DsoWidget::updateSamplerate( double newSamplerate ) {
    samplerate = newSamplerate;
    scope->horizontal.dotsOnScreen = int( ceil( samplerate * timebase * DIVS_TIME ) );
    settingsSamplerateLabel->setText( valueToString( samplerate, UNIT_SAMPLES, -1 ) + tr( "/s" ) + " " );
}


void DsoWidget::updateOversample( unsigned newOversample ) {
    oversample = newOversample;
    settingsOversampleLabel->setText( tr( "%1x ovr " ).arg( oversample ) );
}


void DsoWidget::updateTimebase( double newTimebase ) {
    timebase = newTimebase;
    scope->horizontal.dotsOnScreen = int( ceil( samplerate * timebase * DIVS_TIME ) );
    settingsTimebaseLabel->setText( valueToString( timebase, UNIT_SECONDS, -1 ) + tr( "/div" ) + " " );
    updateMarkerDetails();
}


void DsoWidget::updateSpectrumMagnitude( ChannelID channel ) {
    updateSpectrumDetails( channel );
    updateMarkerDetails();
}


void DsoWidget::updateSpectrumUsed( ChannelID channel, bool used ) {
    if ( scope->verboseLevel > 2 )
        qDebug() << "  DsoWidget::updateSpectrumUsed()" << channel << used;
    if ( channel >= scope->voltage.size() )
        return;
    bool spectrumUsed = false;
    for ( size_t ch = 0; ch < scope->voltage.size(); ++ch )
        if ( scope->spectrum[ ch ].used )
            spectrumUsed = true;
    settingsFrequencybaseLabel->setVisible( spectrumUsed );
    mainSliders.voltageOffsetSlider->setIndexVisible( unsigned( scope->voltage.size() ) + channel, used );
    zoomSliders.voltageOffsetSlider->setIndexVisible( unsigned( scope->voltage.size() ) + channel, used );

    updateSpectrumDetails( channel );
    updateMarkerDetails();
    if ( !used && selectedCursor == channel + scope->countChannels() + 1 )
        switchToMarker();
}


void DsoWidget::updateTriggerMode() {
    updateTriggerDetails();
    mainSliders.triggerPositionSlider->setVisible( scope->trigger.mode != Dso::TriggerMode::ROLL );
    zoomSliders.triggerPositionSlider->setVisible( zoomScope->isVisible() && scope->trigger.mode != Dso::TriggerMode::ROLL );
}


void DsoWidget::updateTriggerSlope() { updateTriggerDetails(); }


void DsoWidget::updateTriggerSource() {
    mainSliders.triggerPositionSlider->setColor( 0, view->colors->voltage[ unsigned( scope->trigger.source ) ] );
    zoomSliders.triggerPositionSlider->setColor( 0, view->colors->voltage[ unsigned( scope->trigger.source ) ] );

    for ( ChannelID channel = 0; channel < scope->voltage.size(); ++channel ) {
        QColor color = ( channel == unsigned( scope->trigger.source ) ) ? view->colors->voltage[ channel ]
                                                                        : view->colors->voltage[ channel ].darker();
        mainSliders.triggerLevelSlider->setColor( channel, color );
        zoomSliders.triggerLevelSlider->setColor( channel, color );
    }

    updateTriggerDetails();
}


void DsoWidget::updateVoltageCoupling( ChannelID channel ) {
    if ( channel >= scope->voltage.size() )
        return;
    // [MOD] TZ §9.1.3 — append the CtPU unit indicator for physical channels
    // (e.g. "DC [°C]") so the operator sees at a glance that CtPU is active.
    QString text = Dso::couplingString( scope->coupling( channel, spec ) );
    if ( channel < spec->channels && scope->voltage[ channel ].ctpuMode != CtPU::Mode::OFF ) {
        const QString &unit = scope->voltage[ channel ].ctpuUnit;
        if ( !unit.isEmpty() && unit != QStringLiteral( "V" ) )
            text += QStringLiteral( " [%1]" ).arg( unit );
    }
    measurementMiscLabel[ channel ]->setText( text );
}


void DsoWidget::updateMathMode() {
    ChannelID mathChannel = spec->channels;
    // [FIX] TZ §9.4.2 — show formulas for ALL enabled math channels (M1..M4),
    // not just M1. Each math slot gets its own measurementMiscLabel entry.
    // The primary slot (M1 = spec->channels) gets the full "M1=CH1+CH2 [W]" string;
    // additional enabled slots (M2..M4) get appended on separate lines so the
    // operator can see the full math-stack configuration at a glance.
    if ( !scope->mathStack.empty() ) {
        QStringList formulas;
        for ( int i = 0; i < DsoSettingsScope::maxMathChannels; ++i ) {
            const auto &m = scope->mathStack[ i ];
            if ( !m.enabled )
                continue;
            QString opStr;
            switch ( m.op ) {
            case Dso::MathOp::ADD: opStr = QStringLiteral( "+" ); break;
            case Dso::MathOp::SUB: opStr = QStringLiteral( "-" ); break;
            case Dso::MathOp::MUL: opStr = QStringLiteral( "*" ); break;
            case Dso::MathOp::DIV: opStr = QStringLiteral( "/" ); break;
            }
            QString aName = ( m.srcA < scope->voltage.size() ) ? scope->voltage[ m.srcA ].name : QStringLiteral( "?" );
            QString bName = ( m.srcB < scope->voltage.size() ) ? scope->voltage[ m.srcB ].name : QStringLiteral( "?" );
            QString f = QStringLiteral( "M%1=%2%3%4" ).arg( i + 1 ).arg( aName ).arg( opStr ).arg( bName );
            if ( !m.ctpuUnit.isEmpty() && m.ctpuUnit != QStringLiteral( "V" ) )
                f += QStringLiteral( " [%1]" ).arg( m.ctpuUnit );
            formulas << f;
        }
        if ( formulas.isEmpty() ) {
            measurementMiscLabel[ mathChannel ]->setText( tr( "(no math enabled)" ) );
        } else {
            measurementMiscLabel[ mathChannel ]->setText( formulas.join( QStringLiteral( "  " ) ) );
        }
    } else {
        measurementMiscLabel[ mathChannel ]->setText( Dso::mathModeString( Dso::getMathMode( scope->voltage[ mathChannel ] ) ) );
    }
    // [MOD] TZ §9.1.1 (Variant B) — no cache to update anymore; unitFor()
    // resolves mathChannel's unit live from scope->mathStack[0].ctpuUnit.
    updateMarkerDetails();
}


void DsoWidget::updateVoltageGain( ChannelID channel ) {
    if ( channel >= scope->voltage.size() )
        return;
    adaptTriggerLevelSlider( mainSliders, channel );
    adaptTriggerLevelSlider( zoomSliders, channel );
    updateVoltageDetails( channel );

    updateMarkerDetails();
}


void DsoWidget::updateVoltageUsed( ChannelID channel, bool used ) {
    if ( scope->verboseLevel > 2 )
        qDebug() << "  DsoWidget::updateVoltageUsed()" << channel << used;
    if ( channel >= scope->voltage.size() )
        return;

    mainSliders.voltageOffsetSlider->setIndexVisible( channel, used );
    zoomSliders.voltageOffsetSlider->setIndexVisible( channel, used );

    mainSliders.triggerLevelSlider->setIndexVisible( channel, used );
    zoomSliders.triggerLevelSlider->setIndexVisible( channel, used );

    setMeasurementVisible( channel );
    updateVoltageDetails( channel );
    updateMarkerDetails();
    if ( !used && selectedCursor == channel + 1 )
        switchToMarker();
}


void DsoWidget::updateRecordLength( int size ) {
    settingsSamplesOnScreen->setText( valueToString( double( size ), UNIT_SAMPLES, -1 ) + " " + tr( "on screen" ) + " " );
}


void DsoWidget::switchToMarker() {
    cursorDataGrid->selectItem( 0 );
    mainScope->selectCursor( 0 );
    zoomScope->selectCursor( 0 );
}


void DsoWidget::updateZoom( bool enabled ) {
    if ( scope->verboseLevel > 2 )
        qDebug() << "  DsoWidget::updateZoom()" << enabled;
    cursorMeasurementValid = false;
    showCursorMessage();
    mainLayout->setRowStretch( zoomScopeRow, enabled ? int( pow( 2, view->zoomHeightIndex ) ) : 0 );
    zoomScope->setVisible( enabled );
    zoomSliders.voltageOffsetSlider->setVisible( enabled );
    zoomSliders.triggerPositionSlider->setVisible( enabled );
    zoomSliders.triggerLevelSlider->setVisible( enabled );
    markerLayout->setStretch( 3, enabled ? 1 : 0 );
    markerTimebaseLabel->setVisible( enabled );
    markerLayout->setStretch( 4, enabled ? 1 : 0 );
    markerFrequencybaseLabel->setVisible( enabled );
    updateMarkerDetails();
    repaint();
}


void DsoWidget::wheelEvent( QWheelEvent *event ) {
    if ( view->zoom ) {
        if ( event->angleDelta().y() > 0 && view->zoomHeightIndex < 4 ) {
            ++view->zoomHeightIndex;
            updateZoom( true );
        } else if ( event->angleDelta().y() < 0 && view->zoomHeightIndex > 0 ) {
            --view->zoomHeightIndex;
            updateZoom( true );
        }
    }
    event->accept();
}


/// \brief Prints analyzed data.
/// \param analysedData The post processed data from the data analyzer.
void DsoWidget::ingestNew( std::shared_ptr< PPresult > analysedData ) {
    if ( !analysedData )
        return;
    // Data-bearing consumption only (D-05): every frame must reach the
    // recorders — the display may be coalesced (mailbox-1 in MainWindow),
    // recorded data may not. Keep this cheap: decimation, no widgets.
    if ( scope->horizontal.format == Dso::GraphFormat::XY && scope->horizontal.xyContinuous ) {
        // [MOD] TZ §7.4 — feed the frame into every enabled curve's recorder.
        // Each recorder maintains its own cascade decimation pipeline; the
        // (xChannel, yChannel) pair for each curve comes from scope->xyCurves.
        // Curve 0 also feeds the legacy m_xyRecorder for export compatibility.
        for ( int i = 0; i < DsoSettingsScope::maxXYCurves; ++i ) {
            const auto &cfg = scope->xyCurves[ i ];
            if ( !cfg.enabled )
                continue;
            XYRecorder *rec = ( i == 0 ) ? &m_xyRecorder : &m_xyRecorders[ i ];
            // [FIX] TZ §7.4 — bind this recorder to its curve config BEFORE
            // addFrame(). Without this, addFrame() defaults to (0,1) and all
            // four recorders record the same CH1×CH2 trajectory regardless
            // of the user's per-curve channel selection.
            rec->setCurveConfig( cfg );
            rec->addFrame( analysedData.get() );
        }
    }
}


void DsoWidget::showNew( std::shared_ptr< PPresult > analysedData ) {
    if ( scope->verboseLevel > 4 )
        qDebug() << "    DsoWidget::showNew()" << analysedData->tag;

    // D-05 HUD metric: how old is what the operator is looking at.
    if ( analysedData->captureTimestampMs > 0 ) {
        qint64 ageMs = QDateTime::currentMSecsSinceEpoch() - analysedData->captureTimestampMs;
        frameAgeLabel->setText( tr( "%1 ms" ).arg( ageMs ) );
        frameAgeLabel->setVisible( true );
    } else {
        frameAgeLabel->setVisible( false );
    }

    // ============================================================
    // XY CONTINUOUS RECORDER MODE (TZ §7 — multi-curve)
    // ============================================================
    if ( scope->horizontal.format == Dso::GraphFormat::XY && scope->horizontal.xyContinuous ) {
        // Recorders were already fed by ingestNew() — here we only read
        // their current state for display.
        std::size_t totalPoints = 0;
        std::array< const XYRecorder *, DsoSettingsScope::maxXYCurves > recorderPtrs{};
        std::array< const XYCurveConfig *, DsoSettingsScope::maxXYCurves > configPtrs{};
        for ( int i = 0; i < DsoSettingsScope::maxXYCurves; ++i ) {
            const auto &cfg = scope->xyCurves[ i ];
            if ( !cfg.enabled )
                continue;
            XYRecorder *rec = ( i == 0 ) ? &m_xyRecorder : &m_xyRecorders[ i ];
            recorderPtrs[ i ] = rec;
            configPtrs[ i ] = &cfg;
            totalPoints += rec->size();
        }

        // Update scopes with all active XY trajectories.
        mainScope->updateXY( recorderPtrs, configPtrs );
        zoomScope->updateXY( recorderPtrs, configPtrs );

        // Show XY REC status (blue background)
        swTriggerStatus->setText( tr( "<b> XY REC </b>" ) );
        QPalette triggerLabelPalette = palette();
        triggerLabelPalette.setColor( QPalette::WindowText, Qt::black );
        triggerLabelPalette.setColor( QPalette::Window, QColor( 0, 128, 255 ) );
        swTriggerStatus->setPalette( triggerLabelPalette );
        swTriggerStatus->setVisible( true );

        // Update display with trajectory info
        settingsSamplesOnScreen->setText( tr( "%1 XY pts" ).arg( totalPoints ) );
        settingsSamplerateLabel->setText( valueToString( scope->horizontal.samplerate, UNIT_SAMPLES, -1 ) + tr( "/s" ) + " " );
        settingsTimebaseLabel->setVisible( false );
        settingsOversampleLabel->setVisible( false );
        settingsFrequencybaseLabel->setVisible( false );

        // Minimal measurement updates from current frame statistics
        const DataChannel *ch1 = analysedData->data( 0 );
        if ( ch1 && scope->voltage[ 0 ].used ) {
            measurementVppLabel[ 0 ]->setText( valueToString( ch1->vmax - ch1->vmin, ch1->physicalUnit, 3 ) + tr( "pp" ) );
            measurementDCLabel[ 0 ]->setText( valueToString( ch1->dc, ch1->physicalUnit, 3 ) + "=" );
            measurementACLabel[ 0 ]->setText( valueToString( ch1->ac, ch1->physicalUnit, 3 ) + "~" );
            measurementRMSLabel[ 0 ]->setText( valueToString( ch1->rms, ch1->physicalUnit, 3 ) + tr( "rms" ) );
            measurementdBLabel[ 0 ]->setText( valueToString( ch1->dB, UNIT_DECIBEL, 3 ) + scope->analysis.dBsuffix() );
            measurementFrequencyLabel[ 0 ]->setText( valueToString( ch1->frequency, UNIT_HERTZ, 4 ) );
        }
        const DataChannel *ch2 = analysedData->data( 1 );
        if ( ch2 && scope->voltage[ 1 ].used ) {
            measurementVppLabel[ 1 ]->setText( valueToString( ch2->vmax - ch2->vmin, ch2->physicalUnit, 3 ) + tr( "pp" ) );
            measurementDCLabel[ 1 ]->setText( valueToString( ch2->dc, ch2->physicalUnit, 3 ) + "=" );
            measurementACLabel[ 1 ]->setText( valueToString( ch2->ac, ch2->physicalUnit, 3 ) + "~" );
            measurementRMSLabel[ 1 ]->setText( valueToString( ch2->rms, ch2->physicalUnit, 3 ) + tr( "rms" ) );
            measurementdBLabel[ 1 ]->setText( valueToString( ch2->dB, UNIT_DECIBEL, 3 ) + scope->analysis.dBsuffix() );
            measurementFrequencyLabel[ 1 ]->setText( valueToString( ch2->frequency, UNIT_HERTZ, 4 ) );
        }

        // Highlight clipped channels
        for ( ChannelID channel = 0; channel < scope->voltage.size(); ++channel ) {
            const DataChannel *data = analysedData->data( channel );
            if ( data ) {
                QPalette validPalette;
                if ( data->valid ) {
                    validPalette.setColor( QPalette::WindowText, view->colors->voltage[ channel ] );
                    validPalette.setColor( QPalette::Window, view->colors->background );
                } else {
                    validPalette.setColor( QPalette::WindowText, Qt::black );
                    validPalette.setColor( QPalette::Window, Qt::red );
                }
                measurementNameLabel[ channel ]->setPalette( validPalette );
            }
        }

        return; // Skip normal rendering entirely
    }
    // ============================================================
    // END XY CONTINUOUS MODE
    // ============================================================

    // STANDARD OSCILLOSCOPE MODE (original code)
    mainScope->showData( analysedData );
    zoomScope->showData( analysedData );

    QPalette triggerLabelPalette = palette();
    if ( scope->liveCalibrationActive ) {
        swTriggerStatus->setText( tr( "<b> OFFSET CALIBRATION </b>" ) );
        triggerLabelPalette.setColor( QPalette::WindowText, Qt::black );
        triggerLabelPalette.setColor( QPalette::Window, Qt::red );
        swTriggerStatus->setPalette( triggerLabelPalette );
        swTriggerStatus->setVisible( true );
    } else if ( scope->trigger.mode == Dso::TriggerMode::ROLL ) {
        swTriggerStatus->setVisible( false );
    } else {
        swTriggerStatus->setText( tr( "TR" ) );
        triggerLabelPalette.setColor( QPalette::WindowText, Qt::black );
        triggerLabelPalette.setColor( QPalette::Window, analysedData->softwareTriggerTriggered ? Qt::green : Qt::red );
        swTriggerStatus->setPalette( triggerLabelPalette );
        swTriggerStatus->setVisible( true );
    }
    const size_t CH1 = 0;
    updateRecordLength( scope->horizontal.dotsOnScreen );
    pulseWidth1 = analysedData.get()->data( CH1 )->pulseWidth1;
    pulseWidth2 = analysedData.get()->data( CH1 )->pulseWidth2;
    // [MOD] TZ §9.1.1 (Variant B) — no cache to prime anymore. The per-channel
    // loop below already holds `data = analysedData.get()->data(channel)` and
    // reads `data->physicalUnit` directly; everywhere outside this loop
    // (trigger/gain/cursor labels) uses unitFor(channel), which reads
    // scope->voltage/scope->mathStack live.
    updateTriggerDetails();

    QString uStr;
    QString mStr;
    double uCursor = INT_MIN;
    double mCursor = INT_MIN;
    bool uVisible = false;
    bool mVisible = false;

    for ( ChannelID channel = 0; channel < scope->voltage.size(); ++channel ) {
        if ( ( scope->voltage[ channel ].used || scope->spectrum[ channel ].used ) && analysedData.get()->data( channel ) ) {
            const DataChannel *data = analysedData.get()->data( channel );
            if ( cursorMeasurementValid ) {
                uCursor = ( cursorMeasurementPosition.y() - scope->voltage[ channel ].offset ) * scope->displayGain( channel );
                mCursor =
                    ( cursorMeasurementPosition.y() - scope->spectrum[ channel ].offset ) * scope->spectrum[ channel ].magnitude;
                if ( scope->voltage[ channel ].visible ) {
                    uVisible = true;
                    if ( uCursor > data->vmin - 0.2 * scope->displayGain( channel ) &&
                         uCursor <= data->vmax + 0.2 * scope->displayGain( channel ) )
                        uStr += '\t' + scope->voltage[ channel ].name + ": " + valueToString( uCursor, data->physicalUnit, 3 );
                }
                if ( scope->spectrum[ channel ].visible ) {
                    mVisible = true;
                    if ( mCursor > data->dBmin - 0.2 * scope->spectrum[ channel ].magnitude &&
                         mCursor <= data->dBmax + 0.2 * scope->spectrum[ channel ].magnitude )
                        mStr += '\t' + scope->spectrum[ channel ].name + ": " + valueToString( mCursor, UNIT_DECIBEL, 3 ) +
                                scope->analysis.dBsuffix();
                }
            }
            measurementVppLabel[ channel ]->setText( valueToString( data->vmax - data->vmin, data->physicalUnit, 3 ) + tr( "pp" ) );
            measurementDCLabel[ channel ]->setText( valueToString( data->dc, data->physicalUnit, 3 ) + "=" );
            measurementACLabel[ channel ]->setText( valueToString( data->ac, data->physicalUnit, 3 ) + "~" );
            measurementRMSLabel[ channel ]->setText( valueToString( data->rms, data->physicalUnit, 3 ) + tr( "rms" ) );
            measurementdBLabel[ channel ]->setText( valueToString( data->dB, UNIT_DECIBEL, 3 ) + scope->analysis.dBsuffix() );
            measurementFrequencyLabel[ channel ]->setText( valueToString( data->frequency, UNIT_HERTZ, 4 ) );
            if ( scope->analysis.showNoteValue ) {
                measurementLayout->setColumnStretch( 12, 3 );
                measurementNoteLabel[ channel ]->setText( data->note );
            } else {
                measurementNoteLabel[ channel ]->setText( "" );
                measurementLayout->setColumnStretch( 12, 0 );
            }
            if ( scope->analysis.calculateDummyLoad && scope->analysis.dummyLoad > 0 ) {
                measurementLayout->setColumnStretch( 9, 3 );
                measurementRMSPowerLabel[ channel ]->setText(
                    valueToString( ( data->rms * data->rms ) / scope->analysis.dummyLoad, UNIT_WATTS, 3 ) );
            } else {
                measurementRMSPowerLabel[ channel ]->setText( "" );
                measurementLayout->setColumnStretch( 9, 0 );
            }
            if ( scope->analysis.calculateTHD ) {
                double thd = data->thd;
                measurementLayout->setColumnStretch( 10, 2 );
                if ( thd > 0 )
                    measurementTHDLabel[ channel ]->setText( QString( "%1%" ).arg( thd * 100, 4, 'f', thd < 1 ? 1 : 0 ) );
                else
                    measurementTHDLabel[ channel ]->setText( "" );
            } else {
                measurementTHDLabel[ channel ]->setText( "" );
                measurementLayout->setColumnStretch( 10, 0 );
            }
        }

        // [FIX] Guard against a null DataChannel* — this used to be an
        // unconditional dereference (unlike the XY-continuous branch above,
        // which already null-checks `ch1`/`ch2`). A channel that has never
        // been populated (e.g. a disabled math channel) can legitimately
        // return nullptr from data().
        const DataChannel *dataForPalette = analysedData.get()->data( channel );
        QPalette validPalette;
        if ( dataForPalette && dataForPalette->valid ) {
            validPalette.setColor( QPalette::WindowText, view->colors->voltage[ channel ] );
            validPalette.setColor( QPalette::Window, view->colors->background );
        } else {
            validPalette.setColor( QPalette::WindowText, Qt::black );
            validPalette.setColor( QPalette::Window, Qt::red );
        }
        measurementNameLabel[ channel ]->setPalette( validPalette );
    }

    if ( cursorMeasurementValid ) {
        QString measurement;
        if ( uVisible && ( !uStr.isEmpty() || ( uStr.isEmpty() && mStr.isEmpty() ) ) ) {
            measurement +=
                valueToString( ( cursorMeasurementPosition.x() + DIVS_TIME / 2.0 ) * scope->horizontal.timebase, UNIT_SECONDS, 3 );
            measurement += '\t' + uStr;
        }
        if ( mVisible && ( !mStr.isEmpty() || ( uStr.isEmpty() && mStr.isEmpty() ) ) ) {
            if ( !measurement.isEmpty() )
                measurement += '\n';
            measurement += valueToString( ( cursorMeasurementPosition.x() + DIVS_TIME / 2.0 ) * scope->horizontal.frequencybase,
                                          UNIT_HERTZ, 3 );
            measurement += '\t' + mStr;
        }
        if ( !measurement.isEmpty() ) {
            showCursorMessage( cursorGlobalPosition, measurement );
        }
    }
}


void DsoWidget::showCursorMessage( QPoint globalPos, const QString &message ) {
    if ( scope->verboseLevel > 3 )
        qDebug() << "   DsoWidget::showCursorMessage()" << globalPos << message;
    QToolTip::showText( globalPos, message );
}


void DsoWidget::showEvent( QShowEvent *event ) {
    QWidget::showEvent( event );
    updateTriggerDetails();
    updateRecordLength( scope->horizontal.recordLength );
    updateFrequencybase( scope->horizontal.frequencybase );
    updateSamplerate( scope->horizontal.samplerate );
    updateOversample( oversample );
    updateTimebase( scope->horizontal.timebase );
    updateZoom( view->zoom );

    updateTriggerSource();
    adaptTriggerPositionSlider();
}


/// \brief Applies recorder configuration gathered by HorizontalDock right
/// before it starts continuous XY acquisition. Must run before the next
/// showNew()/addFrame() call - HorizontalDock emits this synchronously from
/// its "XY Recorder" checkbox handler, ahead of xyContinuousChanged(true).
/// [MOD] TZ §7.4 — configures all `maxXYCurves` recorders (curve 0 is the
/// legacy m_xyRecorder slot, curves 1..3 live in m_xyRecorders[]).
void DsoWidget::configureXYRecorder( XYRecorder::Config cfg ) {
    if ( scope->verboseLevel > 2 )
        qDebug() << "  DsoWidget::configureXYRecorder()";
    // [FIX] TZ §7.4 — bind each recorder to its curve config from scope->xyCurves
    // BEFORE configure(), so that writeHeader() and the tape column header use
    // the correct channel names. showNew() also calls setCurveConfig() on every
    // frame, but configure() opens the tape file and writes the header, so the
    // binding must be in place here.
    if ( !scope->xyCurves.empty() )
        m_xyRecorder.setCurveConfig( scope->xyCurves[ 0 ] );
    m_xyRecorder.configure( scope, spec, cfg );
    if ( cfg.sheetMode == XYRecorder::SheetMode::TAPE && !cfg.tapeFilePath.isEmpty() &&
         !m_xyRecorder.isStreamingToDisk() )
        qDebug() << "  DsoWidget::configureXYRecorder() failed to open tape file, falling back to bounded RAM:"
                  << cfg.tapeFilePath;
    // Apply the same config to the additional recorders. Each curve will
    // stream to its own tape file (path suffix _curveN) so multi-curve TAPE
    // recordings don't collide on disk.
    for ( int i = 1; i < DsoSettingsScope::maxXYCurves; ++i ) {
        XYRecorder::Config cfgI = cfg;
        if ( cfgI.sheetMode == XYRecorder::SheetMode::TAPE && !cfgI.tapeFilePath.isEmpty() ) {
            // Insert curve suffix before the file extension.
            const QString &path = cfgI.tapeFilePath;
            int dot = path.lastIndexOf( QLatin1Char( '.' ) );
            cfgI.tapeFilePath = ( dot > 0 ? path.left( dot ) : path ) + QStringLiteral( "_curve%1" ).arg( i )
                                + ( dot > 0 ? path.mid( dot ) : QString() );
        }
        if ( i < int( scope->xyCurves.size() ) )
            m_xyRecorders[ i ].setCurveConfig( scope->xyCurves[ i ] );
        m_xyRecorders[ i ].configure( scope, spec, cfgI );
    }
}


/// \brief XY Continuous mode toggled.
void DsoWidget::updateXYContinuous( bool enabled ) {
    if ( scope->verboseLevel > 2 )
        qDebug() << "  DsoWidget::updateXYContinuous()" << enabled;
    if ( !enabled ) {
        // finalize() BEFORE clear(): flushes whatever's still buffered to
        // the tape file (if streaming) and closes it. clear() alone would
        // silently drop that tail. Apply to all curve recorders (TZ §7.4).
        m_xyRecorder.finalize();
        m_xyRecorder.clear();
        for ( int i = 1; i < DsoSettingsScope::maxXYCurves; ++i ) {
            m_xyRecorders[ i ].finalize();
            m_xyRecorders[ i ].clear();
        }
    }
    // Restore standard labels when leaving XY mode
    if ( !enabled || scope->horizontal.format != Dso::GraphFormat::XY ) {
        settingsTimebaseLabel->setVisible( true );
        settingsOversampleLabel->setVisible( true );
        settingsFrequencybaseLabel->setVisible( true );
    }
    mainScope->update();
    zoomScope->update();
}


void DsoWidget::updateOffset( ChannelID channel, double value, bool pressed, QPoint globalPos ) {
    if ( channel < scope->voltage.size() ) {
        scope->voltage[ channel ].offset = value;
        adaptTriggerLevelSlider( mainSliders, channel );
        adaptTriggerLevelSlider( zoomSliders, channel );
    } else if ( channel < scope->voltage.size() * 2 )
        scope->spectrum[ channel - scope->voltage.size() ].offset = value;
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wfloat-equal"
#endif
    if ( channel < scope->voltage.size() * 2 ) {
        if ( mainSliders.voltageOffsetSlider->value( int( channel ) ) != value ) {
            const QSignalBlocker blocker( mainSliders.voltageOffsetSlider );
            mainSliders.voltageOffsetSlider->setValue( int( channel ), value );
        }
        if ( zoomSliders.voltageOffsetSlider->value( int( channel ) ) != value ) {
            const QSignalBlocker blocker( zoomSliders.voltageOffsetSlider );
            zoomSliders.voltageOffsetSlider->setValue( int( channel ), value );
        }
    }
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
    if ( scope->verboseLevel > 2 )
        qDebug() << "  DsoWidget::updateOffset()" << channel << value << pressed << globalPos;

    emit voltageOffsetChanged( channel, value );
}


double DsoWidget::mainToZoom( double position ) const {
    double m1 = scope->getMarker( 0 );
    double m2 = scope->getMarker( 1 );
    if ( m1 > m2 )
        std::swap( m1, m2 );
    return ( ( position - 0.5 ) * DIVS_TIME - m1 ) / std::max( m2 - m1, 1e-9 );
}


double DsoWidget::zoomToMain( double position ) const {
    double m1 = scope->getMarker( 0 );
    double m2 = scope->getMarker( 1 );
    if ( m1 > m2 )
        std::swap( m1, m2 );
    return 0.5 + ( m1 + position * ( m2 - m1 ) ) / DIVS_TIME;
}


void DsoWidget::adaptTriggerPositionSlider() {
    double value = mainToZoom( scope->trigger.position );

    LevelSlider &slider = *zoomSliders.triggerPositionSlider;
    const QSignalBlocker blocker( slider );
    if ( slider.minimum( 0 ) <= value && value <= slider.maximum( 0 ) ) {
        slider.setEnabled( true );
        slider.setValue( 0, value );
    } else {
        slider.setEnabled( false );
        if ( value < slider.minimum( 0 ) ) {
            slider.setValue( 0, slider.minimum( 0 ) );
        } else {
            slider.setValue( 0, slider.maximum( 0 ) );
        }
    }
}


void DsoWidget::updateTriggerPosition( int index, double value, bool pressed, QPoint globalPos, bool mainView ) {
    if ( index != 0 )
        return;

    if ( mainView ) {
        scope->trigger.position = value;
        adaptTriggerPositionSlider();
    } else {
        scope->trigger.position = zoomToMain( value );
        const QSignalBlocker blocker( mainSliders.triggerPositionSlider );
        mainSliders.triggerPositionSlider->setValue( index, scope->trigger.position );
    }

    updateTriggerDetails();
    updateMarkerDetails();
    if ( scope->verboseLevel > 2 )
        qDebug() << "  DsoWidget::updateTriggerPosition()" << index << scope->trigger.position << pressed << globalPos;
    emit triggerPositionChanged( scope->trigger.position );
    if ( pressed ) {
        int resolution = 3;
        if ( !mainView )
            resolution += int( log10( zoomFactor ) );
        showCursorMessage( globalPos, valueToString( scope->trigger.position * scope->horizontal.timebase * DIVS_TIME, UNIT_SECONDS,
                                                     resolution ) );
    } else
        showCursorMessage();
}


void DsoWidget::updateTriggerLevel( ChannelID channel, double value, bool pressed, QPoint globalPos ) {
    scope->voltage[ channel ].trigger = value;
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wfloat-equal"
#endif
    if ( mainSliders.triggerLevelSlider->value( int( channel ) ) != value ) {
        const QSignalBlocker blocker( mainSliders.triggerLevelSlider );
        mainSliders.triggerLevelSlider->setValue( int( channel ), value );
    }
    if ( zoomSliders.triggerLevelSlider->value( int( channel ) ) != value ) {
        const QSignalBlocker blocker( zoomSliders.triggerLevelSlider );
        zoomSliders.triggerLevelSlider->setValue( int( channel ), value );
    }
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
    updateTriggerDetails();
    if ( scope->verboseLevel > 2 )
        qDebug() << "  DsoWidget::updateTriggerValue()" << channel << value << pressed << globalPos;
    emit triggerLevelChanged( channel, value );
    if ( pressed )
        showCursorMessage( globalPos, valueToString( value, unitFor( channel ), 3 ) );
    else
        showCursorMessage();
}


void DsoWidget::updateMarker( unsigned marker, double value ) {
    if ( scope->verboseLevel > 3 )
        qDebug() << "   DsoWidget::updateMarker()" << marker << value;
    scope->setMarker( marker, value );
    adaptTriggerPositionSlider();
    updateMarkerDetails();
}


void DsoWidget::updateSlidersSettings() {
    if ( scope->verboseLevel > 2 )
        qDebug() << "  DsoWidget::updateSlidersSettings()";
    for ( ChannelID channel = 0; channel < scope->voltage.size(); ++channel ) {
        updateOffset( channel, scope->voltage[ channel ].offset, false, QPoint() );
        mainSliders.voltageOffsetSlider->setColor( ( channel ), view->colors->voltage[ channel ] );
        mainSliders.voltageOffsetSlider->setValue( int( channel ), scope->voltage[ channel ].offset );
        mainSliders.voltageOffsetSlider->setIndexVisible( channel, scope->voltage[ channel ].used );
        zoomSliders.voltageOffsetSlider->setColor( ( channel ), view->colors->voltage[ channel ] );
        zoomSliders.voltageOffsetSlider->setValue( int( channel ), scope->voltage[ channel ].offset );
        zoomSliders.voltageOffsetSlider->setIndexVisible( channel, scope->voltage[ channel ].used );

        updateOffset( unsigned( scope->voltage.size() + channel ), scope->spectrum[ channel ].offset, false, QPoint() );
        mainSliders.voltageOffsetSlider->setColor( unsigned( scope->voltage.size() ) + channel, view->colors->spectrum[ channel ] );
        mainSliders.voltageOffsetSlider->setValue( int( scope->voltage.size() + channel ), scope->spectrum[ channel ].offset );
        mainSliders.voltageOffsetSlider->setIndexVisible( unsigned( scope->voltage.size() ) + channel,
                                                          scope->spectrum[ channel ].used );
        zoomSliders.voltageOffsetSlider->setColor( unsigned( scope->voltage.size() ) + channel, view->colors->spectrum[ channel ] );
        zoomSliders.voltageOffsetSlider->setValue( int( scope->voltage.size() + channel ), scope->spectrum[ channel ].offset );
        zoomSliders.voltageOffsetSlider->setIndexVisible( unsigned( scope->voltage.size() ) + channel,
                                                          scope->spectrum[ channel ].used );
    }

    mainSliders.triggerPositionSlider->setValue( 0, scope->trigger.position );
    updateTriggerPosition( 0, scope->trigger.position, false, QPoint(), true );
    updateTriggerPosition( 0, mainToZoom( scope->trigger.position ), false, QPoint(), false );

    for ( ChannelID channel = 0; channel < scope->voltage.size(); ++channel ) {
        mainSliders.triggerLevelSlider->setValue( int( channel ), scope->voltage[ channel ].trigger );
        adaptTriggerLevelSlider( mainSliders, channel );
        mainSliders.triggerLevelSlider->setColor( channel, ( channel == unsigned( scope->trigger.source ) )
                                                               ? view->colors->voltage[ channel ]
                                                               : view->colors->voltage[ channel ].darker() );
        mainSliders.triggerLevelSlider->setIndexVisible( channel, scope->voltage[ channel ].used );
        zoomSliders.triggerLevelSlider->setValue( int( channel ), scope->voltage[ channel ].trigger );
        adaptTriggerLevelSlider( zoomSliders, channel );
        zoomSliders.triggerLevelSlider->setColor( channel, ( channel == unsigned( scope->trigger.source ) )
                                                               ? view->colors->voltage[ channel ]
                                                               : view->colors->voltage[ channel ].darker() );
        zoomSliders.triggerLevelSlider->setIndexVisible( channel, scope->voltage[ channel ].used );
    }
    updateTriggerDetails();

    for ( int marker = 0; marker < 2; ++marker ) {
        mainSliders.markerSlider->setValue( marker, scope->horizontal.cursor.pos[ marker ].x() );
    }
    updateMarkerDetails();
}


/// Привести слой камеры к настройкам вида.
///
/// Перезапуск, а не «подкрутка на ходу»: смена прибора есть смена источника, и
/// делать её незаметно нельзя. Прозрачность применится сама - её читает
/// `GlScope::drawCameraLayer()` на каждой отрисовке.
void DsoWidget::applyCameraSettings() {
    // Ориентация задаётся до всего прочего и живёт в слое: там же, где кадр,
    // и потому одинаково для холста и для снимка «фото камеры».
    cameraLayer->setOrientation( view->cameraRotation, view->cameraMirrorH, view->cameraMirrorV );
    const bool wanted = view->cameraLayerEnabled && !view->cameraDeviceId.isEmpty();
    if ( !wanted ) {
        cameraLayer->stop();
        mainScope->setCameraFrame( QImage() ); // слоя нет - и картинки нет
    } else if ( cameraLayer->currentDeviceId() != view->cameraDeviceId ) {
        cameraLayer->start( view->cameraDeviceId );
    }
    mainScope->update();
}


/// Чистый кадр камеры: то, что дала камера, без нашей графики.
QImage DsoWidget::cameraPhoto() const { return cameraLayer->frame(); }


QString DsoWidget::cameraLayerError() const { return mainScope ? mainScope->cameraLayerError() : QString(); }


/// Холст целиком - фон, кадр камеры, сетка, кривые - в порядке слоёв экрана.
///
/// Берём буфер самого GL-виджета, а не рисуем сцену заново: снимок обязан
/// совпадать с тем, что оператор видел в момент нажатия, иначе это уже другая
/// картина.
QImage DsoWidget::canvasWithCamera() { return mainScope->grabFramebuffer(); }
