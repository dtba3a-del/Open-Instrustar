// SPDX-License-Identifier: GPL-3.0-or-later
//
// Автоматический уровень триггера по гистограмме (задание 9 очереди).
// Проверяется существо приёма, а не совпадение с реализацией:
//   - у двухсостоянийного сигнала порог лежит МЕЖДУ состояниями, а не в моде;
//   - у одномодового берётся медиана;
//   - у постоянного сигнала уровень не назначается вовсе.

#include <QtTest>

#include "autotrigger.h"

#include <cmath>
#include <vector>

class TestAutoTrigger : public QObject {
    Q_OBJECT
  private slots:
    void testSquareWaveLevelIsBetweenStates();
    void testAsymmetricDutyStillBetweenStates();
    void testSineFallsBackToMedian();
    void testConstantSignalGivesNoLevel();
    void testTooFewSamplesGiveNoLevel();
};


// Меандр 0 / 1: обе моды населены, порог обязан оказаться между ними.
// Именно здесь «где чаще» как прямой ответ дало бы 0 или 1 — уровень, на
// котором сигнал стоит, а не пересекает.
void TestAutoTrigger::testSquareWaveLevelIsBetweenStates() {
    std::vector< double > s;
    for ( int i = 0; i < 1000; ++i )
        s.push_back( ( i / 50 ) % 2 ? 1.0 : 0.0 );
    const auto lvl = AutoTrigger::level( s );
    QVERIFY( lvl.valid );
    QVERIFY( lvl.bimodal );
    QVERIFY( lvl.value > 0.2 );
    QVERIFY( lvl.value < 0.8 );
}


// Скважность 1:9 — низкое состояние населено вдевятеро гуще. Порог всё равно
// между состояниями: иначе триггер срабатывал бы на шуме нижней полки.
void TestAutoTrigger::testAsymmetricDutyStillBetweenStates() {
    std::vector< double > s;
    for ( int i = 0; i < 1000; ++i )
        s.push_back( ( i % 100 ) < 10 ? 2.0 : -2.0 );
    const auto lvl = AutoTrigger::level( s );
    QVERIFY( lvl.valid );
    QVERIFY( lvl.bimodal );
    QVERIFY( lvl.value > -1.5 );
    QVERIFY( lvl.value < 1.5 );
}


// Синус: двух состояний нет (моды на краях — это точки разворота), поэтому
// берётся медиана, то есть уровень, который сигнал пересекает по построению.
void TestAutoTrigger::testSineFallsBackToMedian() {
    std::vector< double > s;
    for ( int i = 0; i < 2000; ++i )
        s.push_back( 3.0 * std::sin( 2.0 * M_PI * double( i ) / 200.0 ) + 1.0 );
    const auto lvl = AutoTrigger::level( s );
    QVERIFY( lvl.valid );
    QVERIFY( std::fabs( lvl.value - 1.0 ) < 0.5 ); // медиана около смещения
}


// Постоянный сигнал: пересечения не существует. Назначить уровень значило бы
// пообещать событие, которого не будет.
void TestAutoTrigger::testConstantSignalGivesNoLevel() {
    const std::vector< double > s( 500, 1.234 );
    QVERIFY( !AutoTrigger::level( s ).valid );
}


void TestAutoTrigger::testTooFewSamplesGiveNoLevel() {
    QVERIFY( !AutoTrigger::level( {} ).valid );
    QVERIFY( !AutoTrigger::level( { 1.0 } ).valid );
}


QTEST_APPLESS_MAIN( TestAutoTrigger )
#include "test_autotrigger.moc"
