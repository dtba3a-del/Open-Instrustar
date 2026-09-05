// SPDX-License-Identifier: GPL-2.0-or-later
//
// Unit tests for MathStack (TZ §14.1.3).
// Covers:
//   - ADD/SUB/MUL/DIV operations on simple pairs
//   - Cascade: M2 depends on M1 (M1 = CH1+CH2, M2 = CH1*M1)
//   - Invalid operand (srcA/srcB >= mathIndex) — must skip without crash
//   - Inversion flag
//   - DIV by zero — must emit 0 instead of NaN/Inf
//   - Disabled math channel — must leave its output empty
//
// Build: see tests/CMakeLists.txt. Run via `ctest --output-on-failure`.

#include <QtTest>

#include "ctpu.h"
#include "hantekdso/dsosamples.h"
#include "hantekdso/enums.h"
#include "hantekdso/mathstack.h"
#include "scopesettings.h"

#include <cmath>
#include <memory>
#include <vector>

class TestMathStack : public QObject {
    Q_OBJECT
  private slots:
    void testAdd();
    void testSub();
    void testMul();
    void testDiv();
    void testDivByZero();
    void testCascade();
    void testInvalidOperand();
    void testInvert();
    void testDisabled();
};


/// Helper: build a minimal DsoSettingsScope with 2 real + 4 math channels.
static DsoSettingsScope makeScope() {
    DsoSettingsScope s;
    s.voltage.clear();
    // 2 real channels (CH1, CH2)
    for ( int i = 0; i < 2; ++i ) {
        DsoSettingsScopeVoltage v;
        v.name = QStringLiteral( "CH%1" ).arg( i + 1 );
        s.voltage.push_back( v );
    }
    // 4 math channels (M1..M4)
    for ( int i = 0; i < DsoSettingsScope::maxMathChannels; ++i ) {
        DsoSettingsScopeVoltage v;
        v.name = QStringLiteral( "M%1" ).arg( i + 1 );
        s.voltage.push_back( v );
    }
    s.mathStack.resize( DsoSettingsScope::maxMathChannels );
    s.xyCurves.resize( DsoSettingsScope::maxXYCurves );
    return s;
}


/// Helper: build a DSOsamples with CH1 and CH2 populated.
/// DSOsamples is non-copyable/non-movable (contains QReadWriteLock), so we
/// return by pointer.
static std::unique_ptr< DSOsamples > makeSamples( const std::vector< double > &ch1, const std::vector< double > &ch2 ) {
    auto r = std::make_unique< DSOsamples >();
    r->data.resize( 2 + DsoSettingsScope::maxMathChannels );
    r->data[ 0 ] = ch1;
    r->data[ 1 ] = ch2;
    r->samplerate = 1e6;
    return r;
}


void TestMathStack::testAdd() {
    // TZ §14.1.3: ADD {1,2} + {3,4} = {4,6}
    auto scope = makeScope();
    scope.mathStack[ 0 ].enabled = true;
    scope.mathStack[ 0 ].srcA = 0;
    scope.mathStack[ 0 ].srcB = 1;
    scope.mathStack[ 0 ].op = Dso::MathOp::ADD;
    MathStack ms( &scope, 2 );
    auto r = makeSamples( { 1.0, 2.0 }, { 3.0, 4.0 } );
    ms.calculate( *r );
    QCOMPARE( r->data[ 2 ].size(), size_t( 2 ) );
    QCOMPARE( r->data[ 2 ][ 0 ], 4.0 );
    QCOMPARE( r->data[ 2 ][ 1 ], 6.0 );
}


void TestMathStack::testSub() {
    auto scope = makeScope();
    scope.mathStack[ 0 ].enabled = true;
    scope.mathStack[ 0 ].srcA = 0;
    scope.mathStack[ 0 ].srcB = 1;
    scope.mathStack[ 0 ].op = Dso::MathOp::SUB;
    MathStack ms( &scope, 2 );
    auto r = makeSamples( { 1.0, 2.0 }, { 3.0, 4.0 } );
    ms.calculate( *r );
    QCOMPARE( r->data[ 2 ][ 0 ], -2.0 ); // 1 - 3
    QCOMPARE( r->data[ 2 ][ 1 ], -2.0 ); // 2 - 4
}


void TestMathStack::testMul() {
    auto scope = makeScope();
    scope.mathStack[ 0 ].enabled = true;
    scope.mathStack[ 0 ].srcA = 0;
    scope.mathStack[ 0 ].srcB = 1;
    scope.mathStack[ 0 ].op = Dso::MathOp::MUL;
    MathStack ms( &scope, 2 );
    auto r = makeSamples( { 1.0, 2.0 }, { 3.0, 4.0 } );
    ms.calculate( *r );
    QCOMPARE( r->data[ 2 ][ 0 ], 3.0 ); // 1 * 3
    QCOMPARE( r->data[ 2 ][ 1 ], 8.0 ); // 2 * 4
}


void TestMathStack::testDiv() {
    auto scope = makeScope();
    scope.mathStack[ 0 ].enabled = true;
    scope.mathStack[ 0 ].srcA = 0;
    scope.mathStack[ 0 ].srcB = 1;
    scope.mathStack[ 0 ].op = Dso::MathOp::DIV;
    MathStack ms( &scope, 2 );
    auto r = makeSamples( { 6.0, 8.0 }, { 3.0, 4.0 } );
    ms.calculate( *r );
    QCOMPARE( r->data[ 2 ][ 0 ], 2.0 ); // 6 / 3
    QCOMPARE( r->data[ 2 ][ 1 ], 2.0 ); // 8 / 4
}


void TestMathStack::testDivByZero() {
    auto scope = makeScope();
    scope.mathStack[ 0 ].enabled = true;
    scope.mathStack[ 0 ].srcA = 0;
    scope.mathStack[ 0 ].srcB = 1;
    scope.mathStack[ 0 ].op = Dso::MathOp::DIV;
    MathStack ms( &scope, 2 );
    auto r = makeSamples( { 6.0 }, { 0.0 } );
    ms.calculate( *r );
    QCOMPARE( r->data[ 2 ][ 0 ], 0.0 ); // division by zero → 0.0 (TZ §5.5.1)
    QVERIFY( !std::isinf( r->data[ 2 ][ 0 ] ) );
    QVERIFY( !std::isnan( r->data[ 2 ][ 0 ] ) );
}


void TestMathStack::testCascade() {
    // TZ §14.1.3: M1 = CH1+CH2, M2 = CH1*M1
    // CH1=1, CH2=2 → M1 = 3; M2 = 1*3 = 3
    // CH1=2, CH2=3 → M1 = 5; M2 = 2*5 = 10
    auto scope = makeScope();
    scope.mathStack[ 0 ].enabled = true;
    scope.mathStack[ 0 ].srcA = 0;
    scope.mathStack[ 0 ].srcB = 1;
    scope.mathStack[ 0 ].op = Dso::MathOp::ADD;
    // M2 uses CH1 (idx 0) and M1 (idx 2) — stack dependency (srcB < mathIndex=3).
    scope.mathStack[ 1 ].enabled = true;
    scope.mathStack[ 1 ].srcA = 0;
    scope.mathStack[ 1 ].srcB = 2;
    scope.mathStack[ 1 ].op = Dso::MathOp::MUL;
    MathStack ms( &scope, 2 );
    auto r = makeSamples( { 1.0, 2.0 }, { 2.0, 3.0 } );
    ms.calculate( *r );
    // M1 (idx 2): 1+2=3, 2+3=5
    QCOMPARE( r->data[ 2 ][ 0 ], 3.0 );
    QCOMPARE( r->data[ 2 ][ 1 ], 5.0 );
    // M2 (idx 3): 1*3=3, 2*5=10
    QCOMPARE( r->data[ 3 ][ 0 ], 3.0 );
    QCOMPARE( r->data[ 3 ][ 1 ], 10.0 );
}


void TestMathStack::testInvalidOperand() {
    // srcA >= mathIndex → must skip without crash, output empty.
    auto scope = makeScope();
    scope.mathStack[ 0 ].enabled = true;
    scope.mathStack[ 0 ].srcA = 5; // invalid: >= mathIndex (2)
    scope.mathStack[ 0 ].srcB = 0;
    scope.mathStack[ 0 ].op = Dso::MathOp::ADD;
    MathStack ms( &scope, 2 );
    auto r = makeSamples( { 1.0 }, { 2.0 } );
    ms.calculate( *r );
    QVERIFY( r->data[ 2 ].empty() ); // skipped — output left empty
}


void TestMathStack::testInvert() {
    auto scope = makeScope();
    scope.mathStack[ 0 ].enabled = true;
    scope.mathStack[ 0 ].srcA = 0;
    scope.mathStack[ 0 ].srcB = 1;
    scope.mathStack[ 0 ].op = Dso::MathOp::ADD;
    scope.mathStack[ 0 ].invert = true;
    MathStack ms( &scope, 2 );
    auto r = makeSamples( { 1.0, 2.0 }, { 3.0, 4.0 } );
    ms.calculate( *r );
    QCOMPARE( r->data[ 2 ][ 0 ], -4.0 ); // -(1+3)
    QCOMPARE( r->data[ 2 ][ 1 ], -6.0 ); // -(2+4)
}


void TestMathStack::testDisabled() {
    auto scope = makeScope();
    scope.mathStack[ 0 ].enabled = false; // disabled
    scope.mathStack[ 0 ].srcA = 0;
    scope.mathStack[ 0 ].srcB = 1;
    scope.mathStack[ 0 ].op = Dso::MathOp::ADD;
    MathStack ms( &scope, 2 );
    auto r = makeSamples( { 1.0 }, { 2.0 } );
    ms.calculate( *r );
    QVERIFY( r->data[ 2 ].empty() ); // disabled → no output
}


QTEST_GUILESS_MAIN( TestMathStack )
#include "test_mathstack.moc"
