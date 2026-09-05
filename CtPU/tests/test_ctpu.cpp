// SPDX-License-Identifier: GPL-2.0-or-later
//
// Unit tests for CtPU (TZ §14.1.1, §14.1.2).
// Covers:
//   - apply(): single-sample linear conversion
//   - applyToVector(): in-place vector conversion (incl. identity fast-path)
//   - calculateFromCalibration(): two-point CCtPU, including edge cases:
//       * zero dV (must fail, k/b unchanged)
//       * negative k (inverting sensor)
//       * negative span physical value
//
// Build: see tests/CMakeLists.txt. Run via `ctest --output-on-failure`.

#include <QtTest>

#include "ctpu.h"

#include <vector>

class TestCtPU : public QObject {
    Q_OBJECT
  private slots:
    void testApply();
    void testApplyIdentity();
    void testApplyToVector();
    void testCalculateFromCalibration();
    void testCalculateFromCalibrationZeroDV();
    void testCalculateFromCalibrationNegativeK();
    void testCalculateFromCalibrationNegativeSpan();
};


void TestCtPU::testApply() {
    // apply(0, 100, -2.5) == -2.5 (zero input → offset b)
    QCOMPARE( CtPU::apply( 0.0, 100.0, -2.5 ), -2.5 );
    // apply(1, 100, -2.5) == 97.5 (1V → 100·1 + (-2.5) = 97.5°C)
    QCOMPARE( CtPU::apply( 1.0, 100.0, -2.5 ), 97.5 );
    // apply(-1, 100, -2.5) == -102.5 (symmetry check)
    QCOMPARE( CtPU::apply( -1.0, 100.0, -2.5 ), -102.5 );
}


void TestCtPU::testApplyIdentity() {
    // k=1, b=0 — identity, should return the input unchanged.
    QCOMPARE( CtPU::apply( 42.5, 1.0, 0.0 ), 42.5 );
    QCOMPARE( CtPU::apply( -7.0, 1.0, 0.0 ), -7.0 );
}


void TestCtPU::testApplyToVector() {
    std::vector< double > v = { 0.0, 1.0, -1.0, 2.5 };
    CtPU::applyToVector( v, 10.0, 5.0 );
    QCOMPARE( v.size(), size_t( 4 ) );
    QCOMPARE( v[ 0 ], 5.0 );    // 10·0 + 5
    QCOMPARE( v[ 1 ], 15.0 );   // 10·1 + 5
    QCOMPARE( v[ 2 ], -5.0 );   // 10·(-1) + 5
    QCOMPARE( v[ 3 ], 30.0 );   // 10·2.5 + 5
}


void TestCtPU::testCalculateFromCalibration() {
    // TZ §14.1.1: calculateFromCalibration(0.1, 1.1, 100) → k=100, b=-10
    // dV = 1.1 - 0.1 = 1.0; k = 100/1.0 = 100; b = -100·0.1 = -10
    double k = 0.0, b = 0.0;
    QVERIFY( CtPU::calculateFromCalibration( 0.1, 1.1, 100.0, k, b ) );
    QCOMPARE( k, 100.0 );
    QCOMPARE( b, -10.0 );
}


void TestCtPU::testCalculateFromCalibrationZeroDV() {
    // dV = 0 → must fail, k/b unchanged.
    double k = 42.0, b = -7.0;
    QVERIFY( !CtPU::calculateFromCalibration( 1.0, 1.0, 100.0, k, b ) );
    QCOMPARE( k, 42.0 ); // unchanged
    QCOMPARE( b, -7.0 ); // unchanged
}


void TestCtPU::testCalculateFromCalibrationNegativeK() {
    // TZ §14.1.2: V₀=1.0, V₁=0.0, P₁=100 → k=-100, b=100.
    // dV = 0 - 1 = -1; k = 100/(-1) = -100; b = -(-100)·1 = 100
    double k = 0.0, b = 0.0;
    QVERIFY( CtPU::calculateFromCalibration( 1.0, 0.0, 100.0, k, b ) );
    QCOMPARE( k, -100.0 );
    QCOMPARE( b, 100.0 );
}


void TestCtPU::testCalculateFromCalibrationNegativeSpan() {
    // TZ §14.1.2: V₀=0.0, V₁=1.0, P₁=-50 → k=-50, b=0.
    // dV = 1 - 0 = 1; k = -50/1 = -50; b = -(-50)·0 = 0
    double k = 0.0, b = 0.0;
    QVERIFY( CtPU::calculateFromCalibration( 0.0, 1.0, -50.0, k, b ) );
    QCOMPARE( k, -50.0 );
    QCOMPARE( b, 0.0 );
}


QTEST_GUILESS_MAIN( TestCtPU )
#include "test_ctpu.moc"
