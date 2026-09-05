// SPDX-License-Identifier: GPL-3.0-or-later
//
// Тесты реестра параметров (params.h).
//
// Реестр заведён против конкретного класса ошибок: настройка, размазанная
// по пяти местам, расходится между ними молча. Поэтому проверяется не
// «работает ли реестр вообще», а именно те свойства, ради которых он
// существует: объявление одно, границы применяются в одном месте,
// круговой ход значения через get/set не теряет и не искажает.

#include <QtTest>

#include "ctpu.h"
#include "params.h"
#include "scopesettings.h"

class TestParams : public QObject {
    Q_OBJECT
  private slots:
    void testCtpuGroupDeclaredOnce();
    void testRoundTripThroughAccessors();
    void testClampKeepsScaleAlive();
    void testClampTextLength();
    void testClampChoiceRange();
    void testKeysAreStable();
};


/// Группа `ctpu` объявлена и содержит ровно те поля, что есть у канала.
/// Если поле добавят в структуру, не объявив, — тест не заметит; заметит
/// следующий: круговой ход по объявлениям должен покрывать всё, что
/// сохраняется.
void TestParams::testCtpuGroupDeclaredOnce() {
    const auto defs = Params::group( QStringLiteral( "ctpu" ) );
    QCOMPARE( int( defs.size() ), 4 );

    QStringList keys;
    for ( const Params::Def *d : defs ) {
        QVERIFY2( d->get != nullptr, "объявление без чтения бесполезно" );
        QVERIFY2( d->set != nullptr, "объявление без записи бесполезно" );
        QVERIFY2( !d->key.isEmpty(), "пустой ключ хранения" );
        QVERIFY2( !keys.contains( d->key ), "ключ объявлен дважды" );
        keys << d->key;
    }
    QVERIFY( keys.contains( QStringLiteral( "ctpuMode" ) ) );
    QVERIFY( keys.contains( QStringLiteral( "ctpuUnit" ) ) );
    QVERIFY( keys.contains( QStringLiteral( "ctpuK" ) ) );
    QVERIFY( keys.contains( QStringLiteral( "ctpuB" ) ) );
}


/// Записали через объявление — прочитали через объявление — совпало.
/// Это и есть то, что раньше делалось руками в двух разных файлах.
void TestParams::testRoundTripThroughAccessors() {
    DsoSettingsScope scope;
    scope.voltage.resize( 2 );

    const auto defs = Params::group( QStringLiteral( "ctpu" ) );
    for ( const Params::Def *d : defs ) {
        if ( d->key == QStringLiteral( "ctpuUnit" ) )
            d->set( scope, 1, QStringLiteral( "°C" ) );
        else if ( d->key == QStringLiteral( "ctpuK" ) )
            d->set( scope, 1, 100.0 );
        else if ( d->key == QStringLiteral( "ctpuB" ) )
            d->set( scope, 1, -273.15 );
        else if ( d->key == QStringLiteral( "ctpuMode" ) )
            d->set( scope, 1, int( CtPU::Mode::FORMULA ) );
    }

    for ( const Params::Def *d : defs ) {
        if ( d->key == QStringLiteral( "ctpuUnit" ) )
            QCOMPARE( d->get( scope, 1 ).toString(), QStringLiteral( "°C" ) );
        else if ( d->key == QStringLiteral( "ctpuK" ) )
            QCOMPARE( d->get( scope, 1 ).toDouble(), 100.0 );
        else if ( d->key == QStringLiteral( "ctpuB" ) )
            QCOMPARE( d->get( scope, 1 ).toDouble(), -273.15 );
        else if ( d->key == QStringLiteral( "ctpuMode" ) )
            QCOMPARE( d->get( scope, 1 ).toInt(), int( CtPU::Mode::FORMULA ) );
    }

    // Соседний канал не задет: параметр объявлен как поканальный.
    QCOMPARE( scope.voltage[ 0 ].ctpuUnit, QStringLiteral( "V" ) );
    QCOMPARE( scope.voltage[ 0 ].ctpuK, 1.0 );
}


/// k = 0 умножает всю шкалу В/дел на ноль: экран схлопывается в линию, и
/// прибор молча перестаёт показывать. Значение подменяется на 1.0 в
/// объявлении, а не в каждом обработчике ввода.
void TestParams::testClampKeepsScaleAlive() {
    DsoSettingsScope scope;
    scope.voltage.resize( 1 );
    const auto defs = Params::group( QStringLiteral( "ctpu" ) );
    const Params::Def *k = nullptr;
    for ( const Params::Def *d : defs )
        if ( d->key == QStringLiteral( "ctpuK" ) )
            k = d;
    QVERIFY( k != nullptr );

    k->set( scope, 0, 0.0 );
    QCOMPARE( scope.voltage[ 0 ].ctpuK, 1.0 );

    k->set( scope, 0, -2.5 ); // отрицательный масштаб допустим (инверсия датчика)
    QCOMPARE( scope.voltage[ 0 ].ctpuK, -2.5 );
}


/// Длина единицы ограничена объявлением, а не длиной поля ввода в одной
/// конкретной форме: форм может быть несколько, объявление одно.
void TestParams::testClampTextLength() {
    const auto defs = Params::group( QStringLiteral( "ctpu" ) );
    for ( const Params::Def *d : defs ) {
        if ( d->key != QStringLiteral( "ctpuUnit" ) )
            continue;
        const QVariant cut = Params::clamp( *d, QStringLiteral( "0123456789" ) );
        QCOMPARE( cut.toString().size(), 8 );
        const QVariant kept = Params::clamp( *d, QStringLiteral( "°C" ) );
        QCOMPARE( kept.toString(), QStringLiteral( "°C" ) );
    }
}


/// Значение вне перечня приводится к границе, а не проходит дальше:
/// невалидный индекс режима — это чтение чужого варианта.
void TestParams::testClampChoiceRange() {
    const auto defs = Params::group( QStringLiteral( "ctpu" ) );
    for ( const Params::Def *d : defs ) {
        if ( d->key != QStringLiteral( "ctpuMode" ) )
            continue;
        QCOMPARE( Params::clamp( *d, 99 ).toInt(), d->choices.size() - 1 );
        QCOMPARE( Params::clamp( *d, -5 ).toInt(), 0 );
        QCOMPARE( Params::clamp( *d, 1 ).toInt(), 1 );
    }
}


/// Ключи хранения — часть совместимости с уже сохранёнными настройками
/// пользователя. Их переименование молча теряет настройки, поэтому имена
/// зафиксированы тестом, а не только договорённостью.
void TestParams::testKeysAreStable() {
    QStringList keys;
    for ( const Params::Def *d : Params::group( QStringLiteral( "ctpu" ) ) )
        keys << d->key;
    keys.sort();
    QStringList expected{ QStringLiteral( "ctpuB" ), QStringLiteral( "ctpuK" ), QStringLiteral( "ctpuMode" ),
                          QStringLiteral( "ctpuUnit" ) };
    QCOMPARE( keys, expected );
}


// Явный GUILESS, как у соседних тестов: QTEST_MAIN при QT_GUI_LIB поднимал
// бы QGuiApplication и требовал дисплея, которого в CI нет.
QTEST_GUILESS_MAIN( TestParams )
#include "test_params.moc"
