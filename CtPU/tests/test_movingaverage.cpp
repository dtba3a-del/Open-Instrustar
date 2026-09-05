// SPDX-License-Identifier: GPL-2.0-or-later
// Last edited: 2026-08-11 11:31:17 UTC
//
// Unit tests for MovingAverage (Step 1 of the plan confirmed in chat).
// Covers:
//   - apply(): centered N-sample average, identity fast-path (N<=1 or too short)
//   - margin(): over-capture size matches (N-1)/2
//   - cutoffHz(): boxcar -3dB approximation, identity at N<=1
//   - effectiveBitsGained(): 0.5*log2(N)
//   - windowForZoom()/zoomForWindow(): zoom = sqrt(N) law, round-trip
//
// Build: see tests/CMakeLists.txt. Run via `ctest --output-on-failure`.

#include <QtTest>

#include "movingaverage.h"

#include <cmath>
#include <vector>

class TestMovingAverage : public QObject {
    Q_OBJECT
  private slots:
    void testApplyIdentityN1();
    void testApplyIdentityTooShort();
    void testApplyConstantSignal();
    void testApplyKnownWindow();
    void testApplyOutputLength();
    void testMargin();
    void testCutoffHz();
    void testCutoffHzIdentity();
    void testEffectiveBitsGained();
    void testZoomWindowLaw();
    void testZoomWindowRoundTrip();
};


void TestMovingAverage::testApplyIdentityN1() {
    std::vector< double > v = { 1.0, 2.0, 3.0, 4.0 };
    auto out = MovingAverage::apply( v, 1 );
    QCOMPARE( out.size(), v.size() );
    for ( size_t i = 0; i < v.size(); ++i )
        QCOMPARE( out[ i ], v[ i ] );
}


void TestMovingAverage::testApplyIdentityTooShort() {
    // N larger than the input: must return input unchanged, not crash/empty.
    std::vector< double > v = { 1.0, 2.0, 3.0 };
    auto out = MovingAverage::apply( v, 10 );
    QCOMPARE( out.size(), v.size() );
    for ( size_t i = 0; i < v.size(); ++i )
        QCOMPARE( out[ i ], v[ i ] );
}


void TestMovingAverage::testApplyConstantSignal() {
    // Averaging a constant signal must return the same constant (no DC shift).
    std::vector< double > v( 20, 5.0 );
    auto out = MovingAverage::apply( v, 4 );
    for ( double s : out )
        QCOMPARE( s, 5.0 );
}


void TestMovingAverage::testApplyKnownWindow() {
    // v = 1,2,3,4,5,6 ; N=3 -> sliding sums: (1+2+3)/3=2, (2+3+4)/3=3,
    // (3+4+5)/3=4, (4+5+6)/3=5
    std::vector< double > v = { 1.0, 2.0, 3.0, 4.0, 5.0, 6.0 };
    auto out = MovingAverage::apply( v, 3 );
    QCOMPARE( out.size(), size_t( 4 ) );
    QCOMPARE( out[ 0 ], 2.0 );
    QCOMPARE( out[ 1 ], 3.0 );
    QCOMPARE( out[ 2 ], 4.0 );
    QCOMPARE( out[ 3 ], 5.0 );
}


void TestMovingAverage::testApplyOutputLength() {
    // Output length must always be exactly in.size() - (N-1) for N>1 and N<=in.size().
    std::vector< double > v( 100, 0.0 );
    for ( unsigned N : { 2u, 5u, 10u, 50u, 100u } ) {
        auto out = MovingAverage::apply( v, N );
        QCOMPARE( out.size(), v.size() - ( N - 1 ) );
    }
}


void TestMovingAverage::testMargin() {
    QCOMPARE( MovingAverage::margin( 1 ), 0u );
    QCOMPARE( MovingAverage::margin( 2 ), 0u );  // (2-1)/2 = 0
    QCOMPARE( MovingAverage::margin( 3 ), 1u );  // (3-1)/2 = 1
    QCOMPARE( MovingAverage::margin( 9 ), 4u );  // (9-1)/2 = 4
    QCOMPARE( MovingAverage::margin( 100 ), 49u ); // (100-1)/2 = 49 (integer division)
}


void TestMovingAverage::testCutoffHz() {
    // f_c = 0.443 * sampleRate / N
    QCOMPARE( MovingAverage::cutoffHz( 10, 1000.0 ), 44.3 );
    QCOMPARE( MovingAverage::cutoffHz( 100, 1000.0 ), 4.43 );
}


void TestMovingAverage::testCutoffHzIdentity() {
    // N<=1: no averaging, cutoff is reported as the sample rate itself (unrestricted).
    QCOMPARE( MovingAverage::cutoffHz( 1, 1000.0 ), 1000.0 );
    QCOMPARE( MovingAverage::cutoffHz( 0, 1000.0 ), 1000.0 );
}


void TestMovingAverage::testEffectiveBitsGained() {
    // bits = 0.5*log2(N): N=4 -> 1 bit, N=16 -> 2 bits, N=1 -> 0 bits.
    QCOMPARE( MovingAverage::effectiveBitsGained( 1 ), 0.0 );
    QVERIFY( std::abs( MovingAverage::effectiveBitsGained( 4 ) - 1.0 ) < 1e-9 );
    QVERIFY( std::abs( MovingAverage::effectiveBitsGained( 16 ) - 2.0 ) < 1e-9 );
}


void TestMovingAverage::testZoomWindowLaw() {
    // The confirmed law: zoom = sqrt(N)  <=>  N = zoom^2
    QCOMPARE( MovingAverage::windowForZoom( 1.0 ), 1u );
    QCOMPARE( MovingAverage::windowForZoom( 2.0 ), 4u );
    QCOMPARE( MovingAverage::windowForZoom( 10.0 ), 100u );
    QVERIFY( std::abs( MovingAverage::zoomForWindow( 4 ) - 2.0 ) < 1e-9 );
    QVERIFY( std::abs( MovingAverage::zoomForWindow( 100 ) - 10.0 ) < 1e-9 );
}


void TestMovingAverage::testZoomWindowRoundTrip() {
    for ( double zoom : { 1.0, 2.0, 5.0, 10.0, 50.0, 100.0 } ) {
        unsigned N = MovingAverage::windowForZoom( zoom );
        double back = MovingAverage::zoomForWindow( N );
        QVERIFY( std::abs( back - zoom ) < 1e-6 );
    }
}


QTEST_GUILESS_MAIN( TestMovingAverage )
#include "test_movingaverage.moc"
