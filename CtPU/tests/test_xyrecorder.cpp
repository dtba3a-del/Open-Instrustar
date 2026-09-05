// SPDX-License-Identifier: GPL-2.0-or-later
//
// Регресс-тест на дефект «пропали точки мультикривых XY».
//
// Симптом был: в режиме XY REC не выводилась НИ ОДНА кривая, счётчик
// «XY pts» стоял на нуле, экспорт молча пропускал все кривые — при том, что
// данные исправно поступали и BinTape их принимал.
//
// Причина: при useBinTape=true (значение по умолчанию) источник истины —
// m_tape, а `traj` — лишь ленивый кэш проекции, наполняемый ТОЛЬКО внутри
// trajectory(). Методы empty()/size() читали `traj` напрямую, а все три
// вызывающих (GlScope::updateXY, DsoWidget::showNew, экспорт в MainWindow)
// проверяют empty()/size() ПЕРЕД обращением к trajectory(). Замкнутый круг:
// кэш нечем наполнить, не отрисовав, и нечего отрисовывать, не наполнив кэш.
//
// Инвариант, который тут закрепляется: **empty()/size() обязаны совпадать с
// trajectory() при ЛЮБОМ порядке вызовов** — в том числе когда empty()
// спрашивают первым, до единого обращения к trajectory().

#include <QtTest>

#include "post/ppresult.h"
#include "scopesettings.h"
#include "xyrecorder.h"

#include <cmath>

class TestXYRecorder : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase();
    void testEmptyBeforeAnyFrame();
    void testSizeMatchesTrajectoryBinTape();
    void testEmptyIsFalseWithoutTouchingTrajectoryFirst();
    void testCurveBindingSelectsChannels();
    void testCascadeModeStillWorks();
    void testTimeAxisMonotonicAcrossFrames();

  private:
    DsoSettingsScope scope;
    /// Кадр с двумя каналами: X — пила, Y — синус от неё.
    std::unique_ptr< PPresult > makeFrame( std::size_t n, double xScale = 1.0, double yScale = 1.0 );
    XYRecorder::Config binTapeConfig() const;
};


void TestXYRecorder::initTestCase() {
    // Минимально достаточная настройка: 2 реальных канала + maxMathChannels,
    // как это делает DsoSettings при старте.
    scope.voltage.resize( 2 + DsoSettingsScope::maxMathChannels );
    scope.spectrum.resize( scope.voltage.size() );
    scope.horizontal.samplerate = 1e6;
    scope.xyCurves.resize( DsoSettingsScope::maxXYCurves );
}


std::unique_ptr< PPresult > TestXYRecorder::makeFrame( std::size_t n, double xScale, double yScale ) {
    auto frame = std::make_unique< PPresult >( unsigned( scope.voltage.size() ) );
    for ( ChannelID ch = 0; ch < ChannelID( scope.voltage.size() ); ++ch ) {
        DataChannel *d = frame->modifiableData( ch );
        d->voltage.samples.resize( n );
        d->voltage.interval = 1.0 / scope.horizontal.samplerate;
    }
    DataChannel *x = frame->modifiableData( 0 );
    DataChannel *y = frame->modifiableData( 1 );
    for ( std::size_t i = 0; i < n; ++i ) {
        const double t = double( i ) / double( n );
        x->voltage.samples[ i ] = xScale * t;                       // пила
        y->voltage.samples[ i ] = yScale * std::sin( 2 * M_PI * t ); // отклик
    }
    // Третий канал — чтобы отличить привязку кривой от жёсткого CH1/CH2.
    DataChannel *m = frame->modifiableData( 2 );
    for ( std::size_t i = 0; i < n; ++i )
        m->voltage.samples[ i ] = 42.0;
    return frame;
}


XYRecorder::Config TestXYRecorder::binTapeConfig() const {
    XYRecorder::Config cfg;
    cfg.useBinTape = true; // режим по умолчанию — именно он и был сломан
    cfg.binCount = 64;
    cfg.medianWindow = 0;
    cfg.sheetMode = XYRecorder::SheetMode::FINITE;
    cfg.tapeFilePath.clear(); // без записи на диск
    return cfg;
}


/// До первого кадра рекордер обязан быть пуст — и это должно быть видно
/// через empty(), а не только через trajectory().
void TestXYRecorder::testEmptyBeforeAnyFrame() {
    XYRecorder rec;
    rec.configure( &scope, nullptr, binTapeConfig() );
    QVERIFY( rec.empty() );
    QCOMPARE( rec.size(), std::size_t( 0 ) );
    QCOMPARE( rec.trajectory().size(), std::size_t( 0 ) );
}


/// size() обязан отражать то же число точек, что вернёт trajectory().
void TestXYRecorder::testSizeMatchesTrajectoryBinTape() {
    XYRecorder rec;
    rec.configure( &scope, nullptr, binTapeConfig() );

    auto frame = makeFrame( 1000 );
    rec.addFrame( frame.get() );

    const std::size_t viaTrajectory = rec.trajectory().size();
    QVERIFY2( viaTrajectory > 0, "BinTape принял кадр, но проекция пуста" );
    QCOMPARE( rec.size(), viaTrajectory );
    QVERIFY( !rec.empty() );

    // Второй кадр помечает проекцию грязной — согласованность обязана
    // сохраниться и после пересборки кэша.
    rec.addFrame( frame.get() );
    QCOMPARE( rec.size(), rec.trajectory().size() );
    QVERIFY( !rec.empty() );
}


/// Тест самого дефекта: empty() спрашивается ПЕРВЫМ, как это делают
/// GlScope::updateXY() и MainWindow (экспорт). До исправления здесь
/// возвращалось true навсегда, и до trajectory() дело не доходило никогда.
void TestXYRecorder::testEmptyIsFalseWithoutTouchingTrajectoryFirst() {
    XYRecorder rec;
    rec.configure( &scope, nullptr, binTapeConfig() );

    auto frame = makeFrame( 1000 );
    rec.addFrame( frame.get() );

    QVERIFY2( !rec.empty(), "empty() читает кэш проекции вместо ленты — кривая никогда не отрисуется" );
    QVERIFY2( rec.size() > 0, "size() читает кэш проекции вместо ленты — счётчик XY pts застрянет на нуле" );
}


/// Привязка кривой обязана определять, какие каналы пишутся в ленту:
/// кривая (CH3, CH2) не должна давать ту же траекторию, что (CH1, CH2).
void TestXYRecorder::testCurveBindingSelectsChannels() {
    auto frame = makeFrame( 1000 );

    XYCurveConfig c0;
    c0.enabled = true;
    c0.xChannel = 0;
    c0.yChannel = 1;

    XYCurveConfig c1;
    c1.enabled = true;
    c1.xChannel = 2; // канал с константой 42.0
    c1.yChannel = 1;

    XYRecorder recA;
    recA.setCurveConfig( c0 );
    recA.configure( &scope, nullptr, binTapeConfig() );
    recA.addFrame( frame.get() );

    XYRecorder recB;
    recB.setCurveConfig( c1 );
    recB.configure( &scope, nullptr, binTapeConfig() );
    recB.addFrame( frame.get() );

    QVERIFY( !recA.empty() );
    QVERIFY( !recB.empty() );
    // X канала 2 — константа 42, у канала 0 — пила 0..1.
    QVERIFY2( std::fabs( recB.trajectory().front().x - 42.0 ) < 1e-9,
              "рекордер не читает назначенный кривой X-канал" );
    QVERIFY( std::fabs( recA.trajectory().front().x - 42.0 ) > 1.0 );
}


/// Старый каскадный путь (useBinTape=false) пишет в traj напрямую — там
/// согласованность была и раньше, но ломать её исправлением нельзя.
void TestXYRecorder::testCascadeModeStillWorks() {
    XYRecorder::Config cfg = binTapeConfig();
    cfg.useBinTape = false;
    cfg.cascadeBase = 8;
    cfg.targetPoints = 100;

    XYRecorder rec;
    rec.configure( &scope, nullptr, cfg );
    auto frame = makeFrame( 4096 );
    rec.addFrame( frame.get() );

    QCOMPARE( rec.size(), rec.trajectory().size() );
    QCOMPARE( rec.empty(), rec.trajectory().empty() );
}


// GUILESS: Qt5::Gui линкуется только ради QVector3D в PPresult; сам тест
// экрана не требует, а 
/// У3 — TimeChannel: ось X = время. Значения оси обязаны быть монотонными
/// СКВОЗЬ кадры (intra + epoch, честная склейка), а Y — попадать в ленту как
/// обычно. Кадры с captureTimestampMs=0 (синтетика) склеиваются встык по
/// накопленной длительности — без провалов в ноль на границе кадра.
void TestXYRecorder::testTimeAxisMonotonicAcrossFrames() {
    XYRecorder rec;
    XYCurveConfig cfg{};
    cfg.enabled = true;
    cfg.xChannel = DsoSettingsScope::timeChannelIndex; // время как ось X
    cfg.yChannel = 1;
    rec.setCurveConfig( cfg );
    rec.configure( &scope, nullptr, binTapeConfig() );

    auto f1 = makeFrame( 1000 );
    auto f2 = makeFrame( 1000 );
    rec.addFrame( f1.get() );
    rec.addFrame( f2.get() );

    const auto &traj = rec.trajectory();
    QVERIFY( !traj.empty() );
    // Монотонность времени по всей ленте (бины упорядочены по позиции).
    for ( std::size_t i = 1; i < traj.size(); ++i )
        QVERIFY2( traj[ i ].x >= traj[ i - 1 ].x, "time axis must be monotonic across frames" );
    // Полный охват: 2 кадра по 1000 отсчётов на 1 MSps = 2 мс записи.
    const double expectedSpan = 2000.0 / scope.horizontal.samplerate;
    QVERIFY( traj.back().x > 0.9 * expectedSpan * ( double( traj.size() ) / double( traj.size() + 1 ) ) );
    QVERIFY( traj.back().x <= expectedSpan );
}

// Явный GUILESS: QTEST_MAIN при QT_GUI_LIB поднимал бы
// QGuiApplication и падал в headless-окружении (CI, MSYS2 без дисплея).
QTEST_GUILESS_MAIN( TestXYRecorder )
#include "test_xyrecorder.moc"
