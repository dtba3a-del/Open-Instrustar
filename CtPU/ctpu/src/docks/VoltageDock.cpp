// SPDX-License-Identifier: GPL-2.0-or-later

#include <QCloseEvent>
#include <QDebug>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QSignalBlocker>

#include <algorithm>
#include <cmath>

#include "VoltageDock.h"
#include "movingaverage.h"
#include "params.h"
#include "dockwindows.h"
#include "utils/printutils.h"


template < typename... Args > struct SELECT {
    template < typename C, typename R > static constexpr auto OVERLOAD_OF( R ( C::*pmf )( Args... ) ) -> decltype( pmf ) {
        return pmf;
    }
};


const std::array< int, 8 > VoltageDock::probeAttnDetents = { 1, 3, 10, 20, 30, 100, 300, 1000 };


VoltageDock::VoltageDock( DsoSettingsScope *scope, const Dso::ControlSpecification *spec, QWidget *parent )
    : QDockWidget( tr( "Voltage" ), parent ), scope( scope ), spec( spec ) {

    if ( scope->verboseLevel > 1 )
        qDebug() << " VoltageDock::VoltageDock()";

    // Initialize lists for comboboxes
    updateCouplingStrings(); // блоки ещё не созданы — только список строк

    for ( auto e : Dso::MathModeEnum ) {
        modeStrings.append( Dso::mathModeString( e ) );
    }

    updateGainStrings();
    for ( double mathGainStep : scope->mathGainSteps ) {
        mathGainStrings << valueToString( mathGainStep, UNIT_VOLTS, 0 );
    }

    dockLayout = new QGridLayout();
    dockLayout->setColumnMinimumWidth( 0, 50 );
    dockLayout->setColumnStretch( 1, 1 ); // stretch ComboBox in 2nd (middle) column
    dockLayout->setColumnStretch( 2, 1 ); // stretch ComboBox in 3rd (last) column
    dockLayout->setSpacing( DOCK_LAYOUT_SPACING );
    // Initialize elements
    int row = 0;
    for ( ChannelID channel = 0; channel < scope->voltage.size(); ++channel ) {
        ChannelBlock b;

        if ( channel < spec->channels )
            b.usedCheckBox = new QCheckBox( tr( "CH&%1" ).arg( channel + 1 ) ); // define shortcut <ALT>1 / <ALT>2
        else
            b.usedCheckBox = new QCheckBox( tr( "MA&TH" ) );
        b.miscComboBox = new QComboBox();
        b.gainComboBox = new QComboBox();
        if ( scope->toolTipVisible )
            b.gainComboBox->setToolTip( tr( "Voltage range per vertical screen division" ) );
        b.invertCheckBox = new QCheckBox( tr( "Invert" ) );
        b.attnComboBox = new QComboBox();
        for ( int a : probeAttnDetents )
            b.attnComboBox->addItem( tr( "x%1" ).arg( a ), a );
        b.attnComboBox->addItem( tr( "Custom .." ), -1 );
        // У4а — РАЗРЯДНОСТЬ. Окно усреднения N, только степени двойки:
        // шаг ровно полбита по закону +0.5·log₂(N).
        b.resolutionComboBox = new QComboBox();
        for ( unsigned N : { 1u, 2u, 4u, 8u, 16u, 32u, 64u } ) {
            if ( N == 1u )
                b.resolutionComboBox->addItem( tr( "Res ×1" ), N );
            else
                b.resolutionComboBox->addItem(
                    tr( "Res N=%1 (+%2 bit)" ).arg( N ).arg( MovingAverage::effectiveBitsGained( N ), 0, 'g', 2 ), N );
        }
        if ( scope->toolTipVisible )
            b.resolutionComboBox->setToolTip(
                tr( "Resolution: boxcar average of N samples. Gains 0.5·log₂(N) effective bits\n"
                    "(Tektronix HiRes law) at the cost of bandwidth f₋₃dB ≈ 0.443·SR/N.\n"
                    "The mean preserves the level, so this does NOT change the reading or the\n"
                    "V/div grid — it only adds bits. Edges and narrow peaks flatten at large N\n"
                    "(LeCroy LAB 767) — check the residual, not the energy share." ) );

        // У4б — ЭКРАННОЕ УВЕЛИЧЕНИЕ. Отдельная функция: входные уровни не
        // меняются, растянуто только изображение, сетка В/дел перенормируется.
        b.zoomComboBox = new QComboBox();
        for ( double z : { 1.0, 2.0, 4.0, 8.0, 16.0 } )
            b.zoomComboBox->addItem( z == 1.0 ? tr( "Zoom ×1" ) : tr( "Zoom ×%1" ).arg( z, 0, 'g', 2 ), z );
        if ( scope->toolTipVisible )
            b.zoomComboBox->setToolTip(
                tr( "Screen zoom: vertical magnification of the trace only. Input levels are\n"
                    "unchanged; the V/div grid is renormalised by 1/zoom, so the reading stays\n"
                    "true. Trigger level, cursors and the V/div label follow the same factor." ) );
        b.zoomInfoLabel = new QLabel();
        b.zoomInfoLabel->setVisible( false );
        if ( scope->toolTipVisible )
            b.zoomInfoLabel->setToolTip(
                tr( "Tract −3 dB bandwidth on the current V/div setting. The input amplifier\n"
                    "trades bandwidth for gain (BW ≈ GBW/K), so the most sensitive ranges are\n"
                    "the narrowest — datasheet-derived estimate, not measured on this unit.\n"
                    "Digital Zoom narrows it further (0.443·SR/N); the smaller figure is shown." ) );
        if ( scope->toolTipVisible )
            b.attnComboBox->setToolTip( tr( "Probe attenuation: detent positions for standard probes "
                                             "(x1/x3/x10/x20/x30/x100/x300/x1000), Custom for a "
                                             "non-standard divider" ) );
        // CtPU — главная функция прибора, поэтому она в доке канала, а не в
        // четвёртой вкладке диалога настроек. Текст кнопки — действующая
        // единица канала, так что состояние читается не открывая ничего.
        b.ctpuButton = new QToolButton();
        b.ctpuButton->setText( channel < scope->voltage.size() && !scope->voltage[ channel ].ctpuUnit.isEmpty()
                                   ? scope->voltage[ channel ].ctpuUnit
                                   : QStringLiteral( "V" ) );
        b.ctpuButton->setAutoRaise( true );
        b.ctpuButton->setVisible( channel < spec->channels );

        channelBlocks.push_back( std::move( b ) );

        if ( channel < spec->channels ) {
            b.miscComboBox->addItems( couplingStrings );
            if ( scope->toolTipVisible )
                b.miscComboBox->setToolTip( tr( "Select DC or AC coupling" ) );
            b.gainComboBox->addItems( gainStringsForChannel( channel, scope->voltage[ channel ].probeAttn ) );
        } else {
            b.miscComboBox->addItems( modeStrings );
            if ( scope->toolTipVisible )
                b.miscComboBox->setToolTip( tr( "Select the mathematical operation for this channel" ) );
            b.gainComboBox->addItems( gainStringsForChannel( channel, scope->voltage[ channel ].probeAttn ) );
        }

        if ( channel < spec->channels ) {
            dockLayout->setColumnStretch( 1, 1 ); // stretch ComboBox in 2nd (middle) column 1x
            dockLayout->setColumnStretch( 2, 2 ); // stretch ComboBox in 3rd (last) column 2x
            dockLayout->addWidget( b.usedCheckBox, row, 0 );
            dockLayout->addWidget( b.gainComboBox, row++, 1, 1, 2 ); // fill 1 row, 2 col
            dockLayout->addWidget( b.invertCheckBox, row, 0 );
            dockLayout->addWidget( b.attnComboBox, row, 1, 1, 1 );   // fill 1 row, 2 col
            dockLayout->addWidget( b.miscComboBox, row++, 2, 1, 1 ); // fill 1 row, 2 col
            // Колонка 0 в строке зума была пуста — туда встаёт кнопка CtPU,
            // и раскладка дока не растёт ни на одну строку.
            dockLayout->addWidget( b.ctpuButton, row, 0 );
            dockLayout->addWidget( b.resolutionComboBox, row, 1, 1, 1 ); // У4а — разрядность
            dockLayout->addWidget( b.zoomInfoLabel, row++, 2, 1, 1 );
            dockLayout->addWidget( b.zoomComboBox, row++, 1, 1, 1 ); // У4б — экранное увеличение
            // draw divider line
            QFrame *divider = new QFrame();
            divider->setLineWidth( 1 );
            divider->setFrameShape( QFrame::HLine );
            QPalette palette = QPalette();
            palette.setColor( QPalette::WindowText, QColor( 128, 128, 128 ) );
            divider->setPalette( palette ); // reduce the contrast of the divider
            dockLayout->addWidget( divider, row++, 0, 1, 3 );
        } else { // MATH function, all in one row
            dockLayout->addWidget( b.usedCheckBox, row, 0 );
            dockLayout->addWidget( b.gainComboBox, row, 1 );
            dockLayout->addWidget( b.miscComboBox, row, 2 );
        }

        connect( b.gainComboBox, SELECT< int >::OVERLOAD_OF( &QComboBox::currentIndexChanged ), this,
                 [ this, channel ]( unsigned index ) {
                     this->scope->voltage[ channel ].gainStepIndex = index;
                     updateZoomInfo( channel ); // У9: полоса тракта зависит от предела
                     emit gainChanged( channel, this->scope->gain( channel ) );
                 } );
        // `activated` fires only on real user interaction (not on programmatic
        // setCurrentIndex), so setAttn() below can update the combobox without
        // re-entering this handler.
        connect( b.attnComboBox, SELECT< int >::OVERLOAD_OF( &QComboBox::activated ), this, [ this, channel ]( int index ) {
            QComboBox *combo = channelBlocks[ channel ].attnComboBox;
            int attnValue = combo->itemData( index ).toInt();
            if ( attnValue <= 0 ) { // "Custom .." — deliberate non-standard divider
                bool ok = false;
                attnValue = QInputDialog::getInt( this, tr( "Probe attenuation" ),
                                                  tr( "Attenuation factor (x%1 .. x%2):" ).arg( ATTENUATION_MIN ).arg( ATTENUATION_MAX ),
                                                  int( this->scope->voltage[ channel ].probeAttn ), ATTENUATION_MIN,
                                                  ATTENUATION_MAX, 1, &ok );
                if ( !ok ) { // cancelled: restore the current setting's position
                    setAttn( channel, this->scope->voltage[ channel ].probeAttn );
                    return;
                }
            }
            this->scope->voltage[ channel ].probeAttn = attnValue;
            setAttn( channel, attnValue );
            emit probeAttnChanged( channel, attnValue ); // make sure to set the probe first, since this will influence the gain
            emit gainChanged( channel, this->scope->gain( channel ) );
        } );
        connect( b.resolutionComboBox, SELECT< int >::OVERLOAD_OF( &QComboBox::activated ), this,
                 [ this, channel ]( int index ) {
                     const unsigned N = channelBlocks[ channel ].resolutionComboBox->itemData( index ).toUInt();
                     this->scope->voltage[ channel ].resolutionN = N;
                     updateZoomInfo( channel );
                     // Величина не меняется — перерисовка нужна только ради
                     // усреднённой трассы, не ради масштаба.
                     emit gainChanged( channel, this->scope->gain( channel ) );
                 } );
        connect( b.zoomComboBox, SELECT< int >::OVERLOAD_OF( &QComboBox::activated ), this, [ this, channel ]( int index ) {
            const double z = channelBlocks[ channel ].zoomComboBox->itemData( index ).toDouble();
            this->scope->voltage[ channel ].screenZoom = z > 0.0 ? z : 1.0;
            updateZoomInfo( channel );
            // Перенормировка сетки В/дел, порога триггера и курсоров идёт
            // через displayGain() — достаточно попросить перерисовку.
            emit gainChanged( channel, this->scope->gain( channel ) );
        } );
        connect( b.invertCheckBox, &QAbstractButton::toggled, this, [ this, channel ]( bool checked ) {
            this->scope->voltage[ channel ].inverted = checked;
            emit invertedChanged( channel, checked );
        } );
        connect( b.ctpuButton, &QToolButton::clicked, this, [ this, channel ]() { editCtpu( channel ); } );
        connect( b.miscComboBox, SELECT< int >::OVERLOAD_OF( &QComboBox::currentIndexChanged ), this,
                 [ this, channel, spec, scope ]( unsigned index ) {
                     this->scope->voltage[ channel ].couplingOrMathIndex = index;
                     if ( channel < spec->channels ) { // CH1 & CH2
                         // setCoupling(channel, (unsigned)index);
                         emit couplingChanged( channel, scope->coupling( channel, spec ) );
                     } else { // MATH function changed
                         Dso::MathMode mathMode = Dso::getMathMode( this->scope->voltage[ channel ] );
                         setAttn( channel, this->scope->voltage[ channel ].probeAttn );
                         emit modeChanged( mathMode );
                         emit usedChannelChanged( channel, Dso::mathChannelsUsed( mathMode ) );
                     }
                 } );
        connect( b.usedCheckBox, &QCheckBox::toggled, this, [ this, channel ]( bool checked ) {
            this->scope->voltage[ channel ].used = checked;
            this->scope->voltage[ channel ].visible = checked;
            unsigned mask = 0;
            if ( checked ) {
                if ( channel < this->spec->channels )
                    mask = channel + 1;
                else
                    mask = Dso::mathChannelsUsed( Dso::MathMode( this->scope->voltage[ 2 ].couplingOrMathIndex ) );
            }
            emit usedChannelChanged( channel, mask ); // channel bit mask 0b01, 0b10, 0b11
        } );
    }

    // Load settings into GUI
    loadSettings( scope, spec );

    dockWidget = new QWidget();
    SetupDockWidget( this, dockWidget, dockLayout );
}


void VoltageDock::loadSettings( DsoSettingsScope *scope, const Dso::ControlSpecification *spec ) {
    if ( scope->verboseLevel > 2 )
        qDebug() << "  VDock::loadSettings()";
    for ( ChannelID channel = 0; channel < scope->voltage.size(); ++channel ) {
        if ( channel < spec->channels ) {
            if ( int( scope->voltage[ channel ].couplingOrMathIndex ) < couplingStrings.size() )
                setCoupling( channel, scope->voltage[ channel ].couplingOrMathIndex );
        } else {
            setMode( scope->voltage[ channel ].couplingOrMathIndex );
        }

        setGain( channel, scope->voltage[ channel ].gainStepIndex );
        setUsed( channel, scope->voltage[ channel ].used );
        scope->voltage[ channel ].visible = scope->voltage[ channel ].used;
        setAttn( channel, scope->voltage[ channel ].probeAttn );
        {
            QSignalBlocker rb( channelBlocks[ channel ].resolutionComboBox );
            int ri = channelBlocks[ channel ].resolutionComboBox->findData( scope->voltage[ channel ].resolutionN );
            channelBlocks[ channel ].resolutionComboBox->setCurrentIndex( ri >= 0 ? ri : 0 );
            QSignalBlocker zb( channelBlocks[ channel ].zoomComboBox );
            int zi = channelBlocks[ channel ].zoomComboBox->findData( scope->voltage[ channel ].screenZoom );
            channelBlocks[ channel ].zoomComboBox->setCurrentIndex( zi >= 0 ? zi : 0 );
        }
        updateZoomInfo( channel );
        setInverted( channel, scope->voltage[ channel ].inverted );
    }
}


/// \brief Don't close the dock, just hide it
/// \param event The close event that should be handled.
void VoltageDock::closeEvent( QCloseEvent *event ) {
    hide();
    event->accept();
}


void VoltageDock::setCoupling( ChannelID channel, unsigned couplingIndex ) {
    if ( channel >= spec->channels )
        return;
    if ( couplingIndex >= spec->couplings.size() )
        return;
    if ( scope->verboseLevel > 2 )
        qDebug() << "  VDock::setCoupling()" << channel << couplingStrings[ int( couplingIndex ) ];
    QSignalBlocker blocker( channelBlocks[ channel ].miscComboBox );
    channelBlocks[ channel ].miscComboBox->setCurrentIndex( int( couplingIndex ) );
}


void VoltageDock::setGain( ChannelID channel, unsigned gainStepIndex ) {
    if ( channel >= scope->voltage.size() )
        return;
    if ( channel < spec->channels ) { // Voltage channel
        if ( gainStepIndex >= scope->gainSteps.size() )
            return;
        if ( scope->verboseLevel > 2 )
            qDebug() << "  VDock::setGain()" << channel << gainStrings[ int( gainStepIndex ) ];
    } else {
        if ( gainStepIndex >= scope->mathGainSteps.size() )
            return;
        if ( scope->verboseLevel > 2 )
            qDebug() << "  VDock::setGain()" << channel << mathGainStrings[ int( gainStepIndex ) ];
    }
    QSignalBlocker blocker( channelBlocks[ channel ].gainComboBox );
    channelBlocks[ channel ].gainComboBox->setCurrentIndex( int( gainStepIndex ) );
}


void VoltageDock::setAttn( ChannelID channel, double attnValue ) {
    if ( scope->verboseLevel > 2 )
        qDebug() << "  VDock::setAttn()" << channel << attnValue;
    if ( channel >= scope->voltage.size() )
        return;
    QSignalBlocker blocker( channelBlocks[ channel ].gainComboBox );
    int index = channelBlocks[ channel ].gainComboBox->currentIndex();

    channelBlocks[ channel ].gainComboBox->clear();
    // Задание 11 очереди: шкала В/дел строится ОДНИМ местом —
    // gainStringsForChannel(), которое знает про физические единицы канала.
    //
    // Прежде здесь стояла своя сборка списка в вольтах, и она затирала
    // PU-подписи при каждой смене делителя щупа и при загрузке настроек: в
    // канале выставлены °C, а список показывал «V» и «mV». Подпись врала,
    // причём тем чаще, чем больше оператор трогал настройки.
    channelBlocks[ channel ].gainComboBox->addItems( gainStringsForChannel( channel, attnValue ) );
    channelBlocks[ channel ].gainComboBox->setCurrentIndex( index );
    scope->voltage[ channel ].probeAttn = attnValue;
    // У6: select the matching detent, or park on "Custom .." with the value
    // shown in its label — the position always tells the truth about the
    // active factor, никогда «пустой» пункт.
    QComboBox *attnCombo = channelBlocks[ channel ].attnComboBox;
    QSignalBlocker attnBlocker( attnCombo );
    const int customIdx = attnCombo->count() - 1;
    int detentIdx = attnCombo->findData( int( attnValue ) );
    if ( detentIdx >= 0 && detentIdx != customIdx ) {
        attnCombo->setItemText( customIdx, tr( "Custom .." ) );
        attnCombo->setCurrentIndex( detentIdx );
    } else {
        attnCombo->setItemText( customIdx, tr( "x%1 (custom)" ).arg( attnValue ) );
        attnCombo->setCurrentIndex( customIdx );
    }
}


void VoltageDock::updateZoomInfo( ChannelID channel ) {
    // Единая индикация полосы тракта на канале (У4 + У9):
    // - аналоговая часть зависит от ПРЕДЕЛА — K усилителя растёт на
    //   чувствительных пределах и режет полосу (BW ≈ GBW/K, данные из
    //   спецификации модели; расчёт по даташиту, не измерено);
    // - цифровая часть — от зума (0.443·SR/N, закон movingaverage.h);
    // показывается минимум из двух. Полоса — от ТЕКУЩЕЙ samplerate
    // (пересчитывается при каждом вызове; смена SR обновит текст при
    // следующем действии с контролами или загрузке настроек).
    QLabel *info = channelBlocks[ channel ].zoomInfoLabel;
    if ( channel >= spec->channels ) { // MATH: нет ни тракта, ни зума
        info->setVisible( false );
        return;
    }
    const unsigned N = scope->voltage[ channel ].resolutionN;
    double fAnalog = 0.0; // 0 = неизвестно (модель без таблицы полос)
    const unsigned gainStep = scope->voltage[ channel ].gainStepIndex;
    if ( gainStep < spec->analogBandwidth.size() )
        fAnalog = spec->analogBandwidth[ gainStep ];
    const double fDigital = N > 1 ? MovingAverage::cutoffHz( N, scope->horizontal.samplerate ) : 0.0;
    double f3dB = 0.0;
    if ( fAnalog > 0.0 && fDigital > 0.0 )
        f3dB = std::min( fAnalog, fDigital );
    else
        f3dB = std::max( fAnalog, fDigital );
    QString text;
    if ( N > 1 )
        text = tr( "+%1 bit" ).arg( MovingAverage::effectiveBitsGained( N ), 0, 'g', 2 );
    const double zoom = scope->voltage[ channel ].screenZoom;
    if ( zoom > 1.0 ) { // У4б: увеличение показывается отдельно от бит
        if ( !text.isEmpty() )
            text += QStringLiteral( ", " );
        text += tr( "zoom ×%1" ).arg( zoom, 0, 'g', 2 );
    }
    if ( f3dB > 0.0 ) {
        if ( !text.isEmpty() )
            text += QStringLiteral( ", " );
        text += tr( "−3dB %1" ).arg( valueToString( f3dB, UNIT_HERTZ, 3 ) );
    }
    info->setText( text );
    info->setVisible( !text.isEmpty() );
}


void VoltageDock::setMode( unsigned mathModeIndex ) {
    if ( scope->verboseLevel > 2 )
        qDebug() << "  VDock::setMode()" << modeStrings[ int( mathModeIndex ) ];
    QSignalBlocker blocker( channelBlocks[ spec->channels ].miscComboBox );
    channelBlocks[ spec->channels ].miscComboBox->setCurrentIndex( int( mathModeIndex ) );
}


void VoltageDock::setUsed( ChannelID channel, bool used ) {
    if ( channel >= scope->voltage.size() )
        return;
    if ( scope->verboseLevel > 2 )
        qDebug() << "  VDock::setUsed()" << channel << used;
    QSignalBlocker blocker( channelBlocks[ channel ].usedCheckBox );
    channelBlocks[ channel ].usedCheckBox->setChecked( used );
}


void VoltageDock::setInverted( ChannelID channel, bool inverted ) {
    if ( channel >= scope->voltage.size() )
        return;
    if ( scope->verboseLevel > 2 )
        qDebug() << "  VDock::setInverted()" << channel << inverted;
    QSignalBlocker blocker( channelBlocks[ channel ].invertCheckBox );
    channelBlocks[ channel ].invertCheckBox->setChecked( inverted );
}


void VoltageDock::updateCtpuButton( ChannelID channel ) {
    if ( channel >= channelBlocks.size() || !channelBlocks[ channel ].ctpuButton )
        return;
    const auto &v = scope->voltage[ channel ];
    // На кнопке — действующая единица: состояние канала видно без открывания.
    channelBlocks[ channel ].ctpuButton->setText( v.ctpuUnit.isEmpty() ? QStringLiteral( "V" ) : v.ctpuUnit );
    if ( scope->toolTipVisible )
        channelBlocks[ channel ].ctpuButton->setToolTip(
            tr( "Physical units of this channel: P = k·V + b\nk = %1, b = %2 — click to edit" )
                .arg( v.ctpuK )
                .arg( v.ctpuB ) );
    // Шкала В/дел показывается в тех же единицах, иначе подпись врёт.
    QComboBox *gainBox = channelBlocks[ channel ].gainComboBox;
    if ( gainBox ) {
        QSignalBlocker blocker( gainBox );
        const int index = gainBox->currentIndex();
        gainBox->clear();
        gainBox->addItems( gainStringsForChannel( channel, scope->voltage[ channel ].probeAttn ) );
        gainBox->setCurrentIndex( index );
    }
}


/// Редактор группы `ctpu` строится ПО ОБЪЯВЛЕНИЮ (params.h): виды, подписи,
/// единицы, границы и подсказки берутся из реестра. Добавление пятого поля
/// CtPU не потребует правки этого кода — только строки в params.cpp.
void VoltageDock::editCtpu( ChannelID channel ) {
    if ( channel >= scope->voltage.size() )
        return;
    const auto defs = Params::group( QStringLiteral( "ctpu" ) );
    if ( defs.empty() )
        return;

    QDialog dlg( this );
    dlg.setWindowTitle( tr( "CtPU — physical units, CH%1" ).arg( channel + 1 ) );
    auto *form = new QFormLayout( &dlg );
    std::vector< QWidget * > editors;
    editors.reserve( defs.size() );

    for ( const Params::Def *def : defs ) {
        const QVariant now = def->get( *scope, channel );
        QWidget *w = nullptr;
        switch ( def->kind ) {
        case Params::Kind::Choice: {
            auto *box = new QComboBox( &dlg );
            box->addItems( def->choices );
            box->setCurrentIndex( now.toInt() );
            w = box;
            break;
        }
        case Params::Kind::Text: {
            auto *edit = new QLineEdit( now.toString(), &dlg );
            if ( def->max > 0 )
                edit->setMaxLength( int( def->max ) );
            w = edit;
            break;
        }
        case Params::Kind::Real: {
            auto *spin = new QDoubleSpinBox( &dlg );
            spin->setDecimals( def->decimals );
            spin->setRange( def->min, def->max );
            spin->setValue( now.toDouble() );
            if ( !def->unit.isEmpty() )
                spin->setSuffix( QStringLiteral( " " ) + def->unit );
            w = spin;
            break;
        }
        case Params::Kind::Int: {
            auto *spin = new QSpinBox( &dlg );
            spin->setRange( int( def->min ), int( def->max ) );
            spin->setValue( now.toInt() );
            w = spin;
            break;
        }
        case Params::Kind::Bool: {
            auto *check = new QCheckBox( &dlg );
            check->setChecked( now.toBool() );
            w = check;
            break;
        }
        }
        if ( !w )
            continue;
        if ( scope->toolTipVisible && !def->tip.isEmpty() )
            w->setToolTip( def->tip );
        form->addRow( def->label, w );
        editors.push_back( w );
    }

    auto *buttons = new QDialogButtonBox( QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg );
    connect( buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept );
    connect( buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject );
    form->addRow( buttons );

    if ( dlg.exec() != QDialog::Accepted )
        return;

    for ( std::size_t i = 0; i < defs.size() && i < editors.size(); ++i ) {
        const Params::Def *def = defs[ i ];
        QVariant value;
        if ( auto *box = qobject_cast< QComboBox * >( editors[ i ] ) )
            value = box->currentIndex();
        else if ( auto *edit = qobject_cast< QLineEdit * >( editors[ i ] ) )
            value = edit->text();
        else if ( auto *spin = qobject_cast< QDoubleSpinBox * >( editors[ i ] ) )
            value = spin->value();
        else if ( auto *spin = qobject_cast< QSpinBox * >( editors[ i ] ) )
            value = spin->value();
        else if ( auto *check = qobject_cast< QCheckBox * >( editors[ i ] ) )
            value = check->isChecked();
        else
            continue;
        def->set( *scope, channel, Params::clamp( *def, value ) );
    }

    updateCtpuButton( channel );
    emit ctpuChanged( channel );
}


void VoltageDock::updateCouplingStrings() {
    // Что показывать: DC всегда; AC — если прибор умеет её сам
    // (hasACcoupling из спецификации модели) либо если пользователь
    // объявил аппаратную доработку (hasACmodification из настроек).
    QStringList rebuilt;
    for ( Dso::Coupling c : spec->couplings )
        if ( c == Dso::Coupling::DC || scope->hasACcoupling || scope->hasACmodification )
            rebuilt.append( Dso::couplingString( c ) );
    if ( rebuilt == couplingStrings && !channelBlocks.empty() )
        return; // ничего не изменилось — не трогать выпадающие списки
    couplingStrings = rebuilt;

    // Конструктор вызывает нас до создания блоков: там нужен только список.
    for ( ChannelID channel = 0; channel < channelBlocks.size(); ++channel ) {
        if ( channel >= spec->channels ) // математические каналы — не связь, а режим
            continue;
        QComboBox *box = channelBlocks[ channel ].miscComboBox;
        if ( !box )
            continue;
        // Сохранить выбор пользователя, а не сбросить его на DC: молчаливый
        // сброс и был тем, из-за чего «найденная» настройка не держалась.
        unsigned wanted = scope->voltage[ channel ].couplingOrMathIndex;
        const bool blocked = box->blockSignals( true );
        box->clear();
        box->addItems( couplingStrings );
        if ( wanted >= unsigned( couplingStrings.size() ) )
            wanted = 0; // выбранного вида связи больше нет в списке
        box->setCurrentIndex( int( wanted ) );
        box->blockSignals( blocked );
        if ( scope->voltage[ channel ].couplingOrMathIndex != wanted ) {
            scope->voltage[ channel ].couplingOrMathIndex = wanted;
            emit couplingChanged( channel, scope->coupling( channel, spec ) );
        }
    }
}


void VoltageDock::updateGainStrings( double attnValue ) {
    // [MOD] TZ §3.5.2 — gain strings now reflect the active CtPU unit.
    // Since the legacy signature takes no channel parameter, we use the unit
    // of the first real channel (CH1). Per-channel rebuilding is done by the
    // new updateGainStrings(channel, attnValue) overload below.
    gainStrings.clear();
    const QString unit = scope->voltage.empty() ? QStringLiteral( "V" ) : scope->voltage[ 0 ].ctpuUnit;
    for ( auto gainStep : spec->gain ) {
        gainStrings << valueToString( gainStep.Vdiv * attnValue, unit, 0 );
    }
}


/// \brief Per-channel gain string rebuild (TZ §3.5.2).
/// Uses the channel's own ctpuUnit and ctpuK to display the gain in physical
/// units per div (e.g. "100 °C/div" when k=100 and unit="°C"). Falls back to
/// the legacy UNIT_VOLTS formatter when the channel's ctpuMode is OFF.
QStringList VoltageDock::gainStringsForChannel( ChannelID channel, double attnValue ) const {
    QStringList result;
    if ( channel >= scope->voltage.size() )
        return result;
    const auto &v = scope->voltage[ channel ];
    // Единица берётся у канала. Пустая строка означает «единица не задана» —
    // тогда действует базовая: вольт у вещественного канала, а у
    // математического — та, что даёт его операция (V² у произведения и т. п.).
    // Молча подставлять вольт математическому каналу нельзя: у произведения
    // размерность другая, и подпись стала бы неверной.
    const bool ctpuActive = ( v.ctpuMode != CtPU::Mode::OFF ) && !v.ctpuUnit.isEmpty();
    if ( channel < spec->channels ) {
        const QString unit = ctpuActive ? v.ctpuUnit : QStringLiteral( "V" );
        const double k = ctpuActive ? v.ctpuK : 1.0;
        for ( auto gainStep : spec->gain )
            result << valueToString( gainStep.Vdiv * attnValue * k, unit, 0 );
    } else {
        const Unit mathUnit =
            Dso::mathModeUnit( Dso::MathMode( scope->voltage[ spec->channels ].couplingOrMathIndex ) );
        for ( double mathGainStep : scope->mathGainSteps ) {
            const double value = mathGainStep * attnValue * ( ctpuActive ? v.ctpuK : 1.0 );
            if ( ctpuActive )
                result << valueToString( value, v.ctpuUnit, 0 );
            else
                result << valueToString( value, mathUnit, -1 ); // auto format V or V²
        }
    }
    return result;
}
