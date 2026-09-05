// SPDX-License-Identifier: GPL-3.0-or-later
//
// Тесты дерева приборов (instrument/).
//
// Проверяется не «работает ли реестр вообще», а свойства, ради которых
// уровень заведён и которые должны выжить на дюжине приборов:
//   * прибор цепляется, не трогая фронтенд;
//   * транспорт — оснастка, а не порода: один прибор живёт на двух шинах;
//   * способности не смешиваются: измеритель не притворяется осциллографом;
//   * число степеней свободы привода объявляет сам привод, не приложение;
//   * смена типа слота разрывает прежнюю связь, а не бросает шину занятой.

#include <QtTest>

#include "instrument/instrumentregistry.h"

using namespace Instrument;

namespace {

/// Поддельная оснастка: помнит, открывали ли её, и сколько раз закрывали.
class FakeTransport : public Transport {
  public:
    FakeTransport( Bus bus, QString desc ) : m_bus( bus ), m_desc( std::move( desc ) ) {}
    Bus bus() const override { return m_bus; }
    QString description() const override { return m_desc; }
    bool open() override { return m_open = true; }
    void close() override {
        if ( m_open )
            ++closes;
        m_open = false;
    }
    bool isOpen() const override { return m_open; }
    bool exchange( const QByteArray &out, QByteArray &in ) override {
        if ( !m_open )
            return false;
        in = QByteArray( 11, char( 0 ) ); // сообщение фиксированной длины
        if ( !out.isEmpty() )
            in[ 0 ] = out[ 0 ];
        return true;
    }
    int closes = 0;

  private:
    Bus m_bus;
    QString m_desc;
    bool m_open = false;
};

TransportPtr serialT() {
    return std::make_unique< FakeTransport >( Bus::Serial, QStringLiteral( "/dev/pts/9 115200" ) );
}
TransportPtr bluetoothT() { // тот же прибор, другая шина
    return std::make_unique< FakeTransport >( Bus::Serial, QStringLiteral( "SPP Oscill-2A" ) );
}
TransportPtr usbT() {
    return std::make_unique< FakeTransport >( Bus::UsbBulk, QStringLiteral( "USB 1d50:608e" ) );
}
TransportPtr vendorT() { // путь A: тот же прибор через вендорскую библиотеку
    return std::make_unique< FakeTransport >( Bus::VendorDll, QStringLiteral( "vdso.dll" ) );
}
TransportPtr kopT() {
    return std::make_unique< FakeTransport >( Bus::UsbTmc, QStringLiteral( "USB0::TMC КОП:6" ) );
}


/// Осциллограф. Разбор один; на какой шине он живёт — дело оснастки.
class FakeScope : public Backend, public WaveformSource {
  public:
    FakeScope( TransportPtr t, Type type ) : Backend( std::move( t ) ), m_type( type ) {}
    bool probe() override {
        m_state.present = ( m_transport != nullptr );
        return m_state.present;
    }
    bool link() override {
        if ( !probe() || !m_transport->open() )
            return false;
        m_state.linked = true;
        m_state.transport = m_transport->description();
        return true;
    }
    void unlink() override {
        if ( m_transport )
            m_transport->close();
        m_state.linked = false;
    }
    void update() override { ++updates; }
    Flow flow() const override {
        // 30 МБ/с сырых; на стык они не выходят — каскад прореживает у истока.
        return Flow{ Pace::Stream, 30.0, 1'000'000, true };
    }
    Type type() const override { return m_type; }

    unsigned channelCount() const override { return 2; }
    double samplerate() const override { return 30e6; }

    int updates = 0;

  private:
    Type m_type;
};


/// Е7-12: прибор без процессора. Байт — кнопка, сообщение — 11 байт,
/// таймеров нет ни в приборе, ни здесь.
class FakePanel : public Backend, public PanelInstrument {
  public:
    explicit FakePanel( TransportPtr t ) : Backend( std::move( t ) ) {}
    bool probe() override {
        m_state.present = ( m_transport != nullptr );
        return m_state.present;
    }
    bool link() override {
        if ( !probe() || !m_transport->open() )
            return false;
        m_state.linked = true;
        m_state.transport = m_transport->description();
        return true;
    }
    void unlink() override {
        if ( m_transport )
            m_transport->close();
        m_state.linked = false;
    }
    void update() override { ++polls; }
    Flow flow() const override {
        // 11 байт за цикл; цикл — 32 такта, единицы измерений в секунду.
        return Flow{ Pace::OnEvent, 2.0, 11, false };
    }
    Type type() const override { return Type::E7_12; }
    int polls = 0;

    bool pressKey( unsigned code ) override {
        QByteArray in;
        if ( !m_transport->exchange( QByteArray( 1, char( code ) ), in ) )
            return false;
        m_raw = in;
        // Два вычислителя: сопутствующий параметр приходит вместе с основным.
        m_display = { Reading{ QStringLiteral( "C" ), 6.8e-6, QStringLiteral( "F" ), true },
                      Reading{ QStringLiteral( "tgδ" ), 0.012, QString(), true } };
        return true;
    }
    std::vector< Reading > display() const override { return m_display; }
    QByteArray rawMessage() const override { return m_raw; }

  private:
    std::vector< Reading > m_display;
    QByteArray m_raw;
};


/// Манипулятор с произвольным числом степеней свободы: оси объявляет он сам.
class FakeArm : public Backend, public Actuator {
  public:
    FakeArm( TransportPtr t, std::vector< AxisDef > axes )
        : Backend( std::move( t ) ), m_axes( std::move( axes ) ), m_pos( m_axes.size(), 0.0 ) {}
    bool probe() override {
        m_state.present = true;
        return true;
    }
    bool link() override {
        m_state.linked = m_transport && m_transport->open();
        return m_state.linked;
    }
    void unlink() override {
        if ( m_transport )
            m_transport->close();
        m_state.linked = false;
    }
    void update() override { ++polls; }
    Flow flow() const override { return Flow{ Pace::OnRequest, 100.0, 32, false }; }
    Type type() const override { return Type::Manipulator; }
    int polls = 0;

    std::vector< AxisDef > axes() const override { return m_axes; }
    bool moveTo( const std::vector< double > &target ) override {
        if ( target.size() != m_axes.size() )
            return false; // молчаливое дополнение нулями двигало бы не туда
        for ( std::size_t i = 0; i < target.size(); ++i ) {
            if ( target[ i ] < m_axes[ i ].min || target[ i ] > m_axes[ i ].max )
                return false;
            m_pos[ i ] = target[ i ];
        }
        return true;
    }
    std::vector< double > position() const override { return m_pos; }
    bool stop() override {
        ++stops;
        return true;
    }
    int stops = 0;

  private:
    std::vector< AxisDef > m_axes;
    std::vector< double > m_pos;
};

AxisDef ax( const char *n, const char *u, double lo, double hi, bool rot = false ) {
    return AxisDef{ QString::fromUtf8( n ), QString::fromUtf8( u ), lo, hi, rot };
}

} // namespace


class TestInstrument : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase();
    void testThreeInstrumentsCoexist();
    void testSameInstrumentTwoBuses();
    void testCapabilitiesDoNotMix();
    void testActuatorDeclaresItsOwnAxes();
    void testDoubleDeclareRejected();
    void testUnknownTypeGivesNullptr();
    void testRebuildUnlinksPrevious();
    void testNobodyWaitsForAnybody();
};


void TestInstrument::initTestCase() {
    // Объявление модели — одна строка. Фронтенд про модели не знает.
    QVERIFY( Registry::instance().declare( Type::ISDS205B, QStringLiteral( "ISDS-205B" ),
                                           []( int ) -> std::unique_ptr< Backend > {
                                               return std::make_unique< FakeScope >( usbT(), Type::ISDS205B );
                                           } ) );
    QVERIFY( Registry::instance().declare( Type::Oscill, QStringLiteral( "Oscill" ),
                                           []( int ) -> std::unique_ptr< Backend > {
                                               return std::make_unique< FakeScope >( serialT(), Type::Oscill );
                                           } ) );
    QVERIFY( Registry::instance().declare(
        Type::E7_12, QStringLiteral( "Е7-12" ),
        []( int ) -> std::unique_ptr< Backend > { return std::make_unique< FakePanel >( kopT() ); } ) );
    QVERIFY( Registry::instance().declare( Type::Manipulator, QStringLiteral( "Манипулятор" ),
                                           []( int ) -> std::unique_ptr< Backend > {
                                               return std::make_unique< FakeArm >(
                                                   serialT(), std::vector< AxisDef >{ ax( "X", "мм", 0, 200 ),
                                                                                      ax( "Y", "мм", 0, 200 ),
                                                                                      ax( "Z", "мм", 0, 80 ) } );
                                           } ) );
}


/// Связка из трёх приборов на трёх разных шинах в одном наборе. Пока мест
/// не было, каждый прибор требовал отдельной истории.
void TestInstrument::testThreeInstrumentsCoexist() {
    Set set;
    set.rebuild( { Type::ISDS205B, Type::Oscill, Type::E7_12 } );
    QCOMPARE( set.count(), 3 );
    for ( int i = 0; i < 3; ++i )
        QVERIFY( set.slot( i )->link() );

    QCOMPARE( set.slot( 0 )->bus(), Bus::UsbBulk );
    QCOMPARE( set.slot( 1 )->bus(), Bus::Serial );
    QCOMPARE( set.slot( 2 )->bus(), Bus::UsbTmc );
}


/// Главное следствие состава: ОДИН прибор живёт на разных шинах, и разбор
/// протокола при этом один. Наследование транспорта требовало бы второго
/// класса прибора на каждую шину.
///   ISDS205B — путь B (libusb) и путь A (vdso.dll);
///   Oscill   — COM/VCP и Bluetooth SPP.
void TestInstrument::testSameInstrumentTwoBuses() {
    FakeScope scope( usbT(), Type::ISDS205B );
    QVERIFY( scope.link() );
    QCOMPARE( scope.bus(), Bus::UsbBulk );

    scope.setTransport( vendorT() ); // тот же прибор, путь A
    QVERIFY( !scope.state().linked ); // связь разорвана явно, а не молча
    QVERIFY( scope.link() );
    QCOMPARE( scope.bus(), Bus::VendorDll );
    QCOMPARE( scope.channelCount(), 2u ); // способность та же, разбор тот же

    FakeScope osc( serialT(), Type::Oscill );
    QVERIFY( osc.link() );
    osc.setTransport( bluetoothT() );
    QVERIFY( osc.link() );
    QCOMPARE( osc.bus(), Bus::Serial );
    QVERIFY( osc.state().transport.contains( QStringLiteral( "SPP" ) ) );
}


/// Вид данных — способность. Свести приборы к одному интерфейсу «отдаёт
/// данные» можно только соврав про один из них.
void TestInstrument::testCapabilitiesDoNotMix() {
    Set set;
    set.rebuild( { Type::E7_12 } );
    Backend *b = set.slot( 0 );
    QVERIFY( b->link() );

    auto *panel = dynamic_cast< PanelInstrument * >( b );
    QVERIFY2( panel != nullptr, "Е7-12 обязан быть прибором-табло" );
    QVERIFY2( dynamic_cast< WaveformSource * >( b ) == nullptr, "Е7-12 не источник осциллограмм" );
    QVERIFY2( dynamic_cast< Actuator * >( b ) == nullptr, "Е7-12 не привод" );

    QVERIFY( panel->display().empty() ); // табло пусто — состояние, не ошибка
    QVERIFY( panel->pressKey( 0x21 ) );  // байт = кнопка передней панели
    QCOMPARE( panel->rawMessage().size(), 11 );
    const auto r = panel->display();
    QCOMPARE( int( r.size() ), 2 );
    QCOMPARE( r[ 0 ].unit, QStringLiteral( "F" ) );
    QVERIFY( r[ 1 ].unit.isEmpty() ); // tgδ безразмерна
}


/// Манипуляторов несколько, степени свободы у каждого свои. Число осей
/// объявляет прибор; приложение не держит его константой и не дополняет
/// вектор нулями — иначе привод пойдёт туда, куда никто не просил.
void TestInstrument::testActuatorDeclaresItsOwnAxes() {
    FakeArm three( serialT(), { ax( "X", "мм", 0, 200 ), ax( "Y", "мм", 0, 200 ), ax( "Z", "мм", 0, 80 ) } );
    FakeArm five( serialT(), { ax( "X", "мм", 0, 300 ), ax( "Y", "мм", 0, 300 ), ax( "Z", "мм", 0, 120 ),
                               ax( "поворот", "°", -180, 180, true ), ax( "схват", "мм", 0, 40 ) } );
    QVERIFY( three.link() );
    QVERIFY( five.link() );

    QCOMPARE( int( three.axes().size() ), 3 );
    QCOMPARE( int( five.axes().size() ), 5 );
    QCOMPARE( five.axes()[ 3 ].unit, QStringLiteral( "°" ) );
    QVERIFY( five.axes()[ 3 ].rotary );

    QVERIFY( three.moveTo( { 10, 20, 30 } ) );
    QCOMPARE( three.position()[ 2 ], 30.0 );

    QVERIFY2( !three.moveTo( { 10, 20 } ), "короткий вектор обязан быть отвергнут" );
    QVERIFY2( !three.moveTo( { 10, 20, 30, 40 } ), "длинный вектор обязан быть отвергнут" );
    QVERIFY2( !three.moveTo( { 10, 20, 999 } ), "выход за объявленную границу оси" );
    QCOMPARE( three.position()[ 2 ], 30.0 ); // отвергнутая подача ничего не сдвинула

    QVERIFY( five.stop() ); // останов работает всегда
    QCOMPARE( five.stops, 1 );
}


void TestInstrument::testDoubleDeclareRejected() {
    const bool second = Registry::instance().declare(
        Type::Oscill, QStringLiteral( "Другой" ),
        []( int ) -> std::unique_ptr< Backend > { return nullptr; } );
    QVERIFY( !second );
    QCOMPARE( Registry::instance().name( Type::Oscill ), QStringLiteral( "Oscill" ) );
}


void TestInstrument::testUnknownTypeGivesNullptr() {
    QVERIFY( Registry::instance().create( Type::Camera, 0 ) == nullptr );
    Set set;
    set.rebuild( { Type::Camera } );
    QCOMPARE( set.count(), 0 ); // необъявленный тип не создаёт пустой слот
}


/// Смена типа слота обязана разорвать связь прежнего прибора: иначе шина
/// остаётся занятой до перезапуска приложения.
void TestInstrument::testRebuildUnlinksPrevious() {
    Set set;
    set.rebuild( { Type::Oscill } );
    Backend *first = set.slot( 0 );
    QVERIFY( first->link() );
    auto *t = static_cast< FakeTransport * >( first->transport() );
    QVERIFY( t->isOpen() );

    set.rebuild( { Type::ISDS205B } );
    QCOMPARE( set.count(), 1 );
    QCOMPARE( set.slot( 0 )->type(), Type::ISDS205B );
}


/// Кто кого ждёт: НИКТО. Обход не является путём данных и трогает только
/// тех, кто сам объявил `Pace::OnRequest`. Иначе поток осциллографа
/// (30 МБ/с) встал бы на квитировании шины КОП у прибора, отдающего
/// единицы значений в секунду, — разрыв темпов семь порядков.
void TestInstrument::testNobodyWaitsForAnybody() {
    Set set;
    set.rebuild( { Type::ISDS205B, Type::E7_12, Type::Manipulator } );
    for ( int i = 0; i < 3; ++i )
        QVERIFY( set.slot( i )->link() );

    set.update();
    set.update();
    set.update();

    // Потоковый прибор обходом не трогается вовсе: он гонит данные сам.
    QCOMPARE( static_cast< FakeScope * >( set.slot( 0 ) )->updates, 0 );
    QCOMPARE( set.slot( 0 )->flow().pace, Pace::Stream );
    QVERIFY2( set.slot( 0 )->flow().decimatedAtSource,
              "30 МБ/с обязаны прореживаться у источника, а не на стыке" );

    // Событийный не опрашивается: опрос был бы ожиданием шины.
    QCOMPARE( static_cast< FakePanel * >( set.slot( 1 ) )->polls, 0 );
    QCOMPARE( set.slot( 1 )->flow().pace, Pace::OnEvent );

    // Опрашивается только тот, кто на это подписался.
    QCOMPARE( static_cast< FakeArm * >( set.slot( 2 ) )->polls, 3 );

    // Темпы отличаются на порядки — и это записано в паспорте, а не в
    // догадках вызывающего.
    QVERIFY( set.slot( 0 )->flow().bytesPerPortion > 1000 * set.slot( 1 )->flow().bytesPerPortion );

    // Несвязанный прибор не трогается тоже: опрос отсутствующего — источник
    // таймаутов, которые в интерфейсе выглядят как зависание.
    Set lone;
    lone.rebuild( { Type::Manipulator } );
    lone.update();
    QCOMPARE( static_cast< FakeArm * >( lone.slot( 0 ) )->polls, 0 );
}


QTEST_GUILESS_MAIN( TestInstrument )
#include "test_instrument.moc"
