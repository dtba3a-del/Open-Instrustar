// SPDX-License-Identifier: GPL-2.0-or-later

#include <QApplication>
#include <QColor>
#include <QFileInfo>
#include <QSettings>

#include "dsosettings.h"
#include "dsowidget.h"
#include "hantekdso/mathmodes.h"
#include "params.h"

/// \brief Set the number of channels.
/// \param channels The new channel count, that will be applied to lists.
DsoSettings::DsoSettings( const ScopeDevice *scopeDevice, int verboseLevel, bool resetSettings )
    : deviceName( scopeDevice->getModel()->name ), deviceID( scopeDevice->getSerialNumber() ),
      deviceFW( scopeDevice->getFwVersion() ), deviceSpecification( scopeDevice->getModel()->spec() ), verboseLevel( verboseLevel ),
      resetSettings( resetSettings ) {
    scope.verboseLevel = verboseLevel;
    if ( verboseLevel > 1 )
        qDebug() << " DsoSettings::DsoSettings()" << deviceName << deviceID << resetSettings;
    // Add new channels to the list
    int voltage_hue[] = { 60, 210, 0, 120 };   // yellow, lightblue, red, green
    int spectrum_hue[] = { 30, 240, 330, 90 }; // orange, blue, purple, green
    unsigned index = 0;
    scope.hasACcoupling = deviceSpecification->hasACcoupling;
    while ( scope.spectrum.size() < deviceSpecification->channels ) {
        // Spectrum
        DsoSettingsScopeSpectrum newSpectrum;
        newSpectrum.name = tr( "SP%1" ).arg( index + 1 );
        scope.spectrum.push_back( newSpectrum );

        // Voltage
        DsoSettingsScopeVoltage newVoltage;
        newVoltage.name = tr( "CH%1" ).arg( index + 1 );
        scope.voltage.push_back( newVoltage );
        view.screen.voltage.push_back( QColor::fromHsv( voltage_hue[ index ], 0xff, 0xff ) );
        view.screen.spectrum.push_back( QColor::fromHsv( spectrum_hue[ index ], 0xff, 0xff ) );
        view.print.voltage.push_back( view.screen.voltage.back().darker() );
        view.print.spectrum.push_back( view.screen.spectrum.back().darker() );
        if ( ++index >= sizeof voltage_hue )
            index = 0;
    }

    DsoSettingsScopeSpectrum newSpectrum;
    newSpectrum.name = tr( "SPM" );
    scope.spectrum.push_back( newSpectrum );

    DsoSettingsScopeVoltage newVoltage;
    newVoltage.couplingOrMathIndex = unsigned( Dso::MathMode::ADD_CH1_CH2 );
    newVoltage.name = tr( "MATH" );
    scope.voltage.push_back( newVoltage );

    view.screen.voltage.push_back( QColor::fromHsv( 300, 0xff, 0xff ) );  // purple (V=100%)
    view.screen.spectrum.push_back( QColor::fromHsv( 300, 0xff, 0xc0 ) ); // brightness V=75%
    view.print.voltage.push_back( QColor::fromHsv( 300, 0xff, 0xc0 ) );   // brightness V=75%
    view.print.spectrum.push_back( QColor::fromHsv( 300, 0xff, 0x80 ) );  // brightness V=50%

    // -- TZ §5.2 — math-stack: 4 configurable virtual channels.
    // The first math slot inherits the legacy MATH settings (mode ADD_CH1_CH2);
    // the remaining three slots default to disabled. Names use the M1..M4 scheme.
    // We extend `scope.voltage` so all existing iteration loops see them automatically.
    // CRITICAL: `scope.spectrum`, `view.screen.spectrum`, `view.print.spectrum`
    // must all be extended in lockstep with `scope.voltage` — many loops
    // iterate `channel < scope.voltage.size()` and then index `scope.spectrum[channel]`
    // or `view->colors->spectrum[channel]`. A size mismatch crashes on first frame.
    for ( int i = 1; i < DsoSettingsScope::maxMathChannels; ++i ) {
        DsoSettingsScopeVoltage mathVoltage;
        mathVoltage.couplingOrMathIndex = unsigned( Dso::MathMode::ADD_CH1_CH2 ); // legacy default
        mathVoltage.name = tr( "M%1" ).arg( i + 1 );
        mathVoltage.used = false; // only M1 enabled by default (set below)
        scope.voltage.push_back( mathVoltage );

        // Spectrum entry for this math channel — must exist so that
        // `scope.spectrum[channel]` is in-bounds for channel = 2..5.
        DsoSettingsScopeSpectrum mathSpectrum;
        mathSpectrum.name = tr( "SPM%1" ).arg( i + 1 );
        scope.spectrum.push_back( mathSpectrum );

        // hue-shifted math colors: 300, 330, 0, 30 (purple, magenta, red, orange)
        int hue = ( 300 + i * 30 ) % 360;
        view.screen.voltage.push_back( QColor::fromHsv( hue, 0xff, 0xff ) );
        view.screen.spectrum.push_back( QColor::fromHsv( hue, 0xff, 0xc0 ) );
        view.print.voltage.push_back( QColor::fromHsv( hue, 0xff, 0xc0 ) );
        view.print.spectrum.push_back( QColor::fromHsv( hue, 0xff, 0x80 ) );
    }

    // -- TZ §5.2.1 — math-stack configuration vector (separate from voltage[]).
    // M1 = CH1+CH2 enabled by default; M2..M4 disabled.
    scope.mathStack.resize( DsoSettingsScope::maxMathChannels );
    scope.mathStack[ 0 ].enabled = true;
    scope.mathStack[ 0 ].srcA = 0;
    scope.mathStack[ 0 ].srcB = 1;
    scope.mathStack[ 0 ].op = Dso::MathOp::ADD;
    for ( int i = 1; i < DsoSettingsScope::maxMathChannels; ++i ) {
        scope.mathStack[ i ].enabled = false;
        scope.mathStack[ i ].srcA = 0;
        scope.mathStack[ i ].srcB = ( i == 1 ) ? 1 : 0;
        scope.mathStack[ i ].op = Dso::MathOp::ADD;
    }

    // -- TZ §7.1.2 — XY multi-curve: curve 0 = CH1 vs CH2, others disabled.
    scope.xyCurves.resize( DsoSettingsScope::maxXYCurves );
    scope.xyCurves[ 0 ].enabled = true;
    scope.xyCurves[ 0 ].xChannel = 0;
    scope.xyCurves[ 0 ].yChannel = 1;
    scope.xyCurves[ 0 ].explicitColor = defaultCurveColor( 0 );
    for ( int i = 1; i < DsoSettingsScope::maxXYCurves; ++i ) {
        scope.xyCurves[ i ].enabled = false;
        scope.xyCurves[ i ].xChannel = 0;
        scope.xyCurves[ i ].yChannel = 1;
        // Умолчания разнесены по кругу оттенков: совпавший цвет прячет одну
        // кривую под другой и молча вводит в заблуждение (задание 4).
        scope.xyCurves[ i ].explicitColor = defaultCurveColor( i );
    }

    // create an unique storage for this device based on device name and serial number
    // individual device settings location:
    // Linux, Unix: $HOME/.config/OpenHantek/<deviceName>_<deviceID>.conf
    // macOS:       $HOME/Library/Preferences/org.openhantek.<deviceName>_<deviceID>.plist
    // Windows:     HKEY_CURRENT_USER\Software\OpenHantek\<deviceName>_<deviceID>
    // more info:   https://doc.qt.io/qt-5/qsettings.html#platform-specific-notes
    storeSettings =
        std::unique_ptr< QSettings >( new QSettings( QCoreApplication::organizationName(), deviceName + "_" + deviceID ) );
    // and get the persistent settings
    load();
}


// store the current settings to an explicitly named file
bool DsoSettings::saveToFile( const QString &filename ) {
    if ( verboseLevel > 1 )
        qDebug() << " DsoSettings::saveFilename()" << filename;
    std::unique_ptr< QSettings > local = std::unique_ptr< QSettings >( new QSettings( filename, QSettings::IniFormat ) );
    if ( local->status() != QSettings::NoError ) {
        qWarning() << "Could not save to config file " << filename;
        return false;
    }
    storeSettings.swap( local ); // switch to requested filename
    save();                      // store the settings
    storeSettings.swap( local ); // and switch back to default persistent storage location (file, registry, ...)
    return true;
}


// load settings from a config file
bool DsoSettings::loadFromFile( const QString &filename ) {
    if ( verboseLevel > 1 )
        qDebug() << " DsoSettings::loadFilename()" << filename;
    if ( QFileInfo( filename ).isReadable() ) {
        std::unique_ptr< QSettings > local = std::unique_ptr< QSettings >( new QSettings( filename, QSettings::IniFormat ) );
        if ( local->status() == QSettings::NoError ) {
            storeSettings.swap( local );
            load();
            storeSettings.swap( local );
            return true;
        }
    }
    qWarning() << "Could not load from config file " << filename;
    return false;
}


// load the persistent scope settings
// called by "DsoSettings::DsoSettings()" and "loadFromFile()"
void DsoSettings::load() {
    if ( verboseLevel > 1 )
        qDebug() << " DsoSettings::load()" << storeSettings->fileName();
    // Start with default configuration?
    if ( resetSettings || storeSettings->value( "configuration/version", 0 ).toUInt() < CONFIG_VERSION ) {
        // incompatible change or config reset by user
        storeSettings->clear(); // start with a clean config storage
        QSettings().clear();    // and a clean global storage
        setDefaultConfig();
        return;
    }

    alwaysSave = storeSettings->value( "configuration/alwaysSave", alwaysSave ).toBool();

    // Oscilloscope settings
    storeSettings->beginGroup( "scope" );
    // Horizontal axis
    storeSettings->beginGroup( "horizontal" );
    if ( storeSettings->contains( "format" ) )
        scope.horizontal.format = Dso::GraphFormat( storeSettings->value( "format" ).toInt() );
    if ( storeSettings->contains( "frequencybase" ) )
        scope.horizontal.frequencybase = storeSettings->value( "frequencybase" ).toDouble();
    for ( int marker = 0; marker < 2; ++marker ) {
        QString name;
        name = QString( "marker%1" ).arg( marker );
        if ( storeSettings->contains( name ) )
            scope.setMarker( unsigned( marker ), storeSettings->value( name ).toDouble() );
    }
    if ( storeSettings->contains( "timebase" ) )
        scope.horizontal.timebase = storeSettings->value( "timebase" ).toDouble();
    if ( storeSettings->contains( "maxTimebase" ) )
        scope.horizontal.maxTimebase = storeSettings->value( "maxTimebase" ).toDouble();
    if ( storeSettings->contains( "acquireInterval" ) )
        scope.horizontal.acquireInterval = storeSettings->value( "acquireInterval" ).toDouble();
    if ( storeSettings->contains( "recordLength" ) )
        scope.horizontal.recordLength = storeSettings->value( "recordLength" ).toInt();
    if ( storeSettings->contains( "samplerate" ) )
        scope.horizontal.samplerate = storeSettings->value( "samplerate" ).toDouble();
    if ( storeSettings->contains( "calfreq" ) )
        scope.horizontal.calfreq = storeSettings->value( "calfreq" ).toDouble();
    // -- У8 — XY chart-recorder configuration (ENGINEERING_LOG §17)
    if ( storeSettings->contains( "xyUseBinTape" ) )
        scope.horizontal.xyUseBinTape = storeSettings->value( "xyUseBinTape" ).toBool();
    if ( storeSettings->contains( "xyBinCount" ) )
        scope.horizontal.xyBinCount = storeSettings->value( "xyBinCount" ).toUInt();
    if ( storeSettings->contains( "xyMedianWindow" ) )
        scope.horizontal.xyMedianWindow = storeSettings->value( "xyMedianWindow" ).toUInt();
    if ( storeSettings->contains( "xyFieldBinsX" ) )
        scope.horizontal.xyFieldBinsX = storeSettings->value( "xyFieldBinsX" ).toUInt();
    if ( storeSettings->contains( "xyFieldBinsY" ) )
        scope.horizontal.xyFieldBinsY = storeSettings->value( "xyFieldBinsY" ).toUInt();
    if ( storeSettings->contains( "xyMasterAxis" ) )
        scope.horizontal.xyMasterAxis = storeSettings->value( "xyMasterAxis" ).toUInt();
    if ( storeSettings->contains( "xySheetMode" ) )
        scope.horizontal.xySheetMode = storeSettings->value( "xySheetMode" ).toUInt();
    if ( storeSettings->contains( "xySlewRateX" ) )
        scope.horizontal.xySlewRateX = storeSettings->value( "xySlewRateX" ).toDouble();
    if ( storeSettings->contains( "xySlewRateY" ) )
        scope.horizontal.xySlewRateY = storeSettings->value( "xySlewRateY" ).toDouble();
    if ( storeSettings->contains( "xyTargetPoints" ) )
        scope.horizontal.xyTargetPoints = storeSettings->value( "xyTargetPoints" ).toUInt();
    if ( storeSettings->contains( "xyTargetDensity" ) )
        scope.horizontal.xyTargetDensity = storeSettings->value( "xyTargetDensity" ).toDouble();
    if ( storeSettings->contains( "xyTrackSigma" ) )
        scope.horizontal.xyTrackSigma = storeSettings->value( "xyTrackSigma" ).toBool();
    if ( storeSettings->contains( "xyExtractMode" ) )
        scope.horizontal.xyExtractMode = storeSettings->value( "xyExtractMode" ).toUInt();
    if ( storeSettings->contains( "xyTimeSecPerDiv" ) )
        scope.horizontal.xyTimeSecPerDiv = storeSettings->value( "xyTimeSecPerDiv" ).toDouble();
    // -- TZ §7 — XY multi-curve configuration (load before voltage so defaults exist)
    scope.xyCurves.resize( DsoSettingsScope::maxXYCurves );
    for ( int i = 0; i < DsoSettingsScope::maxXYCurves; ++i ) {
        storeSettings->beginGroup( QString( "xyCurves/curve%1" ).arg( i ) );
        if ( storeSettings->contains( "enabled" ) )
            scope.xyCurves[ i ].enabled = storeSettings->value( "enabled" ).toBool();
        if ( storeSettings->contains( "xChannel" ) )
            scope.xyCurves[ i ].xChannel = uint8_t( storeSettings->value( "xChannel" ).toUInt() );
        if ( storeSettings->contains( "yChannel" ) )
            scope.xyCurves[ i ].yChannel = uint8_t( storeSettings->value( "yChannel" ).toUInt() );
        // Задание 4 очереди: явный цвет кривой. Пустая строка = не назначен,
        // тогда действует умолчание, разное для каждой кривой.
        if ( storeSettings->contains( "color" ) ) {
            const QString name = storeSettings->value( "color" ).toString();
            if ( !name.isEmpty() )
                scope.xyCurves[ i ].explicitColor = QColor( name );
        }
        storeSettings->endGroup();
    }
    storeSettings->endGroup(); // horizontal
    // -- TZ §5 — math-stack configuration (separate from voltage[])
    scope.mathStack.resize( DsoSettingsScope::maxMathChannels );
    for ( int i = 0; i < DsoSettingsScope::maxMathChannels; ++i ) {
        storeSettings->beginGroup( QString( "mathStack/math%1" ).arg( i ) );
        if ( storeSettings->contains( "enabled" ) )
            scope.mathStack[ i ].enabled = storeSettings->value( "enabled" ).toBool();
        if ( storeSettings->contains( "srcA" ) )
            scope.mathStack[ i ].srcA = uint8_t( storeSettings->value( "srcA" ).toUInt() );
        if ( storeSettings->contains( "srcB" ) )
            scope.mathStack[ i ].srcB = uint8_t( storeSettings->value( "srcB" ).toUInt() );
        if ( storeSettings->contains( "op" ) )
            scope.mathStack[ i ].op = Dso::MathOp( storeSettings->value( "op" ).toUInt() );
        if ( storeSettings->contains( "invert" ) )
            scope.mathStack[ i ].invert = storeSettings->value( "invert" ).toBool();
        if ( storeSettings->contains( "ctpuUnit" ) )
            scope.mathStack[ i ].ctpuUnit = storeSettings->value( "ctpuUnit" ).toString();
        if ( storeSettings->contains( "ctpuK" ) )
            scope.mathStack[ i ].ctpuK = storeSettings->value( "ctpuK" ).toDouble();
        if ( storeSettings->contains( "ctpuB" ) )
            scope.mathStack[ i ].ctpuB = storeSettings->value( "ctpuB" ).toDouble();
        storeSettings->endGroup();
    }
    // Trigger
    storeSettings->beginGroup( "trigger" );
    if ( storeSettings->contains( "mode" ) )
        scope.trigger.mode = Dso::TriggerMode( storeSettings->value( "mode" ).toUInt() );
    if ( storeSettings->contains( "position" ) )
        scope.trigger.position = storeSettings->value( "position" ).toDouble();
    if ( storeSettings->contains( "slope" ) )
        scope.trigger.slope = Dso::Slope( storeSettings->value( "slope" ).toUInt() );
    if ( storeSettings->contains( "source" ) )
        scope.trigger.source = storeSettings->value( "source" ).toInt();
    if ( storeSettings->contains( "smooth" ) )
        scope.trigger.smooth = storeSettings->value( "smooth" ).toInt();
    storeSettings->endGroup(); // trigger
    // Spectrum
    for ( ChannelID channel = 0; channel < scope.spectrum.size(); ++channel ) {
        storeSettings->beginGroup( QString( "spectrum%1" ).arg( channel ) );
        if ( storeSettings->contains( "magnitude" ) )
            scope.spectrum[ channel ].magnitude = storeSettings->value( "magnitude" ).toDouble();
        if ( storeSettings->contains( "offset" ) )
            scope.spectrum[ channel ].offset = storeSettings->value( "offset" ).toDouble();
        if ( storeSettings->contains( "used" ) )
            scope.spectrum[ channel ].used = storeSettings->value( "used" ).toBool();
        storeSettings->beginGroup( "cursor" );
        if ( storeSettings->contains( "shape" ) )
            scope.spectrum[ channel ].cursor.shape =
                DsoSettingsScopeCursor::CursorShape( storeSettings->value( "shape" ).toUInt() );
        for ( int marker = 0; marker < 2; ++marker ) {
            QString name;
            name = QString( "x%1" ).arg( marker );
            if ( storeSettings->contains( name ) )
                scope.spectrum[ channel ].cursor.pos[ marker ].setX( storeSettings->value( name ).toDouble() );
            name = QString( "y%1" ).arg( marker );
            if ( storeSettings->contains( name ) )
                scope.spectrum[ channel ].cursor.pos[ marker ].setY( storeSettings->value( name ).toDouble() );
        }
        storeSettings->endGroup(); // cursor
        storeSettings->endGroup(); // spectrum%1
    }
    // Voltage
    bool defaultConfig = false;
    // defaultConfig = deviceSpecification->isDemoDevice; // use default channel setting in demo mode
    if ( storeSettings->contains( "hasACmodification" ) )
        scope.hasACmodification = storeSettings->value( "hasACmodification" ).toBool();
    for ( ChannelID channel = 0; channel < scope.voltage.size(); ++channel ) {
        storeSettings->beginGroup( QString( "voltage%1" ).arg( channel ) );
        if ( storeSettings->contains( "gainStepIndex" ) )
            scope.voltage[ channel ].gainStepIndex = storeSettings->value( "gainStepIndex" ).toUInt();
        if ( storeSettings->contains( "couplingOrMathIndex" ) ) {
            scope.voltage[ channel ].couplingOrMathIndex = storeSettings->value( "couplingOrMathIndex" ).toUInt();
            if ( channel < deviceSpecification->channels ) {
                if ( scope.voltage[ channel ].couplingOrMathIndex >= deviceSpecification->couplings.size() ||
                     ( !scope.hasACcoupling && !scope.hasACmodification ) )
                    scope.voltage[ channel ].couplingOrMathIndex = 0; // set to default if out of range
            } else {
                if ( scope.voltage[ channel ].couplingOrMathIndex > unsigned( Dso::LastMathMode ) )
                    scope.voltage[ channel ].couplingOrMathIndex = 0;
            }
        }
        if ( storeSettings->contains( "inverted" ) )
            scope.voltage[ channel ].inverted = storeSettings->value( "inverted" ).toBool();
        if ( storeSettings->contains( "offset" ) )
            scope.voltage[ channel ].offset = storeSettings->value( "offset" ).toDouble();
        if ( storeSettings->contains( "trigger" ) )
            scope.voltage[ channel ].trigger = storeSettings->value( "trigger" ).toDouble();
        if ( storeSettings->contains( "probeAttn" ) )
            scope.voltage[ channel ].probeAttn = storeSettings->value( "probeAttn" ).toDouble();
        // У4а/У4б: прежде обе функции сидели в одном ключе digitalZoomN.
        // Старое значение переносится в разрядность; экранное увеличение
        // НЕ восстанавливается из него намеренно — прежняя связка «усреднение
        // всегда с увеличением √N» и была дефектом (`docs/PROTOTYPE-QUEUE.md` №7).
        if ( storeSettings->contains( "digitalZoomN" ) )
            scope.voltage[ channel ].resolutionN = storeSettings->value( "digitalZoomN" ).toUInt();
        if ( storeSettings->contains( "resolutionN" ) )
            scope.voltage[ channel ].resolutionN = storeSettings->value( "resolutionN" ).toUInt();
        if ( storeSettings->contains( "screenZoom" ) )
            scope.voltage[ channel ].screenZoom = storeSettings->value( "screenZoom" ).toDouble();
        if ( scope.voltage[ channel ].resolutionN < 1 )
            scope.voltage[ channel ].resolutionN = 1;
        if ( !( scope.voltage[ channel ].screenZoom > 0.0 ) )
            scope.voltage[ channel ].screenZoom = 1.0;
        if ( storeSettings->contains( "used" ) )
            scope.voltage[ channel ].used = storeSettings->value( "used" ).toBool();
        else                      // no config file found, e.g. 1st run
            defaultConfig = true; // start with default config

        // -- TZ §3.6 — CtPU: читается ОБХОДОМ РЕЕСТРА (params.h), а не
        // построчно. Ключи, вид и границы объявлены один раз в params.cpp;
        // здесь не должно быть знания о том, сколько у CtPU полей и как они
        // называются — иначе добавление пятого поля снова потребует правки
        // пяти файлов. Совместимость прежняя: отсутствующий ключ оставляет
        // значение по умолчанию.
        for ( const Params::Def *def : Params::group( QStringLiteral( "ctpu" ) ) ) {
            if ( !def->perChannel || !storeSettings->contains( def->key ) )
                continue;
            def->set( scope, channel, Params::clamp( *def, storeSettings->value( def->key ) ) );
        }
        if ( storeSettings->contains( "ccptuZeroV" ) )
            scope.voltage[ channel ].ccptuZeroV = storeSettings->value( "ccptuZeroV" ).toDouble();
        if ( storeSettings->contains( "ccptuSpanV" ) )
            scope.voltage[ channel ].ccptuSpanV = storeSettings->value( "ccptuSpanV" ).toDouble();
        if ( storeSettings->contains( "ccptuSpanPhysical" ) )
            scope.voltage[ channel ].ccptuSpanPhysical = storeSettings->value( "ccptuSpanPhysical" ).toDouble();

        if ( defaultConfig ) { // useful default: show both voltage channels
            setDefaultConfig();
        }

        storeSettings->beginGroup( "cursor" );
        if ( storeSettings->contains( "shape" ) )
            scope.voltage[ channel ].cursor.shape = DsoSettingsScopeCursor::CursorShape( storeSettings->value( "shape" ).toUInt() );
        for ( int marker = 0; marker < 2; ++marker ) {
            QString name;
            name = QString( "x%1" ).arg( marker );
            if ( storeSettings->contains( name ) )
                scope.voltage[ channel ].cursor.pos[ marker ].setX( storeSettings->value( name ).toDouble() );
            name = QString( "y%1" ).arg( marker );
            if ( storeSettings->contains( name ) )
                scope.voltage[ channel ].cursor.pos[ marker ].setY( storeSettings->value( name ).toDouble() );
        }
        storeSettings->endGroup(); // cursor
        storeSettings->endGroup(); // voltage%1
    }

    // Post processing
    if ( storeSettings->contains( "spectrumLimit" ) )
        analysis.spectrumLimit = storeSettings->value( "spectrumLimit" ).toDouble();
    if ( storeSettings->contains( "spectrumWindow" ) ) {
        analysis.spectrumWindow = Dso::WindowFunction( storeSettings->value( "spectrumWindow" ).toInt() );
        if ( analysis.spectrumWindow > Dso::LastWindowFunction )
            analysis.spectrumWindow = Dso::WindowFunction::HAMMING; // fall back to something useful
    }
    // Analysis
    storeSettings->beginGroup( "analysis" );
    if ( storeSettings->contains( "spectrumReference" ) )
        scope.analysis.spectrumReference = storeSettings->value( "spectrumReference" ).toDouble();
    if ( storeSettings->contains( "dBsuffixIndex" ) )
        scope.analysis.dBsuffixIndex = storeSettings->value( "dBsuffixIndex" ).toInt();
    if ( storeSettings->contains( "calculateDummyLoad" ) )
        scope.analysis.calculateDummyLoad = storeSettings->value( "calculateDummyLoad" ).toBool();
    if ( storeSettings->contains( "dummyLoad" ) )
        scope.analysis.dummyLoad = storeSettings->value( "dummyLoad" ).toUInt();
    if ( storeSettings->contains( "calculateTHD" ) )
        scope.analysis.calculateTHD = storeSettings->value( "calculateTHD" ).toBool();
    if ( storeSettings->contains( "reuseFftPlan" ) )
        analysis.reuseFftPlan = storeSettings->value( "reuseFftPlan" ).toBool();
    if ( storeSettings->contains( "showNoteValue" ) )
        scope.analysis.showNoteValue = storeSettings->value( "showNoteValue" ).toBool();
    storeSettings->endGroup(); // analysis
    storeSettings->endGroup(); // scope

    // View
    storeSettings->beginGroup( "view" );
    // Colors
    storeSettings->beginGroup( "color" );
    DsoSettingsColorValues *colors;
    for ( int mode = 0; mode < 2; ++mode ) {
        if ( mode == 0 ) {
            colors = &view.screen;
            storeSettings->beginGroup( "screen" );
        } else {
            colors = &view.print;
            storeSettings->beginGroup( "print" );
        }

        if ( storeSettings->contains( "axes" ) )
            colors->axes = storeSettings->value( "axes" ).value< QColor >();
        if ( storeSettings->contains( "background" ) )
            colors->background = storeSettings->value( "background" ).value< QColor >();
        if ( storeSettings->contains( "border" ) )
            colors->border = storeSettings->value( "border" ).value< QColor >();
        if ( storeSettings->contains( "grid" ) )
            colors->grid = storeSettings->value( "grid" ).value< QColor >();
        if ( storeSettings->contains( "markers" ) )
            colors->markers = storeSettings->value( "markers" ).value< QColor >();
        for ( ChannelID channel = 0; channel < scope.spectrum.size(); ++channel ) {
            QString key = QString( "spectrum%1" ).arg( channel );
            if ( storeSettings->contains( key ) )
                colors->spectrum[ channel ] = storeSettings->value( key ).value< QColor >();
        }
        if ( storeSettings->contains( "text" ) )
            colors->text = storeSettings->value( "text" ).value< QColor >();
        for ( ChannelID channel = 0; channel < scope.voltage.size(); ++channel ) {
            QString key = QString( "voltage%1" ).arg( channel );
            if ( storeSettings->contains( key ) )
                colors->voltage[ channel ] = storeSettings->value( key ).value< QColor >();
        }
        storeSettings->endGroup(); // screen / print
    }
    storeSettings->endGroup(); // color
    // Other view settings
    if ( storeSettings->contains( "histogram" ) )
        scope.histogram = storeSettings->value( "histogram" ).toBool();
    if ( storeSettings->contains( "digitalPhosphor" ) )
        view.digitalPhosphor = storeSettings->value( "digitalPhosphor" ).toBool();
    if ( storeSettings->contains( "interpolation" ) )
        view.interpolation = Dso::InterpolationMode( storeSettings->value( "interpolation" ).toInt() );
    if ( storeSettings->contains( "printerColorImages" ) )
        view.printerColorImages = storeSettings->value( "printerColorImages" ).toBool();
    if ( storeSettings->contains( "zoom" ) )
        view.zoom = storeSettings->value( "zoom" ).toBool();
    if ( storeSettings->contains( "zoomHeightIndex" ) )
        view.zoomHeightIndex = storeSettings->value( "zoomHeightIndex" ).toInt();
    if ( storeSettings->contains( "zoomImage" ) )
        view.zoomImage = storeSettings->value( "zoomImage" ).toBool();
    if ( storeSettings->contains( "exportScaleValue" ) )
        view.exportScaleValue = storeSettings->value( "exportScaleValue" ).toInt();
    if ( storeSettings->contains( "cursorGridPosition" ) )
        view.cursorGridPosition = Qt::ToolBarArea( storeSettings->value( "cursorGridPosition" ).toUInt() );
    if ( storeSettings->contains( "cursorsVisible" ) )
        view.cursorsVisible = storeSettings->value( "cursorsVisible" ).toBool();
    // Слой камеры: прибор, включённость и прозрачность переживают перезапуск -
    // «возможность выбрать И ЗАПОМНИТЬ выбор камеры» (распоряжение автора
    // 2026-09-02). Регуляторы драйвера сюда не пишутся: их хранит сам драйвер.
    storeSettings->endGroup(); // view

    loadCameraChoice();

    storeSettings->beginGroup( "window" );
    mainWindowGeometry = storeSettings->value( "geometry" ).toByteArray();
    mainWindowState = storeSettings->value( "state" ).toByteArray();
    storeSettings->endGroup(); // window
}


// save the persistent scope settings
// called by "DsoSettings::saveToFile()", "MainWindow::closeEvent" and explicitly by "ui->actionSave"
void DsoSettings::save() {
    // Use default configuration after restart?
    if ( 0 == configVersion ) {
        storeSettings->clear();
        if ( verboseLevel > 1 )
            qDebug() << " DsoSettings::save() storeSettings->clear() << storeSettings->fileName()";
        return;
    } else { // save fontSize as global setting
        QSettings().setValue( "view/fontSize", view.fontSize );
        QSettings().setValue( "view/toolTipVisible", scope.toolTipVisible );
        QSettings().setValue( "view/doNotTranslate", scope.doNotTranslate );
    }
    if ( verboseLevel > 1 )
        qDebug() << " DsoSettings::save()" << storeSettings->fileName();
    // now store individual device values

    // Date and Time of last storage
    storeSettings->beginGroup( "ConfigurationSaved" );
    storeSettings->setValue( "Date", QDate::currentDate().toString( "yyyy-MM-dd" ) );
    storeSettings->setValue( "Time", QTime::currentTime().toString( "HH:mm:ss" ) );
    storeSettings->endGroup(); // ConfigurationSaved

    // Device ID (helps to identify the connection of a "Save as" file with a specific device)
    storeSettings->beginGroup( "DeviceID" );
    storeSettings->setValue( "Model", deviceName );
    storeSettings->setValue( "SerialNumber", deviceID );
    storeSettings->endGroup(); // DeviceID

    // Configuration settings
    storeSettings->beginGroup( "configuration" );
    storeSettings->setValue( "version", configVersion );
    storeSettings->setValue( "alwaysSave", alwaysSave );
    storeSettings->endGroup(); // configuration

    // Oszilloskope settings
    storeSettings->beginGroup( "scope" );
    // Horizontal axis
    storeSettings->beginGroup( "horizontal" );
    storeSettings->setValue( "format", scope.horizontal.format );
    storeSettings->setValue( "frequencybase", scope.horizontal.frequencybase );
    for ( int marker = 0; marker < 2; ++marker )
        storeSettings->setValue( QString( "marker%1" ).arg( marker ), scope.getMarker( marker ) );
    storeSettings->setValue( "timebase", scope.horizontal.timebase );
    storeSettings->setValue( "maxTimebase", scope.horizontal.maxTimebase );
    storeSettings->setValue( "acquireInterval", scope.horizontal.acquireInterval );
    storeSettings->setValue( "recordLength", scope.horizontal.recordLength );
    storeSettings->setValue( "samplerate", scope.horizontal.samplerate );
    storeSettings->setValue( "calfreq", scope.horizontal.calfreq );
    // -- У8 — XY chart-recorder configuration save
    storeSettings->setValue( "xyUseBinTape", scope.horizontal.xyUseBinTape );
    storeSettings->setValue( "xyBinCount", scope.horizontal.xyBinCount );
    storeSettings->setValue( "xyMedianWindow", scope.horizontal.xyMedianWindow );
    storeSettings->setValue( "xyFieldBinsX", scope.horizontal.xyFieldBinsX );
    storeSettings->setValue( "xyFieldBinsY", scope.horizontal.xyFieldBinsY );
    storeSettings->setValue( "xyMasterAxis", scope.horizontal.xyMasterAxis );
    storeSettings->setValue( "xySheetMode", scope.horizontal.xySheetMode );
    storeSettings->setValue( "xySlewRateX", scope.horizontal.xySlewRateX );
    storeSettings->setValue( "xySlewRateY", scope.horizontal.xySlewRateY );
    storeSettings->setValue( "xyTargetPoints", scope.horizontal.xyTargetPoints );
    storeSettings->setValue( "xyTargetDensity", scope.horizontal.xyTargetDensity );
    storeSettings->setValue( "xyTrackSigma", scope.horizontal.xyTrackSigma );
    storeSettings->setValue( "xyExtractMode", scope.horizontal.xyExtractMode );
    storeSettings->setValue( "xyTimeSecPerDiv", scope.horizontal.xyTimeSecPerDiv );
    // -- TZ §7 — XY multi-curve configuration save
    for ( int i = 0; i < DsoSettingsScope::maxXYCurves; ++i ) {
        storeSettings->beginGroup( QString( "xyCurves/curve%1" ).arg( i ) );
        storeSettings->setValue( "enabled", scope.xyCurves[ i ].enabled );
        storeSettings->setValue( "xChannel", scope.xyCurves[ i ].xChannel );
        storeSettings->setValue( "yChannel", scope.xyCurves[ i ].yChannel );
        storeSettings->setValue( "color", scope.xyCurves[ i ].explicitColor.isValid()
                                              ? scope.xyCurves[ i ].explicitColor.name()
                                              : QString() );
        storeSettings->endGroup();
    }
    storeSettings->endGroup(); // horizontal
    // -- TZ §5 — math-stack configuration save
    for ( int i = 0; i < DsoSettingsScope::maxMathChannels; ++i ) {
        storeSettings->beginGroup( QString( "mathStack/math%1" ).arg( i ) );
        storeSettings->setValue( "enabled", scope.mathStack[ i ].enabled );
        storeSettings->setValue( "srcA", scope.mathStack[ i ].srcA );
        storeSettings->setValue( "srcB", scope.mathStack[ i ].srcB );
        storeSettings->setValue( "op", unsigned( scope.mathStack[ i ].op ) );
        storeSettings->setValue( "invert", scope.mathStack[ i ].invert );
        storeSettings->setValue( "ctpuUnit", scope.mathStack[ i ].ctpuUnit );
        storeSettings->setValue( "ctpuK", scope.mathStack[ i ].ctpuK );
        storeSettings->setValue( "ctpuB", scope.mathStack[ i ].ctpuB );
        storeSettings->endGroup();
    }
    // Trigger
    storeSettings->beginGroup( "trigger" );
    storeSettings->setValue( "mode", unsigned( scope.trigger.mode ) );
    storeSettings->setValue( "position", scope.trigger.position );
    storeSettings->setValue( "slope", unsigned( scope.trigger.slope ) );
    storeSettings->setValue( "source", scope.trigger.source );
    storeSettings->setValue( "smooth", scope.trigger.smooth );
    storeSettings->endGroup(); // trigger
    // Spectrum
    for ( ChannelID channel = 0; channel < scope.spectrum.size(); ++channel ) {
        storeSettings->beginGroup( QString( "spectrum%1" ).arg( channel ) );
        storeSettings->setValue( "magnitude", scope.spectrum[ channel ].magnitude );
        storeSettings->setValue( "offset", scope.spectrum[ channel ].offset );
        storeSettings->setValue( "used", scope.spectrum[ channel ].used );
        storeSettings->beginGroup( "cursor" );
        storeSettings->setValue( "shape", scope.spectrum[ channel ].cursor.shape );
        for ( int marker = 0; marker < 2; ++marker ) {
            QString name;
            name = QString( "x%1" ).arg( marker );
            storeSettings->setValue( name, scope.spectrum[ channel ].cursor.pos[ marker ].x() );
            name = QString( "y%1" ).arg( marker );
            storeSettings->setValue( name, scope.spectrum[ channel ].cursor.pos[ marker ].y() );
        }
        storeSettings->endGroup(); // cursor
        storeSettings->endGroup(); // spectrum%1
    }
    // Voltage
    storeSettings->setValue( "hasACmodification", scope.hasACmodification );
    for ( ChannelID channel = 0; channel < scope.voltage.size(); ++channel ) {
        storeSettings->beginGroup( QString( "voltage%1" ).arg( channel ) );
        storeSettings->setValue( "gainStepIndex", scope.voltage[ channel ].gainStepIndex );
        storeSettings->setValue( "couplingOrMathIndex", scope.voltage[ channel ].couplingOrMathIndex );
        storeSettings->setValue( "inverted", scope.voltage[ channel ].inverted );
        storeSettings->setValue( "offset", scope.voltage[ channel ].offset );
        storeSettings->setValue( "trigger", scope.voltage[ channel ].trigger );
        storeSettings->setValue( "used", scope.voltage[ channel ].used );
        storeSettings->setValue( "probeAttn", scope.voltage[ channel ].probeAttn );
        storeSettings->setValue( "resolutionN", scope.voltage[ channel ].resolutionN );
        storeSettings->setValue( "screenZoom", scope.voltage[ channel ].screenZoom );
        // -- TZ §3.6 — CtPU: пишется обходом реестра (params.h), см. чтение.
        for ( const Params::Def *def : Params::group( QStringLiteral( "ctpu" ) ) ) {
            if ( !def->perChannel )
                continue;
            storeSettings->setValue( def->key, def->get( scope, channel ) );
        }
        storeSettings->setValue( "ccptuZeroV", scope.voltage[ channel ].ccptuZeroV );
        storeSettings->setValue( "ccptuSpanV", scope.voltage[ channel ].ccptuSpanV );
        storeSettings->setValue( "ccptuSpanPhysical", scope.voltage[ channel ].ccptuSpanPhysical );
        storeSettings->beginGroup( "cursor" );
        storeSettings->setValue( "shape", scope.voltage[ channel ].cursor.shape );
        for ( int marker = 0; marker < 2; ++marker ) {
            QString name;
            name = QString( "x%1" ).arg( marker );
            storeSettings->setValue( name, scope.voltage[ channel ].cursor.pos[ marker ].x() );
            name = QString( "y%1" ).arg( marker );
            storeSettings->setValue( name, scope.voltage[ channel ].cursor.pos[ marker ].y() );
        }
        storeSettings->endGroup(); // cursor
        storeSettings->endGroup(); // voltage%1
    }

    // Post processing
    storeSettings->setValue( "spectrumLimit", analysis.spectrumLimit );
    storeSettings->setValue( "spectrumWindow", unsigned( analysis.spectrumWindow ) );

    // Analysis
    storeSettings->beginGroup( "analysis" );
    storeSettings->setValue( "spectrumReference", scope.analysis.spectrumReference );
    storeSettings->setValue( "dBsuffixIndex", scope.analysis.dBsuffixIndex );
    storeSettings->setValue( "calculateDummyLoad", scope.analysis.calculateDummyLoad );
    storeSettings->setValue( "dummyLoad", scope.analysis.dummyLoad );
    storeSettings->setValue( "calculateTHD", scope.analysis.calculateTHD );
    storeSettings->setValue( "reuseFftPlan", analysis.reuseFftPlan );
    storeSettings->setValue( "showNoteValue", scope.analysis.showNoteValue );
    storeSettings->endGroup(); // analysis
    storeSettings->endGroup(); // scope

    // View
    storeSettings->beginGroup( "view" );
    // Colors
    storeSettings->beginGroup( "color" );
    DsoSettingsColorValues *colors;
    for ( int mode = 0; mode < 2; ++mode ) {
        if ( mode == 0 ) {
            colors = &view.screen;
            storeSettings->beginGroup( "screen" );
        } else {
            colors = &view.print;
            storeSettings->beginGroup( "print" );
        }

        storeSettings->setValue( "axes", colors->axes.name( QColor::HexArgb ) );
        storeSettings->setValue( "background", colors->background.name( QColor::HexArgb ) );
        storeSettings->setValue( "border", colors->border.name( QColor::HexArgb ) );
        storeSettings->setValue( "grid", colors->grid.name( QColor::HexArgb ) );
        storeSettings->setValue( "markers", colors->markers.name( QColor::HexArgb ) );
        for ( ChannelID channel = 0; channel < scope.spectrum.size(); ++channel )
            storeSettings->setValue( QString( "spectrum%1" ).arg( channel ), colors->spectrum[ channel ].name( QColor::HexArgb ) );
        storeSettings->setValue( "text", colors->text.name( QColor::HexArgb ) );
        for ( ChannelID channel = 0; channel < scope.voltage.size(); ++channel )
            storeSettings->setValue( QString( "voltage%1" ).arg( channel ), colors->voltage[ channel ].name( QColor::HexArgb ) );
        storeSettings->endGroup(); // screen / print
    }
    storeSettings->endGroup(); // color

    // Other view settings
    storeSettings->setValue( "histogram", scope.histogram );
    storeSettings->setValue( "digitalPhosphor", view.digitalPhosphor );
    storeSettings->setValue( "interpolation", view.interpolation );
    storeSettings->setValue( "printerColorImages", view.printerColorImages );
    storeSettings->setValue( "zoom", view.zoom );
    storeSettings->setValue( "zoomHeightIndex", view.zoomHeightIndex );
    storeSettings->setValue( "zoomImage", view.zoomImage );
    storeSettings->setValue( "exportScaleValue", view.exportScaleValue );
    storeSettings->setValue( "cursorGridPosition", view.cursorGridPosition );
    storeSettings->setValue( "cursorsVisible", view.cursorsVisible );
    storeSettings->endGroup(); // view

    // Program window geometry and state
    storeSettings->beginGroup( "window" );
    storeSettings->setValue( "geometry", mainWindowGeometry );
    storeSettings->setValue( "state", mainWindowState );
    storeSettings->endGroup(); // window
}


void DsoSettings::setDefaultConfig() {
    scope.voltage[ 0 ].used = true;
    scope.voltage[ 0 ].offset = MARGIN_TOP / 2; // mid of upper screen half
    scope.voltage[ 1 ].used = true;
    scope.voltage[ 1 ].offset = MARGIN_BOTTOM / 2; // mid of lower screen half
}


/// Прочитать выбор камеры.
///
/// Камера принадлежит МАШИНЕ, а не осциллографу: она не меняется от того, какой
/// прибор подключён, и не должна теряться при смене прибора или запуске в
/// демо-режиме. Поэтому выбор лежит там же, где `view/fontSize` и
/// `view/toolTipVisible`, — в общих настройках программы, а не в файле
/// настроек прибора.
///
/// Одно место записи и одно чтения: `save()` этих ключей не касается вовсе.
void DsoSettings::loadCameraChoice() {
    QSettings global;
    global.beginGroup( "camera" );
    if ( global.contains( "deviceId" ) )
        view.cameraDeviceId = global.value( "deviceId" ).toString();
    if ( global.contains( "layerEnabled" ) )
        view.cameraLayerEnabled = global.value( "layerEnabled" ).toBool();
    if ( global.contains( "layerOpacity" ) )
        view.cameraLayerOpacity = global.value( "layerOpacity" ).toDouble();
    if ( global.contains( "rotation" ) )
        view.cameraRotation = global.value( "rotation" ).toInt();
    if ( global.contains( "mirrorH" ) )
        view.cameraMirrorH = global.value( "mirrorH" ).toBool();
    if ( global.contains( "mirrorV" ) )
        view.cameraMirrorV = global.value( "mirrorV" ).toBool();
    global.endGroup();
}


/// Записать выбор камеры немедленно.
///
/// Немедленно, а не при выходе: сохранение при выходе висит на `alwaysSave`, и
/// при выключенном флаге явно сделанный оператором выбор пропадал бы молча.
/// Оператор нажал «Применить» - выбор записан.
void DsoSettings::saveCameraChoice() {
    QSettings global;
    global.beginGroup( "camera" );
    global.setValue( "deviceId", view.cameraDeviceId );
    global.setValue( "layerEnabled", view.cameraLayerEnabled );
    global.setValue( "layerOpacity", view.cameraLayerOpacity );
    global.setValue( "rotation", view.cameraRotation );
    global.setValue( "mirrorH", view.cameraMirrorH );
    global.setValue( "mirrorV", view.cameraMirrorV );
    global.endGroup();
    global.sync();
}
