// SPDX-License-Identifier: GPL-3.0-or-later
// Last edited: 2026-09-03 UTC
//
// Путь A: пересчёт единиц. Проверяется без прибора и без DLL.
//
// Главное, ради чего написан этот файл: ×1000 обязан жить на пути
// НАСТРОЕК и не появиться на пути ДАННЫХ. Ошибка здесь не даёт ни
// отказа сборки, ни сообщения — она даёт график, у которого верная
// форма и неверная величина.

#include "ivdsosession.h"
#include "ivdsounits.h"

#include <QtTest>

using namespace IVdso;

class TestIVdsoUnits : public QObject {
    Q_OBJECT
  private slots:

    // --- предел канала: «В/дел» представления → милливольты вендора ---

    void rangeMv_fromVoltsPerDiv() {
        // 1 В/дел на восьми делениях — это ±4 В, то есть ±4000 мВ.
        QCOMPARE( rangeMvFromVoltsPerDiv( 1.0, 8 ), ( RangeMv{ -4000, 4000 } ) );
        QCOMPARE( rangeMvFromVoltsPerDiv( 0.5, 8 ), ( RangeMv{ -2000, 2000 } ) );
        QCOMPARE( rangeMvFromVoltsPerDiv( 0.02, 8 ), ( RangeMv{ -80, 80 } ) );
    }

    void rangeMv_refusesNonsense() {
        QCOMPARE( rangeMvFromVoltsPerDiv( 0.0, 8 ), ( RangeMv{ 0, 0 } ) );
        QCOMPARE( rangeMvFromVoltsPerDiv( -1.0, 8 ), ( RangeMv{ 0, 0 } ) );
        QCOMPARE( rangeMvFromVoltsPerDiv( 1.0, 0 ), ( RangeMv{ 0, 0 } ) );
    }

    void rangeMv_roundTrip() {
        for ( double vdiv : { 0.02, 0.1, 0.5, 1.0, 5.0 } ) {
            const RangeMv r = rangeMvFromVoltsPerDiv( vdiv, 8 );
            QVERIFY( qFuzzyCompare( voltsPerDivFromRangeMv( r, 8 ), vdiv ) );
        }
    }

    // --- память: килобайты вендора против отсчётов ---

    void kb_toPoints() {
        // «Байт» у вендора равен одному отсчёту на канал: оба его демо
        // выделяют GetMemoryLength()*1024 элементов double.
        QCOMPARE( pointsFromKb( 1 ), 1024u );
        QCOMPARE( pointsFromKb( 1024 ), 1048576u );
    }

    void points_toKb_roundsUp() {
        QCOMPARE( kbFromPoints( 4096 ), 4u );
        QCOMPARE( kbFromPoints( 1 ), 1u );
        // 2044 — не выдумка: столько отдаёт 205A/B при пределах ≥2000 мВ
        // на запрос 4096. Просить под них 1 КБ нельзя.
        QCOMPARE( kbFromPoints( 2044 ), 2u );
    }

    // --- ряд скоростей прибора ---

    void samplerate_nearestFromDeviceSeries() {
        const std::vector< unsigned int > isds205b = { 1000000, 4000000, 8000000, 16000000,
                                                       48000000 };
        QCOMPARE( nearestSupportedSamplerate( 4000000, isds205b ), 4000000u );
        QCOMPARE( nearestSupportedSamplerate( 3000000, isds205b ), 4000000u );
        // Запрос ровно посередине кратного ряда: ничья разрешается вверх.
        QCOMPARE( nearestSupportedSamplerate( 2000000, isds205b ), 4000000u );
        // Выше ряда — верхняя ступень, ниже ряда — нижняя.
        QCOMPARE( nearestSupportedSamplerate( 100000000, isds205b ), 48000000u );
        QCOMPARE( nearestSupportedSamplerate( 1000, isds205b ), 1000000u );
    }

    void samplerate_emptySeries() {
        QCOMPARE( nearestSupportedSamplerate( 1000000, {} ), 0u );
    }

    // --- паспорт прибора ---

    void deviceId_isOneNumberInTwoWords() {
        // GetOnlyId0/1 — половины одного 64-битного номера, а не два
        // независимых числа. Измеренная пара ISDS205B: 3136672271:122.
        QCOMPARE( deviceId( 3136672271u, 122u ), uint64_t( 122 ) << 32 | 3136672271u );
        QCOMPARE( deviceId( 0xFFFFFFFFu, 0u ), uint64_t( 0xFFFFFFFFu ) );
    }

    void model_fromSampleRateAndDds() {
        // Функции «спросить модель» в API нет: максимум ряда плюс DDS —
        // единственный доступный признак.
        QCOMPARE( modelName( 48000000, true ), std::string( "ISDS205B" ) );
        QCOMPARE( modelName( 48000000, false ), std::string( "ISDS205A" ) );
        QCOMPARE( modelName( 100000000, false ), std::string( "ISDS210B" ) );
        QVERIFY( modelName( 7, false ).find( "UNKNOWN" ) == 0 );
    }

    void adcBits_twelveOnlyFor2062() {
        QCOMPARE( adcBits( "ISDS205B" ), 8 );
        QCOMPARE( adcBits( "ISDS2062B" ), 12 );
    }

    void acDc_polarityIsInverted() {
        // Единственная функция семейства Is* с обратной полярностью:
        // 0 = поддерживается. На ISDS205B измерено 1, то есть связь
        // через библиотеку не переключается.
        QVERIFY( acDcSupported( 0 ) );
        QVERIFY( !acDcSupported( 1 ) );
    }

    void clip_flagIsPerChannel() {
        QVERIFY( clipped( 1 ) );
        QVERIFY( !clipped( 0 ) );
    }

    // --- длина кадра ---

    void commonLength_takesTheShorter() {
        // Каналы возвращают разное число отсчётов; считать по окну
        // можно только по общей длине.
        QCOMPARE( commonLength( 4096, 2044 ), 2044u );
        QCOMPARE( commonLength( 0, 2044 ), 0u );
    }

    // --- сырой код АЦП ---

    void codeDifference_isExactInteger() {
        // Внутри библиотеки отсчёт получается как code*resolution+bias;
        // bias наружу не выставлен, поэтому точно восстанавливается
        // разность кодов, а не сам код.
        const double res = 0.003355;
        QVERIFY( qFuzzyCompare( codeDifference( 10 * res, 0.0, res ) + 1.0, 11.0 ) );
        QCOMPARE( codeDifference( 1.0, 0.0, 0.0 ), 0.0 );
    }

    // --- охрана единицы: вольты или милливольты ---

    void resolution_plausibleInVolts() {
        // Измерено на ISDS205B при пределе ±1000 мВ: шаг 0.003355.
        // Полная шкала 0.003355*256 = 0.859 В против окна 2 В — тот же
        // порядок, значит отсчёты в вольтах.
        const RangeMv r{ -1000, 1000 };
        QVERIFY( resolutionPlausible( 0.003355, r, 8, 1.0 ) );
        // Тот же шаг, но если считать данные милливольтами: полная шкала
        // 0.86 мВ против окна 2 В — три порядка мимо, проверка обязана
        // отказать. Ради этого отличия она и написана.
        QVERIFY( !resolutionPlausible( 0.003355, r, 8, 1e-3 ) );
    }

    void resolution_refusesNonsense() {
        const RangeMv r{ -1000, 1000 };
        QVERIFY( !resolutionPlausible( 0.0, r, 8, 1.0 ) );
        QVERIFY( !resolutionPlausible( 0.003355, RangeMv{ 0, 0 }, 8, 1.0 ) );
        QVERIFY( !resolutionPlausible( 0.003355, r, 0, 1.0 ) );
    }

    void resolution_perChannelValuesBothPass() {
        // 205A дал 0.003562 и 0.003421 на соседних каналах: это
        // заводская калибровка, усреднять её нельзя.
        const RangeMv r{ -1000, 1000 };
        QVERIFY( resolutionPlausible( 0.003562, r, 8, 1.0 ) );
        QVERIFY( resolutionPlausible( 0.003421, r, 8, 1.0 ) );
    }

    // --- имя файла калибровки ---

    void calibrationFileName_carriesPathMarker() {
        // Заводская калибровка пути A и наша калибровка пути B — разные
        // величины одного прибора. Общее имя файла подменило бы одну
        // другой молча.
        const std::string n = calibrationFileName( "ISDS205B", 0x7A00000000ull );
        QVERIFY( n.find( "pathA" ) != std::string::npos );
        QVERIFY( n.find( "ISDS205B" ) != std::string::npos );
        QVERIFY( calibrationFileName( "", 1 ).find( "UNKNOWN" ) != std::string::npos );
    }

    // --- множитель приведения ---

    void sampleToVolt_isASinglePlace() {
        // Значение стоит по заголовку вендора и по измерениям пути A.
        // Тест не утверждает, что оно верно, — он утверждает, что оно
        // ОДНО: смена величины обязана быть видна одной правкой.
        QCOMPARE( SAMPLE_TO_VOLT, 1.0 );
    }
};

QTEST_MAIN( TestIVdsoUnits )
#include "Itest_ivdsounits.moc"
