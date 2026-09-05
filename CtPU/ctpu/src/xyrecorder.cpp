// SPDX-License-Identifier: GPL-2.0-or-later
// Last edited: 2026-08-11 11:31:17 UTC

#include "xyrecorder.h"

#include "hantekdso/controlspecification.h"
#include "post/ppresult.h"
#include "scopesettings.h"
#include "viewconstants.h"

#include <QDateTime>
#include <QtGlobal>

#include <algorithm>
#include <cmath>

XYRecorder::~XYRecorder() { finalize(); }


void XYRecorder::configure( DsoSettingsScope *scopeIn, const Dso::ControlSpecification *specIn, const Config &cfg ) {
    finalize(); // close/flush any previous streaming file before reconfiguring

    scope = scopeIn;
    spec = specIn;
    config = cfg;
    rebuildCascade(); // clears cascade + traj

    if ( config.sheetMode == SheetMode::TAPE && !config.tapeFilePath.isEmpty() ) {
        tapeFile.setFileName( config.tapeFilePath );
        if ( tapeFile.open( QIODevice::WriteOnly | QIODevice::Text ) ) {
            tapeStream.setDevice( &tapeFile );
            writeHeader( tapeStream );
            // [FIX] TZ §7.6.3 — column header reflects this recorder's bound
            // channels (from m_curveConfig), not the hardcoded CH1/CH2 pair.
            QString xName = QStringLiteral( "CH1" );
            QString yName = QStringLiteral( "CH2" );
            if ( scope && m_curveConfig.xChannel < scope->voltage.size() )
                xName = scope->voltage[ m_curveConfig.xChannel ].name;
            if ( scope && m_curveConfig.yChannel < scope->voltage.size() )
                yName = scope->voltage[ m_curveConfig.yChannel ].name;
            tapeStream << "X(" << xName << "),Y(" << yName << "),Sigma\n";
        }
        // If open() fails, tapeFile stays closed -> emitPoint() falls back to
        // the bounded-RAM ring behaviour (isStreamingToDisk() == false), so
        // recording still works, just without persistence. Caller should
        // check isStreamingToDisk() after configure() to warn the user.
    }
}


void XYRecorder::rebuildCascade() {
    cascade.clear();
    traj.clear();
    m_renderDirty = true;
    m_timeBaseMs = -1.0; // У3: новая запись — новая база оси времени
    m_timeAccumS = 0.0;
    // Поле XY (xyfield.h): сетка задаётся ДВУМЯ независимыми числами ячеек.
    m_field.configure( config.fieldBinsX, config.fieldBinsY, config.fieldXMin, config.fieldXMax, config.fieldYMin,
                       config.fieldYMax, config.fieldAutoRange );

    if ( config.useBinTape ) {
        // Модель самописца: ни slewRate, ни cascadeBase, ни предсказание
        // длительности развёртки не используются — число бинов задано прямо,
        // а ёмкость бина сама удваивается по мере надобности (см. bintape.h).
        m_tape = BinTape::Tape( config.binCount, 1 );
        safetyCap = 0; // не применяется: стирания нет по построению
        return;
    }

    const double samplerate = scope ? scope->horizontal.samplerate : 1e6;

    const double slewRate =
        qBound( 1e-4, config.masterAxis == MasterAxis::X ? config.slewRateX : config.slewRateY, 1e7 );

    // [FIX] TZ §7.4 — use the channel actually bound to this curve's master
    // axis (via setCurveConfig()) instead of hardcoded CH1=0/CH2=1. Before
    // this fix, every curve's cascade depth (and therefore its point
    // density / smoothing) was sized from CH1's or CH2's V/div setting
    // regardless of which channels the curve was actually configured to
    // record — correct only by coincidence for the default CH1×CH2 curve 0.
    const unsigned masterChannel = config.masterAxis == MasterAxis::X ? m_curveConfig.xChannel : m_curveConfig.yChannel;
    const double fullScaleRange = ( scope && masterChannel < scope->voltage.size() )
                                       ? scope->gain( masterChannel ) * DIVS_VOLTAGE
                                       : DIVS_VOLTAGE; // fallback if scope not wired yet

    double decimationFactor;
    if ( config.sheetMode == SheetMode::FINITE ) {
        const double sweepDuration = fullScaleRange / slewRate; // s, master axis full-scale crossing
        const double targetPts = double( std::max< std::size_t >( 1, config.targetPoints ) );
        decimationFactor = ( samplerate * sweepDuration ) / targetPts;
    } else { // TAPE
        const double density = config.targetDensity > 0.0 ? config.targetDensity : 1.0;
        decimationFactor = samplerate / density;
    }
    decimationFactor = std::max( 1.0, decimationFactor );

    const int base = qMax( 2, config.cascadeBase );
    int depth = int( std::ceil( std::log( decimationFactor ) / std::log( double( base ) ) ) );
    depth = qMax( 1, depth );
    cascade.assign( std::size_t( depth ), CascadeStage() );

    safetyCap = config.safetyCapPoints;
    if ( safetyCap == 0 )
        safetyCap = ( config.sheetMode == SheetMode::FINITE ) ? config.targetPoints * 10 : config.renderWindowPoints;
}


void XYRecorder::addFrame( const PPresult *data ) {
    // [MOD] cascade.empty() больше не является признаком "не сконфигурирован":
    // при useBinTape=true каскад намеренно пуст, источник истины — m_tape.
    if ( !data )
        return;
    if ( !config.useBinTape && cascade.empty() )
        return;

    // [FIX] TZ §7.4 — use this recorder's bound (xChannel, yChannel) pair
    // instead of the hardcoded CH1=0/CH2=1. The binding is set by
    // DsoWidget::configureXYRecorder() via setCurveConfig() and reflects
    // scope->xyCurves[i] for this specific curve slot. Without this fix all
    // four recorders would record the same CH1×CH2 trajectory regardless of
    // the user's per-curve channel selection.
    // У3 — TimeChannel: любая из осей может быть осью времени. Время
    // синтезируется честно по timechannel.h: intra = i/samplerate (измерено),
    // epoch = wall-clock кадра (оценка с USB-джиттером ~мс). Время×время
    // смысла не имеет — кадр пропускается.
    const bool timeX = ( m_curveConfig.xChannel == DsoSettingsScope::timeChannelIndex );
    const bool timeY = ( m_curveConfig.yChannel == DsoSettingsScope::timeChannelIndex );
    if ( timeX && timeY )
        return;
    const DataChannel *chX = timeX ? nullptr : data->data( m_curveConfig.xChannel );
    const DataChannel *chY = timeY ? nullptr : data->data( m_curveConfig.yChannel );
    if ( ( !timeX && !chX ) || ( !timeY && !chY ) )
        return;

    const DataChannel *sigCh = timeX ? chY : chX; // канал, задающий размер кадра
    static const std::vector< double > kEmpty;
    const auto &sx = chX ? chX->voltage.samples : kEmpty;
    const auto &sy = chY ? chY->voltage.samples : kEmpty;
    const std::size_t n = ( timeX || timeY ) ? sigCh->voltage.samples.size() : std::min( sx.size(), sy.size() );
    if ( n == 0 )
        return;

    // Начало кадра на оси времени, с (относительно начала записи).
    double frameT0 = 0.0;
    const double dt = sigCh->voltage.interval; // s/отсчёт = intra-шаг
    if ( timeX || timeY ) {
        if ( data->captureTimestampMs > 0 ) {
            if ( m_timeBaseMs < 0.0 )
                m_timeBaseMs = double( data->captureTimestampMs );
            frameT0 = ( double( data->captureTimestampMs ) - m_timeBaseMs ) / 1000.0;
        } else {
            frameT0 = m_timeAccumS; // синтетика без метки: непрерывная склейка
        }
        m_timeAccumS += double( n ) * dt;
    }
    auto axisValue = [ & ]( bool isTime, const std::vector< double > &v, std::size_t i ) {
        return isTime ? frameT0 + double( i ) * dt : v[ i ];
    };

    // Поле XY (xyfield.h) наполняется ВСЕГДА, независимо от модели
    // прореживания: это отдельная сущность — настоящая XY-запись по
    // значениям осей. Лента усредняет по времени и форму не хранит,
    // поле хранит. Медиана к полю не применяется: она сдвигает значения,
    // а поле должно показывать то, что действительно было на входе.
    if ( config.buildField ) {
        const std::int64_t tFieldMs =
            data->captureTimestampMs > 0 ? data->captureTimestampMs : QDateTime::currentMSecsSinceEpoch();
        for ( std::size_t i = 0; i < n; ++i )
            m_field.addSample( axisValue( timeX, sx, i ), axisValue( timeY, sy, i ), tFieldMs );
    }

    if ( config.useBinTape ) {
        // Модель самописца (bintape.h). Медианный предфильтр — ДО бинирования:
        // снимает коммутационные выбросы и звон, за счёт чего min/max бина
        // означают реальный охват сигнала. Остаток r = исходное - фильтрованное
        // передаётся в ленту, чтобы энергетический баланс сходился точно
        // (Sum(x^2) = Sum(y^2) + Sum(r^2) + 2*Sum(y*r)) и было видно, что
        // именно ушло в отвал и с какой амплитудой.
        const std::int64_t tMs = QDateTime::currentMSecsSinceEpoch();
        if ( config.medianWindow > 1 ) {
            // У3: медианный предфильтр применяется только к СИГНАЛЬНЫМ
            // компонентам — ось времени монотонна, её «звона» не бывает,
            // и отвал (residual) времени тождественно нулевой.
            std::vector< double > inX( n ), inY( n );
            for ( std::size_t i = 0; i < n; ++i ) {
                inX[ i ] = axisValue( timeX, sx, i );
                inY[ i ] = axisValue( timeY, sy, i );
            }
            const std::vector< double > fX = timeX ? inX : MedianFilter::apply( inX, config.medianWindow );
            const std::vector< double > fY = timeY ? inY : MedianFilter::apply( inY, config.medianWindow );
            for ( std::size_t i = 0; i < n; ++i )
                m_tape.addSample( fX[ i ], fY[ i ], tMs, i == 0, inX[ i ] - fX[ i ], inY[ i ] - fY[ i ] );
        } else {
            for ( std::size_t i = 0; i < n; ++i )
                m_tape.addSample( axisValue( timeX, sx, i ), axisValue( timeY, sy, i ), tMs, i == 0, 0.0, 0.0 );
        }
        m_renderDirty = true;
        return;
    }

    for ( std::size_t i = 0; i < n; ++i )
        feedStage( 0, axisValue( timeX, sx, i ), axisValue( timeY, sy, i ), 0.0 );
}


/// Box-car mean per stage, flushed to the next stage every cascadeBase
/// samples -> low-pass *before* decimation at every level, so the final
/// output stays smooth instead of aliasing (unlike plain stride-decimate).
void XYRecorder::feedStage( std::size_t stageIndex, double x, double y, double sigma ) {
    if ( stageIndex >= cascade.size() ) {
        emitPoint( x, y, sigma );
        return;
    }

    CascadeStage &st = cascade[ stageIndex ];
    const bool isFinal = ( stageIndex == cascade.size() - 1 );

    if ( st.count == 0 ) {
        st.minX = st.maxX = x;
        st.minY = st.maxY = y;
    }
    st.sumX += x;
    st.sumY += y;
    st.minX = std::min( st.minX, x );
    st.maxX = std::max( st.maxX, x );
    st.minY = std::min( st.minY, y );
    st.maxY = std::max( st.maxY, y );
    if ( isFinal && config.trackSigma ) {
        st.sumX2 += x * x;
        st.sumY2 += y * y;
    }
    ++st.count;

    if ( st.count < config.cascadeBase )
        return;

    const double meanX = st.sumX / st.count;
    const double meanY = st.sumY / st.count;

    double outSigma = 0.0;
    if ( isFinal && config.trackSigma ) {
        const double varX = std::max( 0.0, st.sumX2 / st.count - meanX * meanX );
        const double varY = std::max( 0.0, st.sumY2 / st.count - meanY * meanY );
        outSigma = std::sqrt( varX + varY );
    }

    if ( isFinal && config.extractMode == ExtractMode::PEAK_ENVELOPE ) {
        feedStage( stageIndex + 1, st.minX, st.minY, 0.0 );
        if ( st.maxX != st.minX || st.maxY != st.minY )
            feedStage( stageIndex + 1, st.maxX, st.maxY, 0.0 );
    } else {
        feedStage( stageIndex + 1, meanX, meanY, outSigma );
    }

    st = CascadeStage(); // reset for the next block
}


void XYRecorder::emitPoint( double x, double y, double sigma ) {
    traj.push_back( { x, y, sigma } );

    if ( tapeFile.isOpen() ) {
        // Streaming: once the render window is exceeded, flush the oldest
        // chunk to disk instead of dropping it - nothing is lost.
        const std::size_t chunk =
            config.flushChunkPoints ? config.flushChunkPoints : std::max< std::size_t >( 1, config.renderWindowPoints / 2 );
        if ( traj.size() > config.renderWindowPoints + chunk )
            flushChunkToDisk( chunk );
    } else if ( safetyCap && traj.size() > safetyCap ) {
        traj.pop_front(); // no streaming target configured -> bounded-RAM fallback, data IS lost
    }
}


void XYRecorder::flushChunkToDisk( std::size_t count ) {
    count = std::min( count, traj.size() );
    for ( std::size_t i = 0; i < count; ++i ) {
        const Point &p = traj[ i ];
        tapeStream << p.x << "," << p.y << "," << p.sigma << "\n";
    }
    traj.erase( traj.begin(), traj.begin() + long( count ) );
    tapeStream.flush();
}


void XYRecorder::finalize() {
    if ( tapeFile.isOpen() ) {
        flushChunkToDisk( traj.size() ); // write everything still buffered
        tapeStream.flush();
        tapeFile.close();
    }
}


void XYRecorder::clear() {
    traj.clear();
    m_renderDirty = true;
    m_tape.clear();
    m_field.clear();
    for ( auto &st : cascade )
        st = CascadeStage();
}


void XYRecorder::writeHeader( QTextStream &out ) const {
    out << "# XY Recorder settings\n";
    // Метка времени экспорта, ISO 8601 (не по локали — должна оставаться
    // однозначной и сортируемой), как и в exportcsv.cpp.
    out << "# Exported: " << QDateTime::currentDateTime().toString( Qt::ISODateWithMs ) << "\n";
    out << "# sheetMode=" << ( config.sheetMode == SheetMode::FINITE ? "FINITE" : "TAPE" ) << "\n";
    out << "# masterAxis=" << ( config.masterAxis == MasterAxis::X ? "X" : "Y" ) << "\n";
    if ( config.useBinTape ) {
        // Модель самописца. Пишем ТОЛЬКО то, что реально участвовало в
        // результате: slewRate/cascadeBase/targetDensity здесь не
        // используются вовсе, и указывать их означало бы вводить в
        // заблуждение того, кто потом читает файл.
        out << "# sizing=BINTAPE\n";
        out << "# binCount=" << qulonglong( config.binCount ) << "\n";
        out << "# binsUsed=" << qulonglong( m_tape.bins().size() ) << "\n";
        out << "# samplesPerBin=" << qulonglong( m_tape.samplesPerBin() ) << "\n";
        out << "# totalSamples=" << qulonglong( m_tape.totalSamples() ) << "\n";
        out << "# medianWindow=" << config.medianWindow << "\n";
        out << "# note=bins are TAPE positions (record order), not signal values;"
               " nothing is discarded, merging only coarsens resolution\n";
        out << "# WARNING=this file is a TIME AVERAGE along the tape, NOT an XY record:"
               " each row is the mean of one tape segment, so the waveform shape is NOT"
               " recoverable from it. For a true XY record (value bins on both axes)"
               " use the XY field export.\n";
    } else {
        out << "# sizing=CASCADE(legacy)\n";
        out << "# slewRateX_V_per_s=" << config.slewRateX << "\n";
        out << "# slewRateY_V_per_s=" << config.slewRateY << "\n";
        if ( config.sheetMode == SheetMode::FINITE )
            out << "# targetPoints=" << qulonglong( config.targetPoints ) << "\n";
        else
            out << "# targetDensity_pts_per_s=" << config.targetDensity << "\n";
        out << "# cascadeBase=" << config.cascadeBase << " cascadeDepth=" << qulonglong( cascade.size() ) << "\n";
        out << "# extractMode=" << ( config.extractMode == ExtractMode::PEAK_ENVELOPE ? "PEAK_ENVELOPE" : "CASCADE" )
            << "\n";
        out << "# trackSigma=" << ( config.trackSigma ? "true" : "false" ) << "\n";
    }

    if ( scope ) {
        out << "# samplerate_Hz=" << scope->horizontal.samplerate << "\n";
        out << "# timebase_s_per_div=" << scope->horizontal.timebase << "\n";
        out << "# format=" << Dso::graphFormatString( scope->horizontal.format ) << "\n";
        out << "# trigger.mode=" << Dso::triggerModeString( scope->trigger.mode )
            << " slope=" << Dso::slopeString( scope->trigger.slope ) << " source=" << scope->trigger.source
            << " position=" << scope->trigger.position << "\n";
        // [FIX] TZ §7.6.3 — emit metadata for this recorder's bound X and Y
        // channels (from m_curveConfig), not the hardcoded CH1/CH2 pair.
        // This ensures the CSV header matches the actual data in the file
        // when exporting curves whose xChannel/yChannel differ from 0/1.
        const auto &cfg = m_curveConfig;
        if ( cfg.xChannel < scope->voltage.size() ) {
            const auto &v = scope->voltage[ cfg.xChannel ];
            out << "# X_channel: name=" << v.name << " gain=" << scope->physicalGain( cfg.xChannel ) << " "
                << v.ctpuUnit << "/div"
                << " offset_div=" << v.offset << " probeAttn=" << v.probeAttn
                << " inverted=" << ( v.inverted ? "true" : "false" );
            if ( spec && cfg.xChannel < spec->channels )
                out << " coupling=" << Dso::couplingString( scope->coupling( ChannelID( cfg.xChannel ), spec ) );
            out << "\n";
        }
        if ( cfg.yChannel < scope->voltage.size() ) {
            const auto &v = scope->voltage[ cfg.yChannel ];
            out << "# Y_channel: name=" << v.name << " gain=" << scope->physicalGain( cfg.yChannel ) << " "
                << v.ctpuUnit << "/div"
                << " offset_div=" << v.offset << " probeAttn=" << v.probeAttn
                << " inverted=" << ( v.inverted ? "true" : "false" );
            if ( spec && cfg.yChannel < spec->channels )
                out << " coupling=" << Dso::couplingString( scope->coupling( ChannelID( cfg.yChannel ), spec ) );
            out << "\n";
        }
    }
    out << "#\n";
}


void XYRecorder::exportFieldCSV( const QString &filename ) const {
    // НАСТОЯЩАЯ XY-ЗАПИСЬ (замечание пользователя 2026-08-21).
    // Экспорт ленты (exportCSV) даёт два столбца средних по ВРЕМЕНИ — по нему
    // форму сигнала восстановить нельзя, и это честно сказано в его шапке.
    // Здесь бинируются ЗНАЧЕНИЯ обеих осей: ячейка = участок плоскости X×Y,
    // значение = сколько раз траектория там побывала (плотность посещений,
    // она же цифровой фосфор). Число ячеек по осям НЕЗАВИСИМО — потому и
    // строк столько, сколько реально посещено, а не квадрат одного числа.
    QFile file( filename );
    if ( !file.open( QIODevice::WriteOnly | QIODevice::Text ) )
        return;
    QTextStream out( &file );
    out.setCodec( "UTF-8" ); // кириллические имена каналов; локальная кодировка давала мусор в шапке
    XYRecorder *self = const_cast< XYRecorder * >( this );
    self->m_field.flushPending(); // короткая запись: досыпать отложенное
    const XYField::Field &f = m_field;

    out << "# XY field (density of visits over the X-Y plane)\n";
    out << "# Exported: " << QDateTime::currentDateTime().toString( Qt::ISODateWithMs ) << "\n";
    out << "# binsX=" << qulonglong( f.binsX() ) << "\n";
    out << "# binsY=" << qulonglong( f.binsY() ) << "\n";
    out << "# xMin=" << f.xMin() << " xMax=" << f.xMax() << " stepX=" << f.stepX() << "\n";
    out << "# yMin=" << f.yMin() << " yMax=" << f.yMax() << " stepY=" << f.stepY() << "\n";
    out << "# totalSamples=" << qulonglong( f.totalSamples() ) << "\n";
    out << "# occupiedCells=" << qulonglong( f.occupiedCells() ) << "\n";
    out << "# maxCount=" << qulonglong( f.maxCount() ) << "\n";
    out << "# clippedX=" << qulonglong( f.clippedX() ) << " clippedY=" << qulonglong( f.clippedY() )
        << "  (samples outside the grid range: beyond the edge, not on it)\n";
    out << "# rangeMode=" << ( config.fieldAutoRange ? "auto (from data)" : "explicit (channel full scale)" ) << "\n";
    out << "# note=cells are SIGNAL VALUE bins on both axes, so the waveform shape survives;"
           " a square wave gives two dense regions, an I-V curve gives its curve\n";
    if ( scope ) {
        out << "# samplerate_Hz=" << scope->horizontal.samplerate << "\n";
        const auto &cfg = m_curveConfig;
        if ( cfg.xChannel < scope->voltage.size() )
            out << "# X_channel: name=" << scope->voltage[ cfg.xChannel ].name
                << " unit=" << scope->voltage[ cfg.xChannel ].ctpuUnit << "\n";
        if ( cfg.yChannel < scope->voltage.size() )
            out << "# Y_channel: name=" << scope->voltage[ cfg.yChannel ].name
                << " unit=" << scope->voltage[ cfg.yChannel ].ctpuUnit << "\n";
    }
    out << "#\n";
    out << "ix,iy,x_center,y_center,count,t_first_ms,t_last_ms\n";
    for ( std::size_t iy = 0; iy < f.binsY(); ++iy ) {
        for ( std::size_t ix = 0; ix < f.binsX(); ++ix ) {
            const XYField::Cell &c = f.at( ix, iy );
            if ( !c.count )
                continue; // пустые ячейки не пишем: файл и так покрывает плоскость сеткой
            out << qulonglong( ix ) << "," << qulonglong( iy ) << "," << f.xCenter( ix ) << "," << f.yCenter( iy ) << ","
                << qulonglong( c.count ) << "," << qlonglong( c.tFirstMs ) << "," << qlonglong( c.tLastMs ) << "\n";
        }
    }
    file.close();
}


void XYRecorder::exportCSV( const QString &filename ) const {
    QFile file( filename );
    if ( !file.open( QIODevice::WriteOnly | QIODevice::Text ) )
        return;
    QTextStream out( &file );
    out.setCodec( "UTF-8" ); // кириллические имена каналов; локальная кодировка давала мусор в шапке
    writeHeader( out );
    // [FIX] TZ §7.6.3 — use this recorder's own m_curveConfig (bound via
    // setCurveConfig()) instead of scope->xyCurves[0]. Without this fix,
    // exporting curves 1–3 would label the columns with curve 0's channels.
    const auto &cfg = m_curveConfig;
    if ( scope && cfg.xChannel < scope->voltage.size() && cfg.yChannel < scope->voltage.size() ) {
        const auto &xv = scope->voltage[ cfg.xChannel ];
        const auto &yv = scope->voltage[ cfg.yChannel ];
        out << "# X: " << xv.name << ", Unit: " << xv.ctpuUnit << ", Gain: " << scope->physicalGain( cfg.xChannel ) << " "
            << xv.ctpuUnit << "/div\n";
        out << "# Y: " << yv.name << ", Unit: " << yv.ctpuUnit << ", Gain: " << scope->physicalGain( cfg.yChannel ) << " "
            << yv.ctpuUnit << "/div\n";
        out << "X,Y\n";
        for ( const auto &p : trajectory() )
            out << p.x << "," << p.y << "\n";
        return;
    }
    // Fallback: legacy format (no metadata, with Sigma column).
    out << "X(CH1),Y(CH2),Sigma\n";
    for ( const auto &p : trajectory() )
        out << p.x << "," << p.y << "," << p.sigma << "\n";
}


void XYRecorder::exportCSVDetailed( const QString &filename ) const {
    QFile file( filename );
    if ( !file.open( QIODevice::WriteOnly | QIODevice::Text ) )
        return;
    QTextStream out( &file );
    out.setCodec( "UTF-8" ); // кириллические имена каналов; локальная кодировка давала мусор в шапке
    writeHeader( out );

    if ( !config.useBinTape ) {
        // Старый путь каскада — прежний формат без изменений.
        out << "Index,X,Y,Sigma\n";
        std::size_t idx = 0;
        for ( const auto &p : trajectory() )
            out << idx++ << "," << p.x << "," << p.y << "," << p.sigma << "\n";
        return;
    }

    // Полный формат ленты (bintape.h). Вместо одинокого бесполезного Sigma —
    // все статистики бина на каждый канал плюс энергетический баланс
    // предфильтра, чтобы данные самописца можно было использовать для
    // расчётов мощности/СКЗ без нарушения закона сохранения энергии.
    //
    // Ключевые небанальные колонки:
    //   trust = path/net — насколько бину можно верить. ~1: бин пройден
    //     напрямую, mean/rms осмысленны. >>1: внутри петли/зубцы/дрожание,
    //     mean НЕ характеризует бин. Пустое значение = inf (замкнутая петля
    //     внутри бина: путь пройден, смещение нулевое) — максимальное
    //     недоверие, и это честный ответ, а не ошибка.
    //   depth — сколько раз бин пережил слияние (0 = ни разу). Растёт, когда
    //     запись длиннее, чем binCount бинов; ничего при этом НЕ стирается,
    //     разрешение лишь равномерно грубеет.
    //   *_resE / *_resPeak — энергия и пиковая амплитуда того, что снял
    //     медианный предфильтр. resPeak важнее доли энергии: узкая срезанная
    //     вершина несёт почти нулевую энергию, но полностью искажает данные.
    //   *_Ein — полная энергия ИСХОДНОГО сигнала, Sum(x^2). Тождество точное:
    //     Ein = Ekept + Eres + 2*cross (фильтр нелинеен, поэтому перекрёстный
    //     член не нулевой и обязан присутствовать, иначе баланс не сойдётся).
    out << "# Columns: per-bin statistics. mean/rms/stddev/min/max/first/last per channel;\n";
    out << "# trust=path/net (~1 trustworthy, >>1 loops inside, empty=inf);\n";
    out << "# depth=merge count; samples=raw samples behind the bin;\n";
    out << "# resE/resPeak/resRms = median pre-filter residual (energy/peak/rms);\n";
    out << "# Ein = Sum(x^2) of the ORIGINAL signal, exact: Ein = Ekept + Eres + 2*cross\n";
    out << "Index,samples,depth,frames,t_first_ms,t_last_ms,path,net,trust,dir,";
    out << "X_mean,X_rms,X_stddev,X_min,X_max,X_first,X_last,X_Ekept,X_Eres,X_cross,X_Ein,X_resPeak,X_resRms,";
    out << "Y_mean,Y_rms,Y_stddev,Y_min,Y_max,Y_first,Y_last,Y_Ekept,Y_Eres,Y_cross,Y_Ein,Y_resPeak,Y_resRms\n";

    const bool masterIsX = ( config.masterAxis == MasterAxis::X );
    std::size_t idx = 0;
    for ( const BinTape::Bin &b : m_tape.bins() ) {
        if ( b.empty() )
            continue;
        const double tr = b.trustRatio();
        out << idx++ << "," << qulonglong( b.x.count ) << "," << b.depth << "," << b.frames << "," << qlonglong( b.tFirstMs )
            << "," << qlonglong( b.tLastMs ) << "," << b.path << "," << b.net() << ",";
        if ( std::isfinite( tr ) )
            out << tr;
        // не-finite -> пустая ячейка, см. комментарий выше
        out << "," << b.direction( masterIsX ) << ",";
        auto emitChannel = [ &out ]( const BinTape::ChannelAccum &c ) {
            out << c.mean() << "," << c.rms() << "," << c.stddev() << "," << c.minV << "," << c.maxV << "," << c.first << ","
                << c.last << "," << c.energyKept() << "," << c.energyResidual() << "," << c.residualCross << ","
                << c.energyIn() << "," << c.residualPeak << "," << c.residualRms();
        };
        emitChannel( b.x );
        out << ",";
        emitChannel( b.y );
        out << "\n";
    }
}
