// SPDX-License-Identifier: GPL-2.0-or-later

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDebug>
#include <QDockWidget>
#include <QLabel>
#include <QSignalBlocker>

#include <cmath>

#include "TriggerDock.h"
#include "dockwindows.h"

#include "dsosettings.h"
#include "hantekdso/controlspecification.h"
#include "sispinbox.h"
#include "utils/printutils.h"


TriggerDock::TriggerDock( DsoSettingsScope *scope, const Dso::ControlSpecification *spec, QWidget *parent )
    : QDockWidget( tr( "Trigger" ), parent ), scope( scope ), mSpec( spec ) {

    if ( scope->verboseLevel > 1 )
        qDebug() << " TriggerDock::TriggerDock()";

    // Initialize lists for comboboxes
    // [MOD] TZ §5.6.2 — populate trigger source list dynamically from
    // scope->voltage.size() (real channels + up to 4 math channels).
    // Names: CH1, CH2, M1, M2, M3, M4. The first `mSpec->channels` entries
    // are real hardware channels; the rest are math-stack virtual channels.
    for ( ChannelID channel = 0; channel < mSpec->channels; ++channel )
        sourceStandardStrings << tr( "CH%1" ).arg( channel + 1 );
    for ( int i = 0; i < DsoSettingsScope::maxMathChannels; ++i )
        sourceStandardStrings << tr( "M%1" ).arg( i + 1 );
    // add "smooth" source
    smoothStandardStrings << tr( "HF" ) << tr( "Normal" ) << tr( "LF" );

    // Initialize elements
    modeLabel = new QLabel( tr( "Mode" ) );
    modeComboBox = new QComboBox();
    if ( scope->toolTipVisible )
        modeComboBox->setToolTip( tr( "Select the trigger mode" ) );
    // Режим кладётся в данные пункта: прежде видимость ROLL держалась на
    // арифметике «спрятать последний», и добавление любого режима после него
    // прятало бы уже не ROLL. Задание 9 добавило FREERUN — по прежнему правилу
    // спрятался бы именно он, то есть ровно тот режим, который обязан быть
    // виден всегда.
    for ( Dso::TriggerMode mode : mSpec->triggerModes )
        modeComboBox->addItem( Dso::triggerModeString( mode ), QVariant::fromValue( int( mode ) ) );

    slopeLabel = new QLabel( tr( "Slope" ) );
    slopeComboBox = new QComboBox();
    if ( scope->toolTipVisible )
        slopeComboBox->setToolTip( tr( "Select positive, negative or both (alternating) slopes" ) );
    for ( Dso::Slope slope : Dso::SlopeEnum )
        slopeComboBox->addItem( Dso::slopeString( slope ) );

    sourceLabel = new QLabel( tr( "Source" ) );
    sourceComboBox = new QComboBox();
    if ( scope->toolTipVisible )
        sourceComboBox->setToolTip( tr( "Select the trigger channel (CH1, CH2, or MATH)" ) );
    sourceComboBox->addItems( sourceStandardStrings );
    smoothComboBox = new QComboBox();
    if ( scope->toolTipVisible )
        smoothComboBox->setToolTip( tr( "Trigger on fast, normal, or slow signals" ) );
    smoothComboBox->addItems( smoothStandardStrings );

    dockLayout = new QGridLayout();
    dockLayout->setColumnMinimumWidth( 0, 50 );
    dockLayout->setColumnStretch( 1, 1 ); // stretch 2nd (middle) column 1x
    dockLayout->setColumnStretch( 2, 2 ); // stretch 3rd (last) column 2x
    dockLayout->setSpacing( DOCK_LAYOUT_SPACING );
    dockLayout->addWidget( modeLabel, 0, 0 );
    dockLayout->addWidget( modeComboBox, 0, 1, 1, 2 ); // fill 1 row, 2 col
    dockLayout->addWidget( sourceLabel, 1, 0 );
    dockLayout->addWidget( sourceComboBox, 1, 1, 1, 2 ); // fill 1 row, 2 col
    dockLayout->addWidget( slopeLabel, 2, 0 );
    dockLayout->addWidget( slopeComboBox, 2, 1 );
    dockLayout->addWidget( smoothComboBox, 2, 2 );

    dockWidget = new QWidget();
    SetupDockWidget( this, dockWidget, dockLayout );

    // Load settings into GUI
    loadSettings( scope );

    // Connect signals and slots
    // Режим берётся из ДАННЫХ пункта, а не из его номера: пункты появляются и
    // исчезают (ROLL), и номер перестаёт совпадать со значением. Прежняя
    // индексная выборка при скрытом ROLL выдавала бы соседний режим.
    connect( modeComboBox, static_cast< void ( QComboBox::* )( int ) >( &QComboBox::currentIndexChanged ), this,
             [ this ]( int index ) {
                 if ( index < 0 )
                     return;
                 this->scope->trigger.mode = Dso::TriggerMode( modeComboBox->itemData( index ).toInt() );
                 emit modeChanged( this->scope->trigger.mode );
             } );
    connect( slopeComboBox, static_cast< void ( QComboBox::* )( int ) >( &QComboBox::currentIndexChanged ), this,
             [ this ]( int index ) {
                 this->scope->trigger.slope = Dso::Slope( index );
                 emit slopeChanged( this->scope->trigger.slope );
             } );
    connect( sourceComboBox, static_cast< void ( QComboBox::* )( int ) >( &QComboBox::currentIndexChanged ), this,
             [ this ]( int index ) {
                 this->scope->trigger.source = index;
                 emit sourceChanged( index );
             } );
    connect( smoothComboBox, static_cast< void ( QComboBox::* )( int ) >( &QComboBox::currentIndexChanged ), this,
             [ this ]( int index ) {
                 this->scope->trigger.smooth = index;
                 emit smoothChanged( index );
             } );
}

void TriggerDock::loadSettings( DsoSettingsScope *scope ) {
    if ( scope->verboseLevel > 2 )
        qDebug() << "  TDock::loadSettings()";
    // Set values
    if ( scope->horizontal.timebase < 0.2 ) // ROLL — только на медленных развёртках
        setModeAvailable( Dso::TriggerMode::ROLL, false );
    setMode( scope->trigger.mode );
    setSlope( scope->trigger.slope );
    setSource( scope->trigger.source );
    setSmooth( scope->trigger.smooth );
}


void TriggerDock::timebaseChanged( double timebase ) { // provide ROLL mode only if samplerate > 100 ms/div
    if ( scope->trigger.mode == Dso::TriggerMode::ROLL )
        return;
    // ROLL осмыслен только на медленных развёртках и потому появляется и
    // исчезает. FREERUN доступен ВСЕГДА: он не про скорость развёртки, а про
    // то, что уровень не ищется вовсе (задание 9 очереди).
    setModeAvailable( Dso::TriggerMode::ROLL, timebase > 0.1 );
}


/// Показать или спрятать один режим по его значению, не трогая остальные.
void TriggerDock::setModeAvailable( Dso::TriggerMode mode, bool available ) {
    const int idx = modeComboBox->findData( QVariant::fromValue( int( mode ) ) );
    if ( available ) {
        if ( idx < 0 ) {
            // Возвращается на своё место в порядке спецификации, а не в конец.
            int insertAt = modeComboBox->count();
            for ( int i = 0; i < modeComboBox->count(); ++i )
                if ( modeComboBox->itemData( i ).toInt() > int( mode ) ) {
                    insertAt = i;
                    break;
                }
            modeComboBox->insertItem( insertAt, Dso::triggerModeString( mode ), QVariant::fromValue( int( mode ) ) );
        }
    } else if ( idx >= 0 && scope->trigger.mode != mode ) {
        modeComboBox->removeItem( idx );
    }
}


/// \brief Don't close the dock, just hide it
/// \param event The close event that should be handled.
void TriggerDock::closeEvent( QCloseEvent *event ) {
    hide();

    event->accept();
}

void TriggerDock::setMode( Dso::TriggerMode mode ) {
    if ( scope->verboseLevel > 2 )
        qDebug() << "  TDock::setMode()" << int( mode );
    // Поиск по данным, а не по позиции в спецификации — см. примечание выше.
    const int index = modeComboBox->findData( QVariant::fromValue( int( mode ) ) );
    QSignalBlocker blocker( modeComboBox );
    if ( index >= 0 )
        modeComboBox->setCurrentIndex( index );
    emit modeChanged( scope->trigger.mode );
}

void TriggerDock::setSlope( Dso::Slope slope ) {
    if ( scope->verboseLevel > 2 )
        qDebug() << "  TDock::setSlope()" << int( slope );
    QSignalBlocker blocker( slopeComboBox );
    slopeComboBox->setCurrentIndex( int( slope ) );
}

void TriggerDock::setSource( int id ) {
    if ( scope->verboseLevel > 2 )
        qDebug() << "  TDock::setSource()" << id;
    if ( id >= sourceStandardStrings.count() )
        return;
    QSignalBlocker blocker( sourceComboBox );
    sourceComboBox->setCurrentIndex( id );
}

void TriggerDock::setSmooth( int smooth ) {
    if ( scope->verboseLevel > 2 )
        qDebug() << "  TDock::setSmooth()" << smooth;
    if ( int( smooth ) >= smoothStandardStrings.count() )
        return;
    QSignalBlocker blocker( smoothComboBox );
    smoothComboBox->setCurrentIndex( int( smooth ) );
}
