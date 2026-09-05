// SPDX-License-Identifier: GPL-2.0-or-later
// Last edited: 2026-08-13 07:32:55 UTC
//
// Тесты примитивов замены кольцевого буфера:
//   MedianFilter — подавление одиночных выбросов без завала фронтов
//   BinTape      — фиксированное число бинов, ТОЧНОЕ слияние пар,
//                  отсутствие стирания, метрика доверия path/net
//
// Ключевые инварианты, которые тут проверяются (именно они — причина, по
// которой эта схема заменяет старый каскад):
//   1. Число точек НИКОГДА не превышает заказанное (старая схема давала
//      6689 при targetPoints=2000).
//   2. Слияние ТОЧНОЕ: mean/rms/min/max/count после слияния совпадают с
//      теми, что получились бы при обработке всех сэмплов одним бином.
//   3. Ни один сэмпл не теряется при переполнении (старая схема делала
//      pop_front и молча выбрасывала начало записи).

#include <QtTest>

#include "bintape.h"
#include "medianfilter.h"

#include <cmath>
#include <vector>

class TestBinTape : public QObject {
    Q_OBJECT
  private slots:
    // MedianFilter
    void testMedianIdentity();
    void testMedianRemovesSpike();
    void testMedianKeepsEdge();
    void testMedianLengthPreserved();
    // BinTape
    void testCapacityNeverExceeded();
    void testNoSampleLost();
    void testMergeIsExact();
    void testTrustRatioStraightLine();
    void testTrustRatioLoop();
    void testEnvelopeSurvivesMerging();
    void testDirection();
    // Аудит вмешательства медианы (баланс без неопределённой недостачи)
    void testMedianCountedNoChange();
    void testMedianCountedSpike();
    void testAuditCountersMergeExactly();
    void testEnergyIdentityExact();
    void testEnergyIdentitySurvivesMerging();
    void testBalanceHasNoShortfall();
};


// ---------------------------------------------------------------- MedianFilter

void TestBinTape::testMedianIdentity() {
    std::vector< double > v = { 1, 2, 3, 4, 5 };
    QCOMPARE( MedianFilter::apply( v, 1 ).size(), v.size() );
    auto out = MedianFilter::apply( v, 1 );
    for ( size_t i = 0; i < v.size(); ++i )
        QCOMPARE( out[ i ], v[ i ] );
}


void TestBinTape::testMedianRemovesSpike() {
    // Одиночный выброс посреди постоянного сигнала должен исчезнуть
    // полностью, а не «размазаться» (как это сделало бы среднее).
    std::vector< double > v( 21, 1.0 );
    v[ 10 ] = 100.0;
    auto out = MedianFilter::apply( v, 5 );
    for ( double s : out )
        QCOMPARE( s, 1.0 );
}


void TestBinTape::testMedianKeepsEdge() {
    // Ступенька должна остаться ступенькой: медиана не заваливает фронт.
    std::vector< double > v;
    for ( int i = 0; i < 10; ++i )
        v.push_back( 0.0 );
    for ( int i = 0; i < 10; ++i )
        v.push_back( 1.0 );
    auto out = MedianFilter::apply( v, 5 );
    QCOMPARE( out.front(), 0.0 );
    QCOMPARE( out.back(), 1.0 );
    // Ровно один переход 0->1, без промежуточных значений.
    int transitions = 0;
    for ( size_t i = 1; i < out.size(); ++i ) {
        QVERIFY( out[ i ] == 0.0 || out[ i ] == 1.0 ); // без «размазывания»
        if ( out[ i ] != out[ i - 1 ] )
            ++transitions;
    }
    QCOMPARE( transitions, 1 );
}


void TestBinTape::testMedianLengthPreserved() {
    // В отличие от MovingAverage::apply(), длина НЕ уменьшается.
    for ( unsigned W : { 3u, 5u, 15u, 31u } ) {
        std::vector< double > v( 100, 0.0 );
        QCOMPARE( MedianFilter::apply( v, W ).size(), v.size() );
    }
}


// -------------------------------------------------------------------- BinTape

void TestBinTape::testCapacityNeverExceeded() {
    // Главный дефект старой схемы: заказ 2000 -> по факту 6689.
    BinTape::Tape tape( 500, 1 );
    for ( int i = 0; i < 100000; ++i )
        tape.addSample( double( i ), double( i ) * 2.0 );
    QVERIFY( tape.bins().size() <= tape.capacity() );
}


void TestBinTape::testNoSampleLost() {
    // Старая схема делала pop_front() и теряла начало записи.
    // Здесь слияние сохраняет ВСЕ сэмплы, лишь огрубляя разрешение.
    BinTape::Tape tape( 100, 1 );
    const int N = 12345;
    for ( int i = 0; i < N; ++i )
        tape.addSample( double( i ), 0.0 );
    QCOMPARE( tape.totalSamples(), std::uint64_t( N ) );
}


void TestBinTape::testMergeIsExact() {
    // Слияние обязано давать те же mean/rms/min/max/count, что и обработка
    // всех сэмплов одним бином — иначе «прореживание» было бы искажением.
    std::vector< double > vals;
    for ( int i = 0; i < 1000; ++i )
        vals.push_back( std::sin( i * 0.01 ) * 3.0 + 1.0 );

    BinTape::ChannelAccum whole;
    for ( double v : vals )
        whole.add( v );

    // То же самое, но раздробленное на 8 накопителей и слитое обратно.
    std::vector< BinTape::ChannelAccum > parts( 8 );
    for ( size_t i = 0; i < vals.size(); ++i )
        parts[ i * 8 / vals.size() ].add( vals[ i ] );
    BinTape::ChannelAccum merged = parts[ 0 ];
    for ( size_t i = 1; i < parts.size(); ++i )
        merged.mergeWithNext( parts[ i ] );

    QCOMPARE( merged.count, whole.count );
    QVERIFY( std::abs( merged.mean() - whole.mean() ) < 1e-9 );
    QVERIFY( std::abs( merged.rms() - whole.rms() ) < 1e-9 );
    QVERIFY( std::abs( merged.stddev() - whole.stddev() ) < 1e-9 );
    QCOMPARE( merged.minV, whole.minV );
    QCOMPARE( merged.maxV, whole.maxV );
    QCOMPARE( merged.first, whole.first );
    QCOMPARE( merged.last, whole.last );
}


void TestBinTape::testTrustRatioStraightLine() {
    // Прямой проход: path == net, доверие ~1, среднее осмысленно.
    BinTape::Tape tape( 10, 1000 );
    for ( int i = 0; i < 1000; ++i )
        tape.addSample( double( i ), double( i ) );
    QVERIFY( !tape.bins().empty() );
    QVERIFY( std::abs( tape.bins().front().trustRatio() - 1.0 ) < 1e-6 );
}


void TestBinTape::testTrustRatioLoop() {
    // Замкнутая петля внутри бина: net == 0 при ненулевом path.
    // Честный ответ — бесконечное недоверие, а не тихое «красивое среднее».
    BinTape::Tape tape( 10, 1000 );
    for ( int i = 0; i < 100; ++i )
        tape.addSample( double( i ), 0.0 );
    for ( int i = 100; i >= 0; --i )
        tape.addSample( double( i ), 0.0 );
    const auto &b = tape.bins().front();
    QVERIFY( b.path > 1.0 );
    QVERIFY( b.trustRatio() > 100.0 ); // включая inf
}


void TestBinTape::testEnvelopeSurvivesMerging() {
    // Даже после многократного слияния min/max обязаны показывать реальный
    // охват — петля должна остаться видна как ПОЛОСА, а не схлопнуться.
    BinTape::Tape tape( 8, 1 );
    for ( int i = 0; i < 10000; ++i ) {
        const double v = std::sin( i * 0.001 ) * 5.0;
        tape.addSample( double( i ), v );
    }
    double lo = 1e300, hi = -1e300;
    for ( const auto &b : tape.bins() ) {
        lo = std::min( lo, b.y.minV );
        hi = std::max( hi, b.y.maxV );
    }
    // Истинный размах синуса амплитудой 5 на этом интервале.
    QVERIFY( hi > 4.0 );
    QVERIFY( lo < -4.0 );
}


void TestBinTape::testDirection() {
    BinTape::Tape up( 4, 100000 );
    for ( int i = 0; i < 100; ++i )
        up.addSample( double( i ), 0.0 );
    QCOMPARE( up.bins().front().direction( true ), 1 );

    BinTape::Tape down( 4, 100000 );
    for ( int i = 100; i >= 0; --i )
        down.addSample( double( i ), 0.0 );
    QCOMPARE( down.bins().front().direction( true ), -1 );
}


void TestBinTape::testMedianCountedNoChange() {
    std::vector< double > v;
    for ( int i = 0; i < 100; ++i )
        v.push_back( double( i ) );
    std::uint64_t mod = 0;
    double abs = 0.0;
    auto out = MedianFilter::applyCounted( v, 5, mod, abs );
    for ( size_t i = 2; i + 2 < out.size(); ++i )
        QCOMPARE( out[ i ], v[ i ] );
    QVERIFY( abs >= 0.0 );
}


void TestBinTape::testMedianCountedSpike() {
    std::vector< double > v( 21, 1.0 );
    v[ 10 ] = 100.0;
    std::uint64_t mod = 0;
    double abs = 0.0;
    auto out = MedianFilter::applyCounted( v, 5, mod, abs );
    QCOMPARE( mod, std::uint64_t( 1 ) );
    QVERIFY( std::abs( abs - 99.0 ) < 1e-9 );
    QCOMPARE( out[ 10 ], 1.0 );
}


void TestBinTape::testAuditCountersMergeExactly() {
    // Энергетические члены обязаны суммироваться точно, иначе после слияния
    // баланс "поплывёт" и появится мнимая пропажа энергии.
    BinTape::ChannelAccum a, b;
    a.add( 1.0 );
    a.noteResidual( 1.0, 2.5 );
    a.add( 2.0 );
    b.add( 3.0 );
    b.noteResidual( 3.0, -0.5 );
    b.add( 4.0 );
    b.noteResidual( 4.0, 1.0 );

    const double expSq = a.residualSumSq + b.residualSumSq;
    const double expCross = a.residualCross + b.residualCross;
    const double expPeak = std::max( a.residualPeak, b.residualPeak );
    a.mergeWithNext( b );
    QVERIFY( std::abs( a.residualSumSq - expSq ) < 1e-12 );
    QVERIFY( std::abs( a.residualCross - expCross ) < 1e-12 );
    QCOMPARE( a.residualPeak, expPeak );
}


void TestBinTape::testEnergyIdentityExact() {
    // ГЛАВНЫЙ ТЕСТ: закон сохранения энергии.
    // Sum(x^2) == Sum(y^2) + Sum(r^2) + 2*Sum(y*r), точно, для нелинейного
    // фильтра. Проверяется на реальном медианном фильтре, а не на выдуманных
    // числах: подаём сигнал с выбросами, фильтруем, накапливаем, сверяем.
    std::vector< double > x;
    for ( int i = 0; i < 500; ++i ) {
        double v = std::sin( i * 0.05 ) * 2.0;
        if ( i % 37 == 0 )
            v += 15.0; // выброс
        x.push_back( v );
    }
    auto y = MedianFilter::apply( x, 5 );

    double energyInDirect = 0.0;
    BinTape::ChannelAccum acc;
    for ( size_t i = 0; i < x.size(); ++i ) {
        energyInDirect += x[ i ] * x[ i ];
        acc.add( y[ i ] );
        acc.noteResidual( y[ i ], x[ i ] - y[ i ] );
    }
    // Тождество должно выполняться с машинной точностью.
    QVERIFY( std::abs( acc.energyIn() - energyInDirect ) < 1e-6 );
    // И отвал должен быть заметен (выбросы реально сняты).
    QVERIFY( acc.energyResidual() > 0.0 );
    QVERIFY( acc.residualPeak > 10.0 );
}


void TestBinTape::testEnergyIdentitySurvivesMerging() {
    // То же тождество обязано сохраняться ПОСЛЕ многократных слияний бинов.
    std::vector< double > x;
    for ( int i = 0; i < 20000; ++i ) {
        double v = std::sin( i * 0.001 ) * 3.0;
        if ( i % 53 == 0 )
            v += 8.0;
        x.push_back( v );
    }
    auto y = MedianFilter::apply( x, 7 );

    double energyInDirect = 0.0;
    BinTape::Tape tape( 16, 1 ); // мало бинов -> много слияний
    for ( size_t i = 0; i < x.size(); ++i ) {
        energyInDirect += x[ i ] * x[ i ];
        tape.addSample( y[ i ], 0.0, 0, false, x[ i ] - y[ i ], 0.0 );
    }
    double energyInBins = 0.0;
    for ( const auto &b : tape.bins() )
        energyInBins += b.x.energyIn();
    QVERIFY( tape.bins().size() <= tape.capacity() );
    // Относительная погрешность после всех слияний — машинного порядка.
    QVERIFY( std::abs( energyInBins - energyInDirect ) / energyInDirect < 1e-9 );
}


void TestBinTape::testBalanceHasNoShortfall() {
    BinTape::Tape tape( 16, 1 );
    const int N = 5000;
    double expectResidualEnergy = 0.0;
    for ( int i = 0; i < N; ++i ) {
        const bool spike = ( i % 97 == 0 );
        const double r = spike ? 3.0 : 0.0;
        tape.addSample( double( i ), 0.0, 0, false, r, 0.0 );
        expectResidualEnergy += r * r;
    }
    QCOMPARE( tape.totalSamples(), std::uint64_t( N ) );
    double got = 0.0;
    for ( const auto &b : tape.bins() )
        got += b.x.energyResidual();
    QVERIFY( std::abs( got - expectResidualEnergy ) < 1e-9 );
}


QTEST_GUILESS_MAIN( TestBinTape )
#include "test_bintape.moc"