// SPDX-License-Identifier: GPL-2.0-or-later
// Last edited: 2026-08-13 18:08:43 UTC
//
// Тесты канала времени (время как равноправный физический канал).

#include <QtTest>

#include "timechannel.h"

#include <cmath>

class TestTimeChannel : public QObject {
    Q_OBJECT
  private slots:
    void testIntraTimeExact();
    void testIntraTimeSeries();
    void testStampSeparatesMeasuredFromEstimated();
    void testDivisionLadderIs125();
    void testDivisionLadderCoversNsToHours();
    void testSnapDivision();
    void testDivisionLabels();
};


void TestTimeChannel::testIntraTimeExact() {
    // Внутриблочное время — деление индекса на частоту, без накопления ошибки.
    QCOMPARE( TimeChannel::intraTime( 0, 1e6 ), 0.0 );
    QVERIFY( std::abs( TimeChannel::intraTime( 1000000, 1e6 ) - 1.0 ) < 1e-15 );
    QVERIFY( std::abs( TimeChannel::intraTime( 3, 2e6 ) - 1.5e-6 ) < 1e-18 );
    // Нулевая/отрицательная частота не должна давать inf/nan.
    QCOMPARE( TimeChannel::intraTime( 100, 0.0 ), 0.0 );
}


void TestTimeChannel::testIntraTimeSeries() {
    auto t = TimeChannel::intraTimeSeries( 5, 1e3 );
    QCOMPARE( t.size(), size_t( 5 ) );
    for ( size_t i = 0; i < t.size(); ++i )
        QVERIFY( std::abs( t[ i ] - double( i ) * 1e-3 ) < 1e-15 );
}


void TestTimeChannel::testStampSeparatesMeasuredFromEstimated() {
    // Ключевое требование: измеренная и оценочная составляющие раздельны,
    // и класс не притворяется, что epoch точен.
    TimeChannel::Stamp s;
    s.epoch = 1234.5;
    s.intra = 0.25;
    QCOMPARE( s.total(), 1234.75 );
    QVERIFY( TimeChannel::Stamp::epochIsEstimate() );
}


void TestTimeChannel::testDivisionLadderIs125() {
    // Каждая ступень ряда должна быть 1, 2 или 5 на своей декаде — это
    // принятая в измерительной технике логарифмически равномерная сетка.
    // Исключение: секундно-минутно-часовая часть (30 s, 60 s, 300 s, 1800 s,
    // 3600 s), где ряд подчиняется шкале времени, а не десятичной декаде.
    const std::vector< double > timeExceptions = { 30.0, 60.0, 120.0, 300.0, 600.0, 1800.0, 3600.0 };
    for ( double s : TimeChannel::divisionSteps() ) {
        bool isException = false;
        for ( double e : timeExceptions )
            if ( std::abs( s - e ) < 1e-12 )
                isException = true;
        if ( isException )
            continue;
        const double decade = std::pow( 10.0, std::floor( std::log10( s ) ) );
        const double mantissa = s / decade;
        QVERIFY2( std::abs( mantissa - 1.0 ) < 1e-9 || std::abs( mantissa - 2.0 ) < 1e-9 ||
                      std::abs( mantissa - 5.0 ) < 1e-9,
                  qPrintable( QString( "step %1 has mantissa %2" ).arg( s ).arg( mantissa ) ) );
    }
}


void TestTimeChannel::testDivisionLadderCoversNsToHours() {
    const auto &steps = TimeChannel::divisionSteps();
    QVERIFY( !steps.empty() );
    QCOMPARE( steps.front(), 1e-9 );  // 1 нс/дел
    QCOMPARE( steps.back(), 3600.0 ); // 1 ч/дел
    // Ряд обязан строго возрастать, иначе snapDivision() сломается.
    for ( size_t i = 1; i < steps.size(); ++i )
        QVERIFY( steps[ i ] > steps[ i - 1 ] );
}


void TestTimeChannel::testSnapDivision() {
    QCOMPARE( TimeChannel::snapDivision( 1e-9 ), 1e-9 );
    QCOMPARE( TimeChannel::snapDivision( 1.5e-9 ), 2e-9 ); // округление ВВЕРХ
    QCOMPARE( TimeChannel::snapDivision( 3e-6 ), 5e-6 );
    QCOMPARE( TimeChannel::snapDivision( 0.7 ), 1.0 );
    // За пределом ряда — максимум, а не мусор.
    QCOMPARE( TimeChannel::snapDivision( 1e9 ), 3600.0 );
}


void TestTimeChannel::testDivisionLabels() {
    QCOMPARE( QString::fromStdString( TimeChannel::divisionLabel( 1e-9 ) ), QString( "1 ns" ) );
    QCOMPARE( QString::fromStdString( TimeChannel::divisionLabel( 5e-6 ) ), QString( "5 us" ) );
    QCOMPARE( QString::fromStdString( TimeChannel::divisionLabel( 2e-3 ) ), QString( "2 ms" ) );
    QCOMPARE( QString::fromStdString( TimeChannel::divisionLabel( 5.0 ) ), QString( "5 s" ) );
    QCOMPARE( QString::fromStdString( TimeChannel::divisionLabel( 60.0 ) ), QString( "1 min" ) );
    QCOMPARE( QString::fromStdString( TimeChannel::divisionLabel( 1800.0 ) ), QString( "30 min" ) );
    QCOMPARE( QString::fromStdString( TimeChannel::divisionLabel( 3600.0 ) ), QString( "1 h" ) );
}


QTEST_GUILESS_MAIN( TestTimeChannel )
#include "test_timechannel.moc"
