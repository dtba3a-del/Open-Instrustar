// SPDX-License-Identifier: GPL-2.0-or-later
//
// Integration test: end-to-end pipeline (TZ §14.2).
// Exercises CtPU + MathStack together on synthetic DSOsamples, mimicking what
// HantekDsoControl::stateMachine() does in the live pipeline:
//   1. Synthetic raw CH1/CH2 samples in volts.
//   2. Apply CtPU to physical channels (CH1 k=10, CH2 k=5).
//   3. Run MathStack (M1 = CH1 - CH2 after CtPU).
//   4. Assert the math output matches the expected physical-unit values.
//
// Test case from TZ §14.2.1:
//   CH1 = 1V (k=10) → 10
//   CH2 = 2V (k=5)  → 10
//   M1 = CH1 - CH2 = 0

#include <QtTest>

#include "ctpu.h"
#include "hantekdso/dsosamples.h"
#include "hantekdso/enums.h"
#include "hantekdso/mathstack.h"
#include "scopesettings.h"

#include <memory>
#include <vector>

class TestPipeline : public QObject {
    Q_OBJECT
  private slots:
    void testCtPUPhysicalThenMath();
    void testCtPUMathChannel();
};


static DsoSettingsScope makeScope() {
    DsoSettingsScope s;
    s.voltage.clear();
    for ( int i = 0; i < 2; ++i ) {
        DsoSettingsScopeVoltage v;
        v.name = QStringLiteral( "CH%1" ).arg( i + 1 );
        s.voltage.push_back( v );
    }
    for ( int i = 0; i < DsoSettingsScope::maxMathChannels; ++i ) {
        DsoSettingsScopeVoltage v;
        v.name = QStringLiteral( "M%1" ).arg( i + 1 );
        s.voltage.push_back( v );
    }
    s.mathStack.resize( DsoSettingsScope::maxMathChannels );
    s.xyCurves.resize( DsoSettingsScope::maxXYCurves );
    return s;
}


void TestPipeline::testCtPUPhysicalThenMath() {
    // TZ §14.2.1: CH1=1V (k=10), CH2=2V (k=5) → M1 = CH1 - CH2 = 0
    auto scope = makeScope();
    scope.voltage[ 0 ].ctpuMode = CtPU::Mode::FORMULA;
    scope.voltage[ 0 ].ctpuK = 10.0;
    scope.voltage[ 0 ].ctpuB = 0.0;
    scope.voltage[ 0 ].ctpuUnit = QStringLiteral( "X" );
    scope.voltage[ 1 ].ctpuMode = CtPU::Mode::FORMULA;
    scope.voltage[ 1 ].ctpuK = 5.0;
    scope.voltage[ 1 ].ctpuB = 0.0;
    scope.voltage[ 1 ].ctpuUnit = QStringLiteral( "Y" );

    scope.mathStack[ 0 ].enabled = true;
    scope.mathStack[ 0 ].srcA = 0;
    scope.mathStack[ 0 ].srcB = 1;
    scope.mathStack[ 0 ].op = Dso::MathOp::SUB;
    scope.mathStack[ 0 ].ctpuK = 1.0;
    scope.mathStack[ 0 ].ctpuB = 0.0;

    auto r = std::make_unique< DSOsamples >();
    r->data.resize( 2 + DsoSettingsScope::maxMathChannels );
    r->data[ 0 ] = { 1.0 }; // 1V
    r->data[ 1 ] = { 2.0 }; // 2V
    r->samplerate = 1e6;

    // Step 1: apply CtPU to physical channels (mirrors stateMachine).
    const unsigned nPhys = 2;
    if ( r->physicalUnits.size() < r->data.size() )
        r->physicalUnits.resize( r->data.size(), QStringLiteral( "V" ) );
    for ( unsigned ch = 0; ch < nPhys; ++ch ) {
        CtPU::applyToVector( r->data[ ch ], scope.voltage[ ch ].ctpuK, scope.voltage[ ch ].ctpuB );
        r->physicalUnits[ ch ] = scope.voltage[ ch ].ctpuUnit;
    }
    // After CtPU: CH1 = 10, CH2 = 10
    QCOMPARE( r->data[ 0 ][ 0 ], 10.0 );
    QCOMPARE( r->data[ 1 ][ 0 ], 10.0 );

    // Step 2: run MathStack — M1 = CH1 - CH2 = 0
    MathStack ms( &scope, nPhys );
    ms.calculate( *r );
    QCOMPARE( r->data[ 2 ].size(), size_t( 1 ) );
    QCOMPARE( r->data[ 2 ][ 0 ], 0.0 ); // 10 - 10
}


void TestPipeline::testCtPUMathChannel() {
    // Verify that CtPU applied to a math channel output works:
    // M1 = CH1+CH2 (raw V), CtPU on M1: k=2, b=1
    // CH1=1V, CH2=2V → M1 = 3V → CtPU → 2*3 + 1 = 7
    auto scope = makeScope();
    scope.mathStack[ 0 ].enabled = true;
    scope.mathStack[ 0 ].srcA = 0;
    scope.mathStack[ 0 ].srcB = 1;
    scope.mathStack[ 0 ].op = Dso::MathOp::ADD;
    scope.mathStack[ 0 ].ctpuK = 2.0;
    scope.mathStack[ 0 ].ctpuB = 1.0;
    scope.mathStack[ 0 ].ctpuUnit = QStringLiteral( "W" );

    auto r = std::make_unique< DSOsamples >();
    r->data.resize( 2 + DsoSettingsScope::maxMathChannels );
    r->data[ 0 ] = { 1.0 };
    r->data[ 1 ] = { 2.0 };

    MathStack ms( &scope, 2 );
    ms.calculate( *r );
    // M1 (idx 2) = (1+2)*2 + 1 = 7
    QCOMPARE( r->data[ 2 ][ 0 ], 7.0 );
    QCOMPARE( r->physicalUnits[ 2 ], QStringLiteral( "W" ) );
}


QTEST_GUILESS_MAIN( TestPipeline )
#include "test_pipeline.moc"
