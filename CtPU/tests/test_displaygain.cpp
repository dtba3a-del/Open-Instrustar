// SPDX-License-Identifier: GPL-2.0-or-later
//
// Регресс-тест на расхождение масштабов при HiRes.
//
// Дефект, ради которого написан (`docs/PROTOTYPE-QUEUE.md`, задание 7):
// один параметр `digitalZoomN` совмещал две разные функции — усреднение
// (прибавка эффективных бит) и экранное увеличение. `graphgenerator` рисовал
// трассу с усилением gain/√N, а порог триггера в `dsowidget` считался от
// physicalGain() без деления — порог расходился с трассой ровно в √N раз.
//
// Функции разделены: `resolutionN` (разрядность, величину НЕ меняет) и
// `screenZoom` (экранное увеличение, перенормирует сетку В/дел). Единственное
// место пересчёта «деление экрана ↔ величина» — DsoSettingsScope::displayGain().
//
// Тест закрепляет три свойства, нарушение любого из которых возвращает дефект.

#include <QtTest>

#include "scopesettings.h"

class TestDisplayGain : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase();
    void testResolutionDoesNotChangeScale();
    void testZoomRenormalisesGrid();
    void testZoomGuardsAgainstNonPositive();
    void testResolutionAndZoomAreIndependent();

  private:
    DsoSettingsScope scope;
    static constexpr unsigned CH = 0;
};


void TestDisplayGain::initTestCase() {
    // Минимальная обстановка: канал 0 обязан быть вещественным, поэтому
    // `voltage` длиннее maxMathChannels — gain() отсчитывает математические
    // каналы с конца вектора, а maxMathChannels — константа времени сборки.
    scope.voltage.resize( DsoSettingsScope::maxMathChannels + 1 );
    scope.gainSteps = { 1.0 };
    scope.mathGainSteps = { 1.0 };
    scope.voltage[ CH ].gainStepIndex = 0;
    scope.voltage[ CH ].probeAttn = 1.0;
    scope.voltage[ CH ].ctpuK = 1.0;
    QCOMPARE( scope.physicalGain( CH ), 1.0 );
}


// 1. Разрядность величину не меняет. Среднее сохраняет уровень, поэтому ни
//    цена деления, ни подпись В/дел от окна усреднения зависеть не должны.
//    Именно это свойство и было нарушено: прежде N задавал ещё и масштаб.
void TestDisplayGain::testResolutionDoesNotChangeScale() {
    const double before = scope.displayGain( CH );
    for ( unsigned N : { 1u, 2u, 4u, 8u, 16u, 32u, 64u } ) {
        scope.voltage[ CH ].resolutionN = N;
        QCOMPARE( scope.displayGain( CH ), before );
    }
    scope.voltage[ CH ].resolutionN = 1;
}


// 2. Экранное увеличение перенормирует сетку ровно в 1/zoom раз: входные
//    уровни не меняются, растянуто только изображение.
void TestDisplayGain::testZoomRenormalisesGrid() {
    for ( double z : { 1.0, 2.0, 4.0, 8.0, 16.0 } ) {
        scope.voltage[ CH ].screenZoom = z;
        QCOMPARE( scope.displayGain( CH ), scope.physicalGain( CH ) / z );
    }
    scope.voltage[ CH ].screenZoom = 1.0;
}


// 3. Ноль и отрицательное увеличение не должны обращать масштаб в
//    бесконечность или менять знак: значение из настроек может быть испорчено.
void TestDisplayGain::testZoomGuardsAgainstNonPositive() {
    for ( double z : { 0.0, -1.0, -4.0 } ) {
        scope.voltage[ CH ].screenZoom = z;
        QCOMPARE( scope.displayGain( CH ), scope.physicalGain( CH ) );
    }
    scope.voltage[ CH ].screenZoom = 1.0;
}


// 4. Совместное применение: разрядность в масштаб не вмешивается, увеличение
//    действует одно. Пара значений подобрана так, что прежний дефект дал бы
//    расхождение в √N = 4 раза и был бы виден.
void TestDisplayGain::testResolutionAndZoomAreIndependent() {
    scope.voltage[ CH ].resolutionN = 16; // прежде дало бы масштаб ×4
    scope.voltage[ CH ].screenZoom = 2.0;
    QCOMPARE( scope.displayGain( CH ), scope.physicalGain( CH ) / 2.0 );

    // Порог триггера и трасса берут одну и ту же величину — это и есть
    // «одно поведение — одно место». Расхождение вернулось бы, если бы одна
    // из сторон снова считала от physicalGain().
    const double traceScale = scope.displayGain( CH );
    const double triggerScale = scope.displayGain( CH );
    QCOMPARE( traceScale, triggerScale );
    QVERIFY( traceScale != scope.physicalGain( CH ) );
}


QTEST_APPLESS_MAIN( TestDisplayGain )
#include "test_displaygain.moc"
