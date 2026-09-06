// SPDX-License-Identifier: GPL-3.0-or-later
//
// Тесты прибора Oscill: кодек OBEX, пересчёты в физические единицы,
// разбор кадра, бэкенд на поддельной оснастке.
//
// ПРИБОРА НЕТ. Значит проверяется всё, что ДО прибора, и проверяется
// по-настоящему: у кодека на входе байты и на выходе байты, у пересчётов
// на входе числа и на выходе числа, у разбора кадра на входе тело
// заголовка `0x49`. Ничему из этого прибор не нужен, а значит отсутствие
// прибора не является основанием не проверить.
//
// ОЖИДАЕМЫЕ ПОСЛЕДОВАТЕЛЬНОСТИ ЗАПИСАНЫ РУКАМИ, а не получены из того же
// кодека. Тест, который строит ожидание проверяемой функцией, проверяет
// лишь то, что функция равна себе; такой тест не падает никогда. Поэтому
// здесь есть собственные сборщики пакета (`packetOf`, `seqHdr`, `oneHdr`,
// `quadHdr`) — они написаны по ПРАВИЛУ формата, а не вызовом `Builder`.
//
// АРИФМЕТИКА ПРИМЕРОВ ВЕНДОРА НЕ ВОСПРОИЗВОДИТСЯ. Из примеров
// первоисточника правилу удовлетворяет ровно один — запрос CONNECT
// `80 00 09 10 00 10 00 B0 A7` (сумма 512 ≡ 0), и он здесь сверяется
// побайтно. Прочие его примеры сумм и длин ошибочны: запись регистра `TS`
// объявляет длину `0x0E`, тогда как по правилу выходит `0x0F`. Тест
// требует `0x0F` — то есть проверяет ПРАВИЛО, а не опечатку.
//
// СТЕК. Oscill (OBEX поверх последовательного порта) не имеет ни одного
// общего факта ни с путём A (`vdso.dll`), ни с путём B (fx2lafw + libusb)
// осциллографа ISDS205 (`docs/ACCESS_PATHS.md`). Ни одно число этого
// файла оттуда не взято.
//
// ЕДИНИЦЫ. Скорость выборки — Sps/kSps/MSps; частота сигнала и тактовая
// частота контроллера — Hz/kHz/MHz. Величины прибора — в его собственных
// единицах: машинный такт 10 пс, период выборки в тактах×256, задержки в
// 12 тактах, чувствительность в мВ/дел, смещение в 1/256 диапазона АЦП.
//
// ПОЧЕМУ ЗДЕСЬ ЛЕЖИТ ЗАГЛУШКА ПОСЛЕДОВАТЕЛЬНОГО ПОРТА. Платформенный
// `serialtransport.cpp` в тест не берётся: он требует `windows.h` либо
// `termios.h` и живого порта. Но бэкенд опознаёт оснастку `dynamic_cast`
// к `SerialTransport` и спрашивает `SerialTransport::exists()` — значит
// сборщику нужны определения его методов, иначе не соберётся ни один
// тест бэкенда. Поэтому определения стоят ЗДЕСЬ, и за ними сидит не
// платформа, а поддельный прибор: тот же байтовый обмен, только
// мгновенный и без железа. Заглушка живёт исключительно в этом двоичном
// файле — приложение собирается с настоящим портом.
//
// СОСТОЯНИЕ УТВЕРЖДЕНИЙ. Всё проверенное здесь имеет метку **[СБОРКА]**:
// проверено сборкой, разбором и прогоном против поддельного прибора, но
// НЕ измерено на приборе Oscill. Ни одно утверждение этого файла не
// является измерением.

#include <QtTest>

#include <QMap>
#include <QPair>
#include <QString>

#include <cmath>
#include <cstdint>
#include <vector>

#include "instrument/instrumentregistry.h"
#include "instrument/oscill/obexcodec.h"
#include "instrument/oscill/oscillbackend.h"
#include "instrument/oscill/oscillprotocol.h"

using namespace Oscill;
using namespace Instrument;

namespace {

// ===========================================================================
// Собственная сборка байт: правило формата, записанное отдельно от кодека
// ===========================================================================

/// Байты из шестнадцатеричной записи: «80 00 09» → три байта. Ожидаемые
/// последовательности пишутся именно так — глазами их читать можно, а
/// вычислять их проверяемым кодом нельзя.
QByteArray hex( const char *text ) {
    return QByteArray::fromHex( QByteArray( text ).replace( ' ', QByteArray() ) );
}

/// Пакет в шестнадцатеричной записи. Нужна ровно затем, чтобы отчёт о
/// падении читался: `QCOMPARE` двух `QByteArray` печатает сырые байты, и
/// расхождение в одном из них в такой печати не видно.
QByteArray shown( const QByteArray &b ) { return b.toHex( ' ' ); }

/// Заголовок классов 00 и 01: идентификатор, двухбайтовая длина, тело.
/// Длина считает и сам идентификатор, и оба своих байта.
QByteArray seqHdr( uint8_t id, const QByteArray &body ) {
    const int wire = 3 + body.size();
    QByteArray h;
    h.append( char( id ) );
    h.append( char( ( wire >> 8 ) & 0xFF ) );
    h.append( char( wire & 0xFF ) );
    h.append( body );
    return h;
}

/// Заголовок класса 10: идентификатор и один байт. Поля длины нет.
QByteArray oneHdr( uint8_t id, uint8_t value ) {
    QByteArray h;
    h.append( char( id ) );
    h.append( char( value ) );
    return h;
}

/// Заголовок класса 11: идентификатор и ровно четыре байта старшим
/// вперёд. Поля длины нет.
QByteArray quadHdr( uint8_t id, const QByteArray &four ) {
    QByteArray h;
    h.append( char( id ) );
    h.append( four.left( 4 ) );
    return h;
}

QByteArray be32( uint32_t v ) {
    QByteArray b;
    b.append( char( ( v >> 24 ) & 0xFF ) );
    b.append( char( ( v >> 16 ) & 0xFF ) );
    b.append( char( ( v >> 8 ) & 0xFF ) );
    b.append( char( v & 0xFF ) );
    return b;
}

/// Пакет целиком: код, объявленная длина, заголовки и — по требованию —
/// контрольная сумма. Значение суммы таково, что сумма ВСЕХ байт пакета,
/// включая идентификатор `0xB0` и сам байт суммы, равна нулю по модулю 256.
QByteArray packetOf( uint8_t code, const QByteArray &headers, bool withCrc ) {
    const int total = 3 + headers.size() + ( withCrc ? 2 : 0 );
    QByteArray p;
    p.append( char( code ) );
    p.append( char( ( total >> 8 ) & 0xFF ) );
    p.append( char( total & 0xFF ) );
    p.append( headers );
    if ( withCrc ) {
        p.append( char( 0xB0 ) );
        unsigned sum = 0;
        for ( int i = 0; i < p.size(); ++i )
            sum += uint8_t( p.at( i ) );
        p.append( char( ( 256u - ( sum & 0xFFu ) ) & 0xFFu ) );
    }
    return p;
}

/// Разбор области заголовков — свой, чтобы поддельный прибор не зависел
/// от разбора, который сам же и проверяется.
std::vector< QPair< uint8_t, QByteArray > > walkHeaders( const QByteArray &area ) {
    std::vector< QPair< uint8_t, QByteArray > > out;
    int i = 0;
    while ( i < area.size() ) {
        const uint8_t id = uint8_t( area.at( i ) );
        const int cls = id & 0xC0;
        if ( cls == 0x00 || cls == 0x40 ) {
            if ( i + 3 > area.size() )
                break;
            const int len = ( int( uint8_t( area.at( i + 1 ) ) ) << 8 ) | int( uint8_t( area.at( i + 2 ) ) );
            if ( len < 3 || i + len > area.size() )
                break;
            out.push_back( { id, area.mid( i + 3, len - 3 ) } );
            i += len;
        } else if ( cls == 0x80 ) {
            if ( i + 2 > area.size() )
                break;
            out.push_back( { id, area.mid( i + 1, 1 ) } );
            i += 2;
        } else {
            if ( i + 5 > area.size() )
                break;
            out.push_back( { id, area.mid( i + 1, 4 ) } );
            i += 5;
        }
    }
    return out;
}

uint32_t numberOf( const QByteArray &body ) {
    uint32_t v = 0;
    for ( int i = 0; i < body.size(); ++i )
        v = ( v << 8 ) | uint32_t( uint8_t( body.at( i ) ) );
    return v;
}

/// Сравнение вещественных с НАЗВАННЫМ допуском. `QCOMPARE` у `double`
/// сравнивает с относительной точностью и на числах разного порядка
/// молчит там, где расхождение уже есть; здесь допуск виден в строке.
bool near( double a, double b, double eps ) { return std::fabs( a - b ) <= eps; }

// ===========================================================================
// Поддельный прибор
// ===========================================================================

/// \brief Прибор, которого нет: отвечает по правилам OBEX и корректирует
/// записываемые регистры по своим границам.
///
/// Корректировка — не украшение, а предмет проверки (П3): прибор вправе
/// подрезать запрошенное, и бэкенд обязан вернуть ФАКТ, а не пожелание.
/// Поэтому `V1`, `MC` и `P1` здесь зажимаются границами свойств, а `QS` —
/// пределом `QSh`, ровно как это делает эмулятор `tools/oscill-mock`.
///
/// Времени прибор не знает: ответ готов в тот же миг, когда пришёл
/// запрос. Сон здесь был бы выдуманной величиной — сроки линии живут в
/// `OscillTiming`, а не в поддельном приборе.
class FakeDevice {
  public:
    struct Value {
        int width = 0;    ///< 1, 2 или 4 байта значения на проводе
        uint32_t number = 0;
        QByteArray text;  ///< непусто — свойство текстовое (4 байта ASCII)
    };

    FakeDevice() { reset(); }

    // --- чем распоряжается тест ---

    bool mute = false;        ///< молчит: ни байта в ответ (отсутствие ответа)
    bool corruptNext = false; ///< следующий ответ уйдёт с испорченным байтом суммы
    int chunk = 0;            ///< >0 — тело кадра выдаётся порциями по столько байт
    QByteArray digitizeBody;  ///< что прибор отдаёт телом кадра на команду 'D'
    int writesAccepted = 0;

    void reset();
    QByteArray answer( const QByteArray &request );

    uint32_t regValue( const char *name ) const { return m_regs.value( name ).number; }

  private:
    QByteArray reply( uint8_t code, const QByteArray &headers );
    QByteArray valueHeader( const Value &v ) const;
    QByteArray nextChunk();
    uint32_t writeReg( const QByteArray &name, uint32_t wanted );
    int16_t signedOf( uint32_t v ) const { return int16_t( uint16_t( v ) ); }

    QMap< QByteArray, Value > m_props;
    QMap< QByteArray, Value > m_regs;
    QByteArray m_last;    ///< последний ответ: его повторяет опкод 0x92
    QByteArray m_pending; ///< недоотданный хвост тела кадра
};


void FakeDevice::reset() {
    m_props.clear();
    m_regs.clear();
    m_last.clear();
    m_pending.clear();
    writesAccepted = 0;

    // Паспорт базовой модели. `VHW` и `VSW` НЕ объявлены намеренно:
    // прибор обязан ответить на них `0xD1`, и по этому отказу клиент
    // переходит ко второму списку версий (`VHD`/`VHA`, `VSD`…`VSO`).
    // Какой из двух списков поддержан, показывает опрос, а не бумага.
    const auto ascii = []( const char *t ) {
        Value v;
        v.width = 4;
        v.text = QByteArray( t );
        return v;
    };
    m_props[ "VNM" ] = ascii( "Osc1" );
    m_props[ "VSN" ] = ascii( "0042" );
    m_props[ "VHD" ] = ascii( "2.00" );
    m_props[ "VHA" ] = ascii( "2.00" );
    m_props[ "VSD" ] = ascii( "2.31" );
    m_props[ "VSI" ] = ascii( "2.31" );
    m_props[ "VSC" ] = ascii( "2.31" );
    m_props[ "VSO" ] = ascii( "2.31" );

    const auto num = []( int w, uint32_t n ) {
        Value v;
        v.width = w;
        v.number = n;
        return v;
    };
    m_props[ "MCd" ] = num( 2, 0x07D0 ); // такт по умолчанию: 20 нс, тактовая 50 MHz
    m_props[ "MCl" ] = num( 2, 0x03E8 ); // минимальный такт: 10 нс, тактовая 100 MHz
    m_props[ "TOl" ] = num( 2, 0x2000 );
    m_props[ "TOv" ] = num( 4, 0x00000104 );
    m_props[ "TMl" ] = num( 2, 0x0010 );
    m_props[ "TMh" ] = num( 2, 0x0100 );
    m_props[ "TPl" ] = num( 4, 0x01000000 );
    m_props[ "TCh" ] = num( 2, 0x0400 );
    m_props[ "QSh" ] = num( 2, 0x0800 );
    m_props[ "TDl" ] = num( 4, 0x00000000 );
    m_props[ "TDh" ] = num( 4, 0x00100000 );
    m_props[ "V1h" ] = num( 2, 0x2710 ); // 10000 мВ/дел = 10 В/дел, НИЗШАЯ чувствительность
    m_props[ "V1l" ] = num( 2, 0x0014 ); // 20 мВ/дел, наивысшая
    m_props[ "P1h" ] = num( 2, 0x0180 ); // +384 в единицах 1/256 диапазона АЦП
    m_props[ "P1l" ] = num( 2, 0xFE80 ); // −384, знаковое
    m_props[ "D1m" ] = num( 2, 0x0064 );

    m_regs[ "MC" ] = num( 2, 0x07D0 );
    m_regs[ "TS" ] = num( 4, 0x2000 );
    m_regs[ "RS" ] = num( 1, 0x00 );
    m_regs[ "AP" ] = num( 1, 0x00 );
    m_regs[ "AR" ] = num( 1, 0x01 );
    m_regs[ "QS" ] = num( 2, 0x0200 );
    m_regs[ "TC" ] = num( 2, 0x0100 );
    m_regs[ "TD" ] = num( 4, 0x00000000 );
    m_regs[ "RT" ] = num( 1, 0x00 );
    m_regs[ "TA" ] = num( 4, 100000 );
    m_regs[ "TW" ] = num( 4, 100000 );
    m_regs[ "O1" ] = num( 1, 0x00 );
    m_regs[ "V1" ] = num( 2, 0x0100 );
    m_regs[ "P1" ] = num( 2, 0x0000 );
    m_regs[ "M1" ] = num( 1, 0x04 );
    m_regs[ "T1" ] = num( 1, 0x01 );
    m_regs[ "S1" ] = num( 1, 0x80 );

    // Кадр по умолчанию: раскладка с полем размера, один канал, обычный
    // формат, запуск по фронту, четыре выборки.
    digitizeBody = hex( "20 00 04 00 00 04 10 20 30 40" );
}


QByteArray FakeDevice::reply( uint8_t code, const QByteArray &headers ) {
    // Сумму прибор ставит всегда: клиенту она необязательна, а вот
    // проверить, что он её проверяет, иначе нечем.
    const QByteArray clean = packetOf( code, headers, true );
    m_last = clean;
    if ( !corruptNext )
        return clean;
    corruptNext = false;
    // Портится ИМЕННО байт суммы. Структура заголовков при этом остаётся
    // проходимой, и клиент обязан назвать причиной несошедшуюся сумму, а
    // не битый заголовок: перезапрос `0x92` полагается первой, а не
    // второй. Перезапрос отдаёт `m_last`, то есть неиспорченный ответ.
    QByteArray bad = clean;
    bad[ bad.size() - 1 ] = char( uint8_t( bad.at( bad.size() - 1 ) ) ^ 0xFF );
    return bad;
}


QByteArray FakeDevice::valueHeader( const Value &v ) const {
    if ( !v.text.isEmpty() )
        return quadHdr( 0xF1, v.text.leftJustified( 4, '\0' ) );
    if ( v.width == 1 )
        return oneHdr( 0xB1, uint8_t( v.number ) );
    if ( v.width == 2 )
        // Двухбайтовое значение прибора едет в ЧЕТЫРЁХбайтовом поле:
        // старшие два байта нулевые. Так требует первоисточник.
        return quadHdr( 0xF0, be32( v.number & 0xFFFFu ) );
    return quadHdr( 0xF1, be32( v.number ) );
}


uint32_t FakeDevice::writeReg( const QByteArray &name, uint32_t wanted ) {
    Value &r = m_regs[ name ];
    uint32_t v = wanted;
    if ( r.width == 1 )
        v &= 0xFFu;
    if ( r.width == 2 )
        v &= 0xFFFFu;

    // Прибор корректирует. Границы берутся из СВОЙСТВ, а не выдумываются:
    // именно так описан порядок в первоисточнике и так делает эмулятор.
    if ( name == "V1" ) {
        const uint32_t lo = m_props[ "V1l" ].number;
        const uint32_t hi = m_props[ "V1h" ].number;
        v = qBound( qMin( lo, hi ), v, qMax( lo, hi ) );
    } else if ( name == "MC" ) {
        const uint32_t lo = m_props[ "MCl" ].number;
        const uint32_t hi = m_props[ "MCd" ].number;
        v = qBound( qMin( lo, hi ), v, qMax( lo, hi ) );
    } else if ( name == "P1" ) {
        const int16_t lo = signedOf( m_props[ "P1l" ].number );
        const int16_t hi = signedOf( m_props[ "P1h" ].number );
        v = uint32_t( uint16_t( qBound( lo, signedOf( v ), hi ) ) );
    } else if ( name == "QS" ) {
        v = qMin( v, m_props[ "QSh" ].number );
    }

    r.number = v;
    ++writesAccepted;
    return v;
}


QByteArray FakeDevice::nextChunk() {
    const int n = qMin( chunk, int( m_pending.size() ) );
    const QByteArray part = m_pending.left( n );
    m_pending.remove( 0, n );
    if ( m_pending.isEmpty() )
        // `0x49` закрывает объект: это последняя или единственная часть.
        return reply( 0xA0, seqHdr( 0x49, part ) );
    return reply( 0x90, seqHdr( 0x48, part ) );
}


QByteArray FakeDevice::answer( const QByteArray &request ) {
    if ( mute )
        // Ноль байт есть ОТСУТСТВИЕ ответа. Это не пустой пакет: пустой
        // пакет — три байта, и клиент обязан различать эти два случая.
        return QByteArray();
    if ( request.size() < 3 )
        return QByteArray();

    const uint8_t op = uint8_t( request.at( 0 ) );

    if ( op == 0x92 )
        // Перезапрос последнего ответа. Отдаётся неиспорченный: смысл
        // перезапроса в том, что второй раз доедет целым.
        return m_last;

    if ( op == 0x80 ) {
        // Ответ на CONNECT: версия, флаги, приёмный буфер ПРИБОРА, и
        // только потом заголовки. Идентификатор соединения `0xCB` в
        // документах вендора не описан — прибор называет его сам.
        QByteArray fixed;
        fixed.append( char( 0x10 ) );
        fixed.append( char( 0x00 ) );
        fixed.append( char( 0x10 ) );
        fixed.append( char( 0x00 ) ); // 0x1000 = 4096 байт
        return reply( 0xA0, fixed + quadHdr( 0xCB, hex( "12 34 56 78" ) ) );
    }
    if ( op == 0xFF ) {
        m_pending.clear(); // Abort прекращает начатую передачу
        return reply( 0xA0, QByteArray() );
    }
    if ( op == 0x81 || op == 0x91 )
        return reply( 0xA0, QByteArray() );

    QByteArray propName, regName, cmdName;
    bool hasValue = false;
    uint32_t value = 0;
    for ( const auto &h : walkHeaders( request.mid( 3 ) ) ) {
        switch ( h.first ) {
        case 0x70:
            propName = h.second;
            break;
        case 0x71:
            regName = h.second;
            break;
        case 0x72:
            cmdName = h.second;
            break;
        case 0xB1:
        case 0xF0:
        case 0xF1:
            hasValue = true;
            value = numberOf( h.second );
            break;
        default:
            break;
        }
    }

    const bool isGet = ( op == 0x03 || op == 0x83 );

    if ( !regName.isEmpty() ) {
        if ( !m_regs.contains( regName ) )
            return reply( 0xD1, QByteArray() ); // такого регистра нет
        if ( hasValue ) {
            const uint32_t actual = writeReg( regName, value );
            if ( !isGet )
                // Запись пакетом PUT факта не несёт: клиенту придётся
                // спросить значение отдельным чтением.
                return reply( 0xA0, QByteArray() );
            // Совмещённая транзакция: тем же ответом идёт ФАКТ.
            Value shownValue = m_regs[ regName ];
            shownValue.number = actual;
            return reply( 0xA0, seqHdr( 0x71, regName ) + valueHeader( shownValue ) );
        }
        return reply( 0xA0, seqHdr( 0x71, regName ) + valueHeader( m_regs[ regName ] ) );
    }

    if ( !propName.isEmpty() ) {
        if ( !m_props.contains( propName ) )
            return reply( 0xD1, QByteArray() ); // такого свойства у прибора нет
        return reply( 0xA0, seqHdr( 0x70, propName ) + valueHeader( m_props[ propName ] ) );
    }

    if ( !cmdName.isEmpty() ) {
        if ( cmdName == "D" ) {
            if ( chunk > 0 && digitizeBody.size() > chunk ) {
                m_pending = digitizeBody;
                return nextChunk();
            }
            return reply( 0xA0, seqHdr( 0x49, digitizeBody ) );
        }
        if ( cmdName == "C" )
            return reply( 0xA0, QByteArray() );
        return reply( 0xD1, QByteArray() );
    }

    if ( isGet ) {
        // GET без предметного заголовка — запрос ПРОДОЛЖЕНИЯ длинного
        // ответа. Форма (`0x03` или `0x83`) первоисточником не уточнена,
        // и прибор обязан принимать обе.
        if ( !m_pending.isEmpty() )
            return nextChunk();
        return reply( 0xA0, seqHdr( 0x49, QByteArray() ) );
    }

    return reply( 0xC0, QByteArray() ); // запрос не понят
}


/// Порты, которые «есть в системе» у заглушки: имя порта → прибор за ним.
/// Их наличие и есть ответ `SerialTransport::exists()`.
QMap< QString, FakeDevice * > &fakePorts() {
    static QMap< QString, FakeDevice * > m;
    return m;
}

} // namespace

// ===========================================================================
// Заглушка последовательного порта
// ===========================================================================
//
// Определения методов `SerialTransport` — здесь и только для этого
// двоичного файла. Платформенный `serialtransport.cpp` в тест не
// включён (он требует `windows.h`/`termios.h` и живого порта), но
// бэкенду его класс нужен: он опознаёт оснастку `dynamic_cast`-ом, а
// значит сборщику нужна таблица виртуальных функций, и спрашивает
// `exists()` в `probe()`. Заглушка отдаёт байты поддельного прибора
// мгновенно: у неё нет ни сроков, ни очередей — сроки живут в бэкенде и
// проверяются его собственным поведением, а не сном здесь.

namespace Instrument {

struct SerialTransport::Impl {
    SerialParams params;
    FakeDevice *device = nullptr;
    bool opened = false;
    QByteArray rx; ///< то, что прибор уже сказал, а мы ещё не забрали
};


SerialTransport::SerialTransport( SerialParams params ) : m_impl( new Impl ) {
    m_impl->params = std::move( params );
}

SerialTransport::~SerialTransport() = default;

Bus SerialTransport::bus() const { return Bus::Serial; }

QString SerialTransport::description() const {
    return QStringLiteral( "%1 %2" ).arg( m_impl->params.port ).arg( m_impl->params.baudRate );
}


bool SerialTransport::open() {
    m_impl->device = fakePorts().value( m_impl->params.port, nullptr );
    if ( !m_impl->device ) {
        m_lastError = QStringLiteral( "порта %1 нет" ).arg( m_impl->params.port );
        return false;
    }
    m_impl->rx.clear();
    m_impl->opened = true;
    return true;
}


void SerialTransport::close() {
    m_impl->opened = false;
    m_impl->rx.clear();
}

bool SerialTransport::isOpen() const { return m_impl->opened; }


bool SerialTransport::write( const QByteArray &data ) {
    if ( !m_impl->opened || !m_impl->device ) {
        m_lastError = QStringLiteral( "порт закрыт" );
        return false;
    }
    m_impl->rx.append( m_impl->device->answer( data ) );
    return true;
}


QByteArray SerialTransport::read( int maxBytes ) {
    if ( !m_impl->opened || maxBytes <= 0 )
        return QByteArray();
    const QByteArray out = m_impl->rx.left( maxBytes );
    m_impl->rx.remove( 0, out.size() );
    return out;
}


int SerialTransport::bytesAvailable() const { return m_impl->opened ? int( m_impl->rx.size() ) : -1; }

QByteArray SerialTransport::readExactly( int count, int /*timeoutMs*/ ) {
    // Ждать нечего: ответ уже лежит в буфере либо его не будет вовсе.
    // Короткий результат означает «пакет не дочитан» — ровно то, что
    // договор и обещает вызывающему.
    return read( count );
}

void SerialTransport::purge() { m_impl->rx.clear(); }


bool SerialTransport::setBaudRate( int baud ) {
    if ( !m_impl->opened || baud <= 0 )
        return false;
    m_impl->params.baudRate = baud;
    return true;
}

const SerialParams &SerialTransport::params() const { return m_impl->params; }

bool SerialTransport::exists( const QString &port ) { return fakePorts().contains( port ); }

std::vector< QString > SerialTransport::enumerate() {
    std::vector< QString > out;
    for ( auto it = fakePorts().cbegin(); it != fakePorts().cend(); ++it )
        out.push_back( it.key() );
    return out;
}

} // namespace Instrument

namespace {

/// Оснастка, которая НЕ последовательный порт. Нужна одному утверждению:
/// `probe()` обязан отказать и назвать причину, а не притвориться, что
/// опознал чужую шину.
class PlainTransport : public Transport {
  public:
    Bus bus() const override { return Bus::UsbBulk; }
    QString description() const override { return QStringLiteral( "USB 1d50:608e" ); }
    bool open() override { return m_open = true; }
    void close() override { m_open = false; }
    bool isOpen() const override { return m_open; }

  private:
    bool m_open = false;
};


/// Стенд: поддельный прибор и порт, на котором он «есть в системе».
/// Порт снимается с учёта разрушением стенда — иначе следующий тест
/// нашёл бы прибор предыдущего.
struct Bench {
    FakeDevice device;
    QString port;

    explicit Bench( const QString &name = QStringLiteral( "ПОРТ-ПОДДЕЛКА" ) ) : port( name ) {
        fakePorts().insert( port, &device );
    }
    ~Bench() { fakePorts().remove( port ); }

    TransportPtr transport() const {
        SerialParams p;
        p.port = port;
        p.baudRate = SerialSpeeds::WORK;
        return std::make_unique< SerialTransport >( p );
    }
};

} // namespace


class TestOscill : public QObject {
    Q_OBJECT
  private slots:
    // --- кодек OBEX: сборка ---
    void testHeaderClassIsRuleNotTable();
    void testHeaderWireSize();
    void testConnectMatchesVendorExample();
    void testBarePackets();
    void testSetSpeedPacket();
    void testReadRequestsByteForByte();
    void testWriteRequestsByteForByte();
    void testBuilderRefusesWrongBody();
    void testBuilderRespectsTxLimit();
    void testSpeedDivisor();

    // --- кодек OBEX: контрольная сумма ---
    void testChecksumRule();
    void testChecksumNotConfusedByData();

    // --- кодек OBEX: разбор ---
    void testParseFourHeaderClasses();
    void testParseMalformedHeaderRefuses();
    void testParseTruncatedAndOverlong();
    void testParseChecksumBeforeHeaders();
    void testParseCorruptionStaysInsideBuffer();
    void testConnectResponse();
    void testBodyAssembler();

    // --- знание о приборе: значения из ответа ---
    void testValueFromResponse();

    // --- пересчёты в физические единицы ---
    void testTickAndClock();
    void testSamplePeriodAndRate();
    void testDelayUnits();
    void testSensitivity();
    void testSignedOffset();
    void testOffsetNeedsCodeSpan();
    void testTriggerLevel();
    void testChannelScaleAndCodes();
    void testCaptureWindow();
    void testFastRealtimeTicks();

    // --- побитовые раскладки ---
    void testBitLayouts();
    void testSampleFormatSizes();
    void testInitOrderAndDependents();

    // --- кадр ---
    void testFrameAllFiveFormats();
    void testFrameTwoChannels();
    void testFrameDeclaredSizeLargerThanBody();
    void testFrameOddArrayInPeakMode();
    void testFrameLayoutDetection();
    void testFrameRollHeadAndEmptyBody();
    void testFrameUnknownFormatIsThirdState();
    void testTimeoutTriggerIsValidFrame();

    // --- бэкенд ---
    void testProbeDoesNotOpenPort();
    void testProbeRejectsForeignTransport();
    void testLinkReadsPassport();
    void testUnlinkForgetsPassportKeepsFrame();
    void testUpdateWithoutLinkIsSafe();
    void testFramesThroughBackend();
    void testBackendTimeoutFrameIsNotAnError();
    void testLongAnswerAssembled();
    void testDeviceCorrectionReturnsFact();
    void testDeviceCorrectionOnPutStyle();
    void testApplySettingsShowsEveryCorrection();
    void testCorruptedAnswerIsRepeatedOnce();
    void testSilentDeviceFailsLinkWithReason();
    void testFlowPaceFollowsAcquisitionMode();
    void testRegistryDeclaration();
};

// ===========================================================================
// Кодек OBEX: сборка
// ===========================================================================

/// Класс заголовка задан ДВУМЯ СТАРШИМИ БИТАМИ, а не списком известных
/// имён. Проверяется именно это: сборщик эталонной реализации писал поле
/// длины по перечню из четырёх идентификаторов, и любой пятый собрался бы
/// битым. Поэтому здесь спрашиваются и объявленные идентификаторы, и
/// заведомо посторонние.
void TestOscill::testHeaderClassIsRuleNotTable() {
    QCOMPARE( headerClass( uint8_t( HeaderId::Name ) ), HeaderClass::Unicode );
    QCOMPARE( headerClass( uint8_t( HeaderId::BodyPart ) ), HeaderClass::ByteSeq );
    QCOMPARE( headerClass( uint8_t( HeaderId::BodyEnd ) ), HeaderClass::ByteSeq );
    QCOMPARE( headerClass( uint8_t( HeaderId::Property ) ), HeaderClass::ByteSeq );
    QCOMPARE( headerClass( uint8_t( HeaderId::Register ) ), HeaderClass::ByteSeq );
    QCOMPARE( headerClass( uint8_t( HeaderId::Command ) ), HeaderClass::ByteSeq );
    QCOMPARE( headerClass( uint8_t( HeaderId::Crc ) ), HeaderClass::Byte1 );
    QCOMPARE( headerClass( uint8_t( HeaderId::Value1 ) ), HeaderClass::Byte1 );
    QCOMPARE( headerClass( uint8_t( HeaderId::ConnectionId ) ), HeaderClass::Byte4 );
    QCOMPARE( headerClass( uint8_t( HeaderId::Value2 ) ), HeaderClass::Byte4 );
    QCOMPARE( headerClass( uint8_t( HeaderId::Value4 ) ), HeaderClass::Byte4 );

    // Идентификаторы, которых нет ни в одном перечне. Правило обязано
    // ответить и про них.
    QCOMPARE( headerClass( uint8_t( 0x3F ) ), HeaderClass::Unicode );
    QCOMPARE( headerClass( uint8_t( 0x7F ) ), HeaderClass::ByteSeq );
    QCOMPARE( headerClass( uint8_t( 0xBF ) ), HeaderClass::Byte1 );
    QCOMPARE( headerClass( uint8_t( 0xC5 ) ), HeaderClass::Byte4 );
}


/// У классов `Byte1` и `Byte4` размер тела на проводной размер не влияет:
/// он задан классом. У последовательностей поле длины считает и сам
/// идентификатор, и оба своих байта.
void TestOscill::testHeaderWireSize() {
    QCOMPARE( headerWireSize( uint8_t( HeaderId::Value1 ), 0 ), 2 );
    QCOMPARE( headerWireSize( uint8_t( HeaderId::Value1 ), 40 ), 2 );
    QCOMPARE( headerWireSize( uint8_t( HeaderId::Value4 ), 0 ), 5 );
    QCOMPARE( headerWireSize( uint8_t( HeaderId::Value4 ), 40 ), 5 );
    QCOMPARE( headerWireSize( uint8_t( HeaderId::Register ), 2 ), 5 );
    QCOMPARE( headerWireSize( uint8_t( HeaderId::BodyEnd ), 0 ), 3 );
    // Отрицательный размер тела — не «минус два байта», а пустое тело.
    QCOMPARE( headerWireSize( uint8_t( HeaderId::BodyEnd ), -7 ), 3 );
}


/// Единственный арифметически согласованный пример вендора, сверенный
/// побайтно: `80 00 09 10 00 10 00 B0 A7`, сумма ровно 512 ≡ 0.
void TestOscill::testConnectMatchesVendorExample() {
    QCOMPARE( shown( makeConnect( CLIENT_RX_BUFFER, true ) ), QByteArray( "80 00 09 10 00 10 00 b0 a7" ) );
    QCOMPARE( shown( makeConnect( CLIENT_RX_BUFFER, false ) ), QByteArray( "80 00 07 10 00 10 00" ) );
    // Приёмный буфер клиента — поле пакета, а не заголовок: два байта
    // старшим вперёд сразу после версии и флагов.
    QCOMPARE( shown( makeConnect( 255, false ) ), QByteArray( "80 00 07 10 00 00 ff" ) );
    QVERIFY( checksumValid( makeConnect( CLIENT_RX_BUFFER, true ) ) );
}


/// Пакет без заголовков — законная форма, а не вырожденная: длина 3 есть
/// сам код плюс два байта длины.
void TestOscill::testBarePackets() {
    QCOMPARE( shown( makeAbort() ), QByteArray( "ff 00 03" ) );
    QCOMPARE( shown( makeDisconnect() ), QByteArray( "81 00 03" ) );
    QCOMPARE( shown( makeRepeatLast( false ) ), QByteArray( "92 00 03" ) );
    QCOMPARE( shown( makeRepeatLast( true ) ), QByteArray( "92 00 05 b0 b9" ) );
    QCOMPARE( shown( makeBare( Op::Get ) ), QByteArray( "03 00 03" ) );
    QCOMPARE( shown( makeBare( Op::GetFinal ) ), QByteArray( "83 00 03" ) );
}


/// Коэффициент скорости — ГОЛОЕ поле после длины, а не заголовок: отсюда
/// длина 4 без суммы и 6 с ней. Оформи его заголовком — вышло бы шесть
/// байт без суммы, и прибор прочёл бы другое число.
void TestOscill::testSetSpeedPacket() {
    QCOMPARE( shown( makeSetSpeed( 16, false ) ), QByteArray( "91 00 04 10" ) );
    QCOMPARE( shown( makeSetSpeed( 16, true ) ), QByteArray( "91 00 06 10 b0 a9" ) );
    QVERIFY( hasChecksum( makeSetSpeed( 16, true ) ) );
    QVERIFY( checksumValid( makeSetSpeed( 16, true ) ) );
}


/// Чтение свойства и регистра: имя ASCII заголовком-последовательностью,
/// пакет GET с установленным старшим битом.
void TestOscill::testReadRequestsByteForByte() {
    QCOMPARE( shown( makeReadRegister( Register::TS, false ) ), QByteArray( "83 00 08 71 00 05 54 53" ) );
    QCOMPARE( shown( makeReadRegister( Register::TS, true ) ),
              QByteArray( "83 00 0a 71 00 05 54 53 b0 a6" ) );
    // Имя свойства — ровно три символа, имя регистра — два, имя команды — один.
    QCOMPARE( shown( makeReadProperty( Property::MCd, false ) ), QByteArray( "83 00 09 70 00 06 4d 43 64" ) );
    QCOMPARE( shown( makeDigitize( false ) ), QByteArray( "83 00 07 72 00 04 44" ) );
    // Калибровка идёт пакетом PUT, оцифровка — пакетом GET.
    QCOMPARE( shown( makeCalibrate( false ) ), QByteArray( "82 00 07 72 00 04 43" ) );

    // Загрузка фрагмента прошивки отвергается явно: к измерению она
    // отношения не имеет, а ошибка в ней стоит прибора.
    QVERIFY( makeCommand( 'F', false, false ).isEmpty() );
    // Байт вне печатаемого ASCII прибор прочтёт как испорченный пакет.
    QVERIFY( makeCommand( '\n', false, false ).isEmpty() );
    QVERIFY( makeCommand( char( 0x7F ), false, false ).isEmpty() );
}


/// Запись регистра: имя ОБЯЗАНО предшествовать значению, а ширина
/// заголовка значения берётся из ширины регистра — 1 байт даёт `0xB1`,
/// 2 байта дают `0xF0` (значение двухбайтовое, поле четырёхбайтовое),
/// 4 байта дают `0xF1`.
void TestOscill::testWriteRequestsByteForByte() {
    // Объявленная длина 0x0F = 3 + 5 + 5 + 2. Пример вендора печатает
    // здесь 0x0E — и он ошибочен: та же структура в его же примере
    // чтения регистра объявлена по правилу. Тест требует ПРАВИЛА.
    const QByteArray ts = makeWriteRegister( Register::TS, 0x2000, WriteStyle::CombinedGet, true );
    QCOMPARE( shown( ts ), QByteArray( "83 00 0f 71 00 05 54 53 f1 00 00 20 00 b0 90" ) );
    QCOMPARE( declaredLength( ts ), 0x0F );

    // Та же запись пакетом PUT: различается ровно опкод.
    QCOMPARE( shown( makeWriteRegister( Register::TS, 0x2000, WriteStyle::PutFinal, true ) ),
              QByteArray( "82 00 0f 71 00 05 54 53 f1 00 00 20 00 b0 91" ) );

    // Однобайтовый регистр: заголовок `0xB1`, поля длины у него нет.
    QCOMPARE( shown( makeWriteRegister( Register::RS, 0x07, WriteStyle::CombinedGet, false ) ),
              QByteArray( "83 00 0a 71 00 05 52 53 b1 07" ) );

    // Двухбайтовый: старшие два байта четырёхбайтового поля нулевые.
    QCOMPARE( shown( makeWriteRegister( Register::V1, 200, WriteStyle::CombinedGet, false ) ),
              QByteArray( "83 00 0d 71 00 05 56 31 f0 00 00 00 c8" ) );

    // Знаковый P1 = −384 едет дополнительным кодом ШИРИНЫ РЕГИСТРА:
    // 0xFE80, а не расширенное до 32 бит 0xFFFFFE80.
    QCOMPARE( shown( makeWriteRegister( Register::P1, uint32_t( uint16_t( -384 ) ),
                                        WriteStyle::CombinedGet, false ) ),
              QByteArray( "83 00 0d 71 00 05 50 31 f0 00 00 fe 80" ) );
}


/// Тело не по классу идентификатора не дополняется нулями и не режется:
/// дополнить значило бы отправить прибору ДРУГОЕ ЧИСЛО, а он его примет,
/// запишет в регистр и подтвердит.
void TestOscill::testBuilderRefusesWrongBody() {
    {
        Builder b( Op::GetFinal );
        b.add( HeaderId::Value1, hex( "01 02" ) ); // двухбайтовое тело у класса Byte1
        QVERIFY( !b.valid() );
        QVERIFY( b.build( false ).isEmpty() );
        QCOMPARE( b.size( false ), 0 ); // размер и содержимое обязаны сходиться
    }
    {
        Builder b( Op::GetFinal );
        b.add( HeaderId::Value4, hex( "01 02 03" ) ); // трёхбайтовое у класса Byte4
        QVERIFY( !b.valid() );
    }
    {
        // `addByte` на идентификаторе класса ByteSeq: через `add()` это
        // прошло бы целым и дало бы на проводе `71 00 04 xx` — байтовую
        // последовательность из одного байта вместо однобайтового
        // значения. Пакет формально верный и по смыслу другой.
        Builder b( Op::GetFinal );
        b.addByte( HeaderId::Register, 0x07 );
        QVERIFY( !b.valid() );
    }
    {
        Builder b( Op::GetFinal );
        b.addUint32( HeaderId::Register, 0x01020304 );
        QVERIFY( !b.valid() );
    }
    {
        // Негодный сборщик остаётся негодным: продолжать собирать пакет,
        // у которого одно поле уже неверно, — значит отправить его целым
        // с виду.
        Builder b( Op::GetFinal );
        b.addByte( HeaderId::Register, 0x07 );
        b.addName( HeaderId::Register, QByteArray( "TS" ) );
        QVERIFY( !b.valid() );
        QVERIFY( b.build( true ).isEmpty() );
    }
}


/// Предел исходящего пакета — 32 байта, и он действует независимо от
/// того, что прибор объявил своим приёмным буфером. Место этого числа
/// одно, и спрашивается оно у кодека.
void TestOscill::testBuilderRespectsTxLimit() {
    Builder small( Op::GetFinal );
    small.addName( HeaderId::Register, QByteArray( "TS" ) );
    QVERIFY( small.fits( true ) );
    QCOMPARE( small.size( false ), 8 );
    QCOMPARE( small.size( true ), 10 );

    Builder big( Op::PutFinal );
    big.addBody( QByteArray( 40, char( 0x5A ) ), true );
    QVERIFY( big.valid() );
    QVERIFY( !big.fits( false ) );
    QVERIFY( !big.fits( false, SAFE_TX_LIMIT ) );
    QVERIFY( big.fits( false, 64 ) ); // предел — аргумент, а не свойство сборщика

    // Ровно на пределе пакет ещё годен, на байт длиннее — уже нет.
    Builder edge( Op::PutFinal );
    edge.addBody( QByteArray( SAFE_TX_LIMIT - BASE_PACKET_LENGTH - 3, char( 0 ) ), true );
    QCOMPARE( edge.size( false ), SAFE_TX_LIMIT );
    QVERIFY( edge.fits( false ) );
    QVERIFY( !edge.fits( true ) ); // с суммой уже 34 байта
}


/// Скорость есть СЛЕДСТВИЕ коэффициента, а не наоборот: `1842000/16` даёт
/// 115125, а не 115200, и требовать от следствия целочисленности
/// бессмысленно. Скорость, для которой целого коэффициента нет, даёт ноль:
/// подставить «ближайшее» молча нельзя — хост откроет порт на одной
/// скорости, прибор перейдёт на другую, и линия умрёт молча.
void TestOscill::testSpeedDivisor() {
    QCOMPARE( int( speedDivisor( 115200 ) ), 16 );
    QCOMPARE( int( baudFromDivisor( 16 ) ), 115125 );
    QCOMPARE( int( speedDivisor( 921000 ) ), 2 );
    QCOMPARE( int( baudFromDivisor( 2 ) ), 921000 );
    QCOMPARE( int( speedDivisor( 9600 ) ), 192 );
    QCOMPARE( int( baudFromDivisor( 192 ) ), 9594 );
    // Обратный пересчёт сходится: 9594 снова даёт коэффициент 192.
    QCOMPARE( int( speedDivisor( 9594 ) ), 192 );

    QCOMPARE( int( speedDivisor( 0 ) ), 0 );
    QCOMPARE( int( baudFromDivisor( 0 ) ), 0 ); // «бесконечной скорости» не бывает
    QCOMPARE( int( speedDivisor( 1000000 ) ), 0 ); // ближайший коэффициент врёт на 8,6 %
    QCOMPARE( int( speedDivisor( 5000000 ) ), 0 ); // быстрее любого коэффициента

    QCOMPARE( int( speedDivisor( SerialSpeeds::WORK ) ), 16 );
}

// ===========================================================================
// Кодек OBEX: контрольная сумма
// ===========================================================================

/// Правило: значение таково, что сумма ВСЕГО пакета, включая
/// идентификатор `0xB0` и сам байт суммы, равна нулю по модулю 256.
void TestOscill::testChecksumRule() {
    QCOMPARE( int( checksumValue( hex( "01" ) ) ), 0xFF );
    QCOMPARE( int( checksumValue( hex( "00" ) ) ), 0x00 );
    // Сумма, уже кратная 256, даёт НОЛЬ, а не 256: ради этого случая во
    // внешнем выражении и стоит маска.
    QCOMPARE( int( checksumValue( hex( "80 80" ) ) ), 0x00 );
    QCOMPARE( int( checksumValue( hex( "ff ff ff" ) ) ), 0x03 );

    const QByteArray good = packetOf( 0xA0, seqHdr( 0x49, hex( "01 02 03" ) ), true );
    QVERIFY( hasChecksum( good ) );
    QVERIFY( checksumValid( good ) );

    QByteArray broken = good;
    broken[ 7 ] = char( uint8_t( broken.at( 7 ) ) ^ 0x01 );
    QVERIFY( hasChecksum( broken ) );
    QVERIFY( !checksumValid( broken ) );

    // Суммы НЕТ — это не «сумма не сошлась»: два разных исхода, и «да» на
    // вопрос без ответа было бы враньём.
    const QByteArray bare = packetOf( 0xA0, seqHdr( 0x49, hex( "01 02 03" ) ), false );
    QVERIFY( !hasChecksum( bare ) );
    QVERIFY( !checksumValid( bare ) );

    // Короче пяти байт пакет с суммой не бывает: три своих плюс два её.
    QVERIFY( !hasChecksum( makeAbort() ) );
}


/// Байт `0xB0` на предпоследнем месте как ЧАСТЬ ДАННЫХ. Взгляд на хвост
/// объявил бы такой ответ несущим сумму, сумма не сошлась бы, ответ ушёл
/// бы в перезапрос — а прибор повторил бы тот же пакет, и кадр терялся бы
/// навсегда, выглядя помехой на линии. Отсюда проход по структуре.
void TestOscill::testChecksumNotConfusedByData() {
    const QByteArray p = packetOf( 0xA0, seqHdr( 0x49, hex( "01 b0 02" ) ), false );
    QCOMPARE( shown( p ), QByteArray( "a0 00 09 49 00 06 01 b0 02" ) );
    QCOMPARE( int( uint8_t( p.at( p.size() - 2 ) ) ), 0xB0 ); // ловушка на месте
    QVERIFY( !hasChecksum( p ) );
    QVERIFY( !checksumValid( p ) );

    Response r;
    QCOMPARE( parseResponse( p, r ), ParseError::None );
    QVERIFY( !r.checksumPresent );
    QCOMPARE( int( r.headers.size() ), 1 );
    QCOMPARE( r.headers[ 0 ].body, hex( "01 b0 02" ) );
}

// ===========================================================================
// Кодек OBEX: разбор
// ===========================================================================

/// Все четыре класса заголовка в одной области.
void TestOscill::testParseFourHeaderClasses() {
    const QByteArray area = hex( "01 00 07 00 41 00 00" ) + hex( "48 00 05 aa bb" ) + hex( "b1 7f" ) +
                            hex( "f1 12 34 56 78" );
    std::vector< Header > out;
    QCOMPARE( parseHeaders( area, out ), ParseError::None );
    QCOMPARE( int( out.size() ), 4 );

    QCOMPARE( int( out[ 0 ].id ), 0x01 );
    QCOMPARE( out[ 0 ].body, hex( "00 41 00 00" ) );
    QCOMPARE( int( out[ 1 ].id ), 0x48 );
    QCOMPARE( out[ 1 ].body, hex( "aa bb" ) );
    QCOMPARE( int( out[ 2 ].id ), 0xB1 );
    QCOMPARE( int( out[ 2 ].byteValue() ), 0x7F );
    QCOMPARE( int( out[ 3 ].id ), 0xF1 );
    QCOMPARE( out[ 3 ].uint32Value(), uint32_t( 0x12345678 ) );

    // Значение не того размера даёт НОЛЬ, а не разобранный кусок: у
    // заголовка не того класса значения нет.
    QCOMPARE( int( out[ 1 ].byteValue() ), 0 );
    QCOMPARE( out[ 1 ].uint32Value(), uint32_t( 0 ) );

    // `0xF0`: значащие — младшие два байта четырёхбайтового поля.
    std::vector< Header > word;
    QCOMPARE( parseHeaders( hex( "f0 00 00 12 34" ), word ), ParseError::None );
    QCOMPARE( int( word[ 0 ].oscillWordValue() ), 0x1234 );
    // Если старшие два пришли ненулевыми, берутся ВСЁ РАВНО младшие:
    // подставить вместо них старшие значило бы вернуть число, которого
    // прибор не посылал. Порчу ловит сумма, а не этот пересчёт.
    QCOMPARE( parseHeaders( hex( "f0 ff ff 12 34" ), word ), ParseError::None );
    QCOMPARE( int( word[ 0 ].oscillWordValue() ), 0x1234 );

    // ASCII: хвостовые нули срезаются, ноль ВНУТРИ значения — нет.
    std::vector< Header > text;
    QCOMPARE( parseHeaders( hex( "f1 4f 6b 00 00" ), text ), ParseError::None );
    QCOMPARE( text[ 0 ].asciiValue(), QByteArray( "Ok" ) );
    QCOMPARE( parseHeaders( hex( "f1 4f 00 6b 00" ), text ), ParseError::None );
    QCOMPARE( text[ 0 ].asciiValue(), hex( "4f 00 6b" ) );
}


/// Место, где эталонная реализация уходит в вечный цикл: заголовок
/// класса 00 с длиной, не сдвигающей индекс. Здесь — отказ, а не цикл.
void TestOscill::testParseMalformedHeaderRefuses() {
    std::vector< Header > out;
    QCOMPARE( parseHeaders( hex( "01 00 00 00" ), out ), ParseError::MalformedHeader );
    QCOMPARE( parseHeaders( hex( "01 00 01" ), out ), ParseError::MalformedHeader );
    QCOMPARE( parseHeaders( hex( "01 00 02" ), out ), ParseError::MalformedHeader );
    // Объявленная длина за границей области.
    QCOMPARE( parseHeaders( hex( "01 00 09 aa" ), out ), ParseError::MalformedHeader );
    // Обрывок поля длины.
    QCOMPARE( parseHeaders( hex( "01 00" ), out ), ParseError::MalformedHeader );
    // Обрывки полей фиксированной ширины.
    QCOMPARE( parseHeaders( hex( "b1" ), out ), ParseError::MalformedHeader );
    QCOMPARE( parseHeaders( hex( "f1 12 34" ), out ), ParseError::MalformedHeader );

    // Пустое тело законно: `49 00 03` — штатный ответ ждущего запуска.
    QCOMPARE( parseHeaders( hex( "49 00 03" ), out ), ParseError::None );
    QCOMPARE( int( out.size() ), 1 );
    QVERIFY( out[ 0 ].body.isEmpty() );

    // Пустая область — ноль заголовков, а не ошибка.
    QCOMPARE( parseHeaders( QByteArray(), out ), ParseError::None );
    QVERIFY( out.empty() );
}


/// «Пакет не дочитан» и «длина не сошлась» — разные исходы: первый лечится
/// ожиданием, второй перезапросом. Смешать их значило бы либо ждать
/// вечно, либо перезапрашивать то, что ещё едет.
void TestOscill::testParseTruncatedAndOverlong() {
    const QByteArray good = packetOf( 0xA0, seqHdr( 0x49, hex( "01 02 03" ) ), false );
    Response r;
    QCOMPARE( parseResponse( good, r ), ParseError::None );
    QCOMPARE( int( r.code ), 0xA0 );
    QCOMPARE( r.declaredLength, 9 );
    QVERIFY( r.isSuccess() );
    QVERIFY( !r.isContinue() );
    QCOMPARE( int( r.headers.size() ), 1 );

    QCOMPARE( parseResponse( QByteArray(), r ), ParseError::Incomplete );
    QCOMPARE( parseResponse( good.left( 2 ), r ), ParseError::Incomplete );
    QCOMPARE( parseResponse( good.left( 3 ), r ), ParseError::Incomplete );
    QCOMPARE( parseResponse( good.left( 8 ), r ), ParseError::Incomplete );
    // Лишние байты — тоже расхождение: за пакетом в потоке уже может
    // лежать начало следующего, и склеенная пара разобралась бы как один
    // испорченный пакет.
    QCOMPARE( parseResponse( good + hex( "00" ), r ), ParseError::LengthMismatch );
    // Объявленная длина меньше собственного заголовка пакета — порча, а
    // не незавершённость: сколько ни жди, целым он не станет.
    QCOMPARE( parseResponse( hex( "a0 00 02" ), r ), ParseError::LengthMismatch );
    QCOMPARE( parseResponse( hex( "a0 00 00 aa" ), r ), ParseError::LengthMismatch );

    QCOMPARE( declaredLength( hex( "a0 00" ) ), -1 );
    QCOMPARE( declaredLength( good ), 9 );
}


/// Сумма проверяется ДО разбора заголовков: если она не сошлась, байты
/// области недостоверны, и `MalformedHeader` назвал бы следствие вместо
/// причины. Вызывающему нужна причина — по ней он шлёт перезапрос `0x92`.
void TestOscill::testParseChecksumBeforeHeaders() {
    QByteArray p = packetOf( 0xA0, seqHdr( 0x49, hex( "01 02 03" ) ), true );
    p[ 7 ] = char( uint8_t( p.at( 7 ) ) ^ 0x55 );

    Response r;
    QCOMPARE( parseResponse( p, r ), ParseError::BadChecksum );
    QVERIFY( r.checksumPresent );
    QVERIFY( !r.checksumOk );
    // Код ответа сохранён: по нему вызывающий отличит отказ прибора от
    // порчи линии.
    QCOMPARE( int( r.code ), 0xA0 );

    // Испорчено поле длины ЗАГОЛОВКА: область не проходится ни одной
    // раскладкой, но байт на месте идентификатора суммы стоит — и
    // причиной обязана быть названа сумма, а не заголовок.
    QByteArray q = packetOf( 0xA0, seqHdr( 0x49, hex( "01 02 03" ) ), true );
    q[ 5 ] = char( 0x7F );
    QCOMPARE( parseResponse( q, r ), ParseError::BadChecksum );
}


/// Обрезанный и повреждённый ввод: разбор обязан отказать, а не прочитать
/// за границей буфера. Проверяется систематически — каждым префиксом и
/// каждой одиночной порчей.
void TestOscill::testParseCorruptionStaysInsideBuffer() {
    // Пакет БЕЗ суммы: иначе почти всякая порча отсекалась бы суммой, и
    // проход по заголовкам не проверялся бы вовсе.
    const QByteArray good = packetOf(
        0xA0, seqHdr( 0x49, hex( "01 02 03 04 05 06" ) ) + oneHdr( 0xB1, 0x2A ) + quadHdr( 0xF1, hex( "de ad be ef" ) ),
        false );

    Response r;
    QCOMPARE( parseResponse( good, r ), ParseError::None );
    QCOMPARE( int( r.headers.size() ), 3 );

    // Всякий ПРЕФИКС целого пакета обязан быть отвергнут: объявленная
    // длина у него та же, а байт меньше.
    for ( int n = 0; n < good.size(); ++n ) {
        Response cut;
        QVERIFY2( parseResponse( good.left( n ), cut ) != ParseError::None,
                  qPrintable( QStringLiteral( "префикс длины %1 разобран как целый пакет" ).arg( n ) ) );
    }

    // Одиночная порча каждого байта. Требование не «отказать всегда» —
    // часть порч даёт законный пакет другого содержания, — а «если
    // разобралось, то заголовки лежат ВНУТРИ пакета и покрывают его
    // ровно». Выход за буфер нарушил бы это равенство.
    const uint8_t values[] = { 0x00, 0x01, 0x40, 0x7F, 0x80, 0xB0, 0xC0, 0xFF };
    for ( int i = 0; i < good.size(); ++i ) {
        for ( uint8_t v : values ) {
            QByteArray bad = good;
            bad[ i ] = char( v );
            Response out;
            if ( parseResponse( bad, out ) != ParseError::None )
                continue;
            int covered = 0;
            for ( const Header &h : out.headers ) {
                QVERIFY( h.body.size() >= 0 );
                QVERIFY( h.body.size() <= bad.size() );
                covered += headerWireSize( h.id, h.body.size() );
            }
            QCOMPARE( covered, out.declaredLength - BASE_PACKET_LENGTH );
        }
    }
}


/// У ответа на CONNECT перед заголовками стоят ЧЕТЫРЕ поля пакета, и
/// проход от третьего байта принял бы номер версии за идентификатор
/// заголовка. Отсюда отдельная функция разбора.
void TestOscill::testConnectResponse() {
    const QByteArray p = packetOf( 0xA0, hex( "10 00 00 26" ) + quadHdr( 0xCB, hex( "12 34 56 78" ) ), false );
    ConnectInfo info;
    QCOMPARE( parseConnectResponse( p, info ), ParseError::None );
    QCOMPARE( int( info.version ), 0x10 );
    QCOMPARE( int( info.flags ), 0x00 );
    QCOMPARE( int( info.deviceRxBuffer ), 0x26 );
    QVERIFY( info.deviceRxKnown );
    QVERIFY( info.hasConnectionId );
    QCOMPARE( info.connectionId, uint32_t( 0x12345678 ) );

    // Нулевой буфер прибором НЕ ОБЪЯВЛЕН, а не объявлен нулевым: прибора,
    // способного принять ноль байт, не существует, а потерянный байт даёт
    // ноль легко.
    ConnectInfo zero;
    QCOMPARE( parseConnectResponse( packetOf( 0xA0, hex( "10 00 00 00" ), false ), zero ), ParseError::None );
    QVERIFY( !zero.deviceRxKnown );
    QCOMPARE( int( zero.deviceRxBuffer ), int( DEVICE_RX_FALLBACK ) );
    QVERIFY( !zero.hasConnectionId );

    // Отказ прибора: фиксированных полей у такого пакета нет, читать из
    // него версию было бы чтением чужих байт. Код при этом сохранён.
    ConnectInfo refused;
    QCOMPARE( parseConnectResponse( packetOf( 0xC0, QByteArray(), false ), refused ), ParseError::NotConnect );
    QCOMPARE( int( refused.response.code ), 0xC0 );
    QCOMPARE( int( refused.version ), 0 );

    // Сумма проверяется ПРЕЖДЕ кода: испорченный байт кода — ровно то, от
    // чего сумма и защищает. Назвать порчу линии отказом прибора значило
    // бы лишить вызывающего единственного верного действия — перезапроса.
    QByteArray spoiled = packetOf( 0xC0, hex( "10 00 10 00" ), true );
    spoiled[ spoiled.size() - 1 ] = char( uint8_t( spoiled.at( spoiled.size() - 1 ) ) ^ 0xFF );
    ConnectInfo bad;
    QCOMPARE( parseConnectResponse( spoiled, bad ), ParseError::BadChecksum );

    // Успех при длине, не вмещающей фиксированные поля.
    ConnectInfo tiny;
    QCOMPARE( parseConnectResponse( packetOf( 0xA0, hex( "10 00" ), false ), tiny ), ParseError::MalformedHeader );
}


/// Накопитель — ПОСЛЕДОВАТЕЛЬНОСТЬ, а не словарь по идентификатору. В
/// словаре повторные `0x48` затирают друг друга, и длинный ответ там не
/// склеивается вовсе, ни при каких настройках.
void TestOscill::testBodyAssembler() {
    Response part;
    QCOMPARE( parseResponse( packetOf( 0x90, seqHdr( 0x48, hex( "aa bb" ) ) + seqHdr( 0x48, hex( "cc" ) ), false ),
                             part ),
              ParseError::None );
    QVERIFY( part.isContinue() );

    BodyAssembler asm1;
    QVERIFY( asm1.feed( part ) );
    QCOMPARE( asm1.body(), hex( "aa bb cc" ) ); // ОБА повтора взяты
    QCOMPARE( int( asm1.partCount() ), 2 );
    QVERIFY( !asm1.complete() );

    Response last;
    QCOMPARE( parseResponse( packetOf( 0xA0, seqHdr( 0x49, hex( "dd" ) ), false ), last ), ParseError::None );
    QVERIFY( asm1.feed( last ) );
    QVERIFY( asm1.complete() );
    QCOMPARE( asm1.body(), hex( "aa bb cc dd" ) );
    QCOMPARE( int( asm1.partCount() ), 3 );

    // Законченный объект больше не растёт: дописать к нему пришедшее
    // после `0x49` значило бы склеить два разных объекта в один.
    QVERIFY( !asm1.feed( last ) );
    QCOMPARE( asm1.body(), hex( "aa bb cc dd" ) );

    asm1.reset();
    QVERIFY( !asm1.complete() );
    QVERIFY( asm1.body().isEmpty() );
    QCOMPARE( int( asm1.partCount() ), 0 );

    // Ответ без тела объекта — не ошибка формата, а сообщение «ждать
    // нечего, разбирай сам».
    Response plain;
    QCOMPARE( parseResponse( packetOf( 0xA0, oneHdr( 0xB1, 0x05 ), false ), plain ), ParseError::None );
    QVERIFY( !asm1.feed( plain ) );

    // Пустое тело `0x49` — тоже ЧАСТЬ: штатный ответ ждущего запуска, у
    // которого истёк предел ожидания. Кадра нет, связь цела, и отличить
    // одно от другого можно только по тому, что часть ПРИШЛА.
    BodyAssembler asm2;
    Response empty;
    QCOMPARE( parseResponse( packetOf( 0xA0, seqHdr( 0x49, QByteArray() ), false ), empty ), ParseError::None );
    QVERIFY( asm2.feed( empty ) );
    QVERIFY( asm2.complete() );
    QVERIFY( asm2.body().isEmpty() );
    QCOMPARE( int( asm2.partCount() ), 1 );
}

// ===========================================================================
// Значение из ответа
// ===========================================================================

/// Величины НЕТ — это не ноль: ноль есть законное значение всех регистров
/// прибора, и отличить его от «не прочитано» вызывающий иначе не смог бы.
void TestOscill::testValueFromResponse() {
    Response named;
    QCOMPARE( parseResponse( packetOf( 0xA0, seqHdr( 0x71, QByteArray( "TS" ) ) + quadHdr( 0xF1, be32( 0x2000 ) ),
                                       false ),
                             named ),
              ParseError::None );

    uint32_t v = 0xDEADBEEF;
    QVERIFY( valueFromResponse( named, Register::TS, v ) );
    QCOMPARE( v, uint32_t( 0x2000 ) );

    // Имя в ответе, если прибор его назвал, обязано совпадать с
    // запрошенным: иначе это ответ про другой регистр.
    v = 0xDEADBEEF;
    QVERIFY( !valueFromResponse( named, Register::TD, v ) );
    QCOMPARE( v, uint32_t( 0xDEADBEEF ) ); // не тронуто

    // Имени нет вовсе — терпимо: первоисточник требует имени в ЗАПРОСЕ, а
    // про ответ такого требования не даёт. Строго там, где он говорит.
    Response nameless;
    QCOMPARE( parseResponse( packetOf( 0xA0, quadHdr( 0xF1, be32( 0x2000 ) ), false ), nameless ),
              ParseError::None );
    QVERIFY( valueFromResponse( nameless, Register::TD, v ) );
    QCOMPARE( v, uint32_t( 0x2000 ) );

    // Значения нет: `0xD1 Not Implemented` и просто пустой ответ дают
    // один исход — величины нет.
    Response none;
    QCOMPARE( parseResponse( packetOf( 0xD1, QByteArray(), false ), none ), ParseError::None );
    v = 0xDEADBEEF;
    QVERIFY( !valueFromResponse( none, Register::TS, v ) );
    QCOMPARE( v, uint32_t( 0xDEADBEEF ) );

    // Приведение по ширине из ТАБЛИЦЫ, а не по тому, что пришло: прибор
    // ответил четырёхбайтовым заголовком на однобайтовый регистр.
    Response wide;
    QCOMPARE( parseResponse( packetOf( 0xA0, seqHdr( 0x71, QByteArray( "RS" ) ) + quadHdr( 0xF1, be32( 0x01FF ) ),
                                       false ),
                             wide ),
              ParseError::None );
    QVERIFY( valueFromResponse( wide, Register::RS, v ) );
    QCOMPARE( v, uint32_t( 0xFF ) );

    // У текстового свойства числа нет вовсе: вернуть четыре байта ASCII
    // как целое значило бы выдать за величину то, что величиной не является.
    Response text;
    QCOMPARE( parseResponse( packetOf( 0xA0, seqHdr( 0x70, QByteArray( "VNM" ) ) + quadHdr( 0xF1, QByteArray( "Osc1" ) ),
                                       false ),
                             text ),
              ParseError::None );
    QVERIFY( !valueFromResponse( text, Property::VNM, v ) );

    // Текст берётся из заголовка ЗНАЧЕНИЯ, а не из заголовка имени: иначе
    // прибор ответил бы «VNM» на запрос «VNM».
    QByteArray raw;
    QVERIFY( asciiFromResponse( text, raw ) );
    QCOMPARE( raw, QByteArray( "Osc1" ) );

    // Числовое свойство читается числом.
    Response num;
    QCOMPARE( parseResponse( packetOf( 0xA0, seqHdr( 0x70, QByteArray( "MCd" ) ) + quadHdr( 0xF0, be32( 0x07D0 ) ),
                                       false ),
                             num ),
              ParseError::None );
    QVERIFY( valueFromResponse( num, Property::MCd, v ) );
    QCOMPARE( v, uint32_t( 0x07D0 ) );
}

// ===========================================================================
// Пересчёты в физические единицы
// ===========================================================================

/// Машинный такт в единицах 10 пс. `MCd = 0x07D0` — это 20 нс, то есть
/// тактовая частота контроллера 50 MHz (именно MHz: тактовая частота, а
/// не скорость выборки).
void TestOscill::testTickAndClock() {
    QCOMPARE( tickPs( 0x07D0 ), 20000.0 );
    QCOMPARE( tickPs( 0x03E8 ), 10000.0 );
    QCOMPARE( tickPs( 0 ), 0.0 );

    QCOMPARE( int( mcFromMHz( 50.0 ) ), 0x07D0 );
    QCOMPARE( int( mcFromMHz( 100.0 ) ), 0x03E8 );
    QCOMPARE( int( mcFromMHz( 0.0 ) ), 0 );
    QCOMPARE( int( mcFromTickPs( 20000.0 ) ), 2000 );

    // Ноль на входе — «такта нет». Ноль на выходе означает то же самое, а
    // не «нулевой такт».
    QCOMPARE( int( mcFromTickPs( 0.0 ) ), 0 );
    QCOMPARE( int( mcFromTickPs( -5.0 ) ), 0 );
    // Меньше одной единицы — единица, а не ноль: ноль означал бы, что
    // такта нет вовсе.
    QCOMPARE( int( mcFromTickPs( 1.0 ) ), 1 );
    // Подрезка явная: значение свыше предела переписалось бы в совершенно
    // другой такт, о котором никто не узнает.
    QCOMPARE( int( mcFromTickPs( 1.0e9 ) ), int( MC_MAX ) );

    QCOMPARE( comparatorDelayPs( 0x0064 ), 1000.0 );
}


/// Период выборки: `T = TS × (MC × 10 пс) / 256`. Скорость выборки —
/// обратная ему величина, и единица у неё Sps, а не Hz.
void TestOscill::testSamplePeriodAndRate() {
    QCOMPARE( samplePeriodPs( 256, 0x07D0 ), 20000.0 );
    QCOMPARE( sampleRateSps( 256, 0x07D0 ), 50.0e6 ); // 50 MSps

    QCOMPARE( samplePeriodPs( 0x2000, 0x07D0 ), 640000.0 );
    QCOMPARE( sampleRateSps( 0x2000, 0x07D0 ), 1562500.0 ); // 1,5625 MSps

    QCOMPARE( tsFromSamplePeriodPs( 640000.0, 0x07D0 ), uint32_t( 0x2000 ) );
    QCOMPARE( tsFromSamplePeriodPs( 20000.0, 0x07D0 ), uint32_t( 256 ) );
    QCOMPARE( tsFromSamplePeriodPs( 0.0, 0x07D0 ), uint32_t( 0 ) );
    QCOMPARE( tsFromSamplePeriodPs( 640000.0, 0 ), uint32_t( 0 ) );

    // Период не установлен. Ноль означает именно это, а не «ноль отсчётов
    // в секунду»: «бесконечно быстро» не бывает.
    QCOMPARE( sampleRateSps( 0, 0x07D0 ), 0.0 );
    QCOMPARE( sampleRateSps( 256, 0 ), 0.0 );

    // Выборок на деление — величина ПРЕДСТАВЛЕНИЯ: одна эталонная
    // реализация взяла 32, другая до 100. Умолчания у неё нет.
    QCOMPARE( timePerDivPs( 256, 0x07D0, 32 ), 640000.0 );
    QCOMPARE( timePerDivPs( 256, 0x07D0, 0 ), 0.0 );

    // Ось времени: момент синхронизации приходится на нуль, TC — центровка.
    QCOMPARE( sampleTimePs( 0, 20000.0, 100 ), -2.0e6 );
    QCOMPARE( sampleTimePs( 100, 20000.0, 100 ), 0.0 );
    QCOMPARE( sampleTimePs( 150, 20000.0, 100 ), 1.0e6 );
}


/// Единица `TD`/`TA`/`TW` — 12 машинных тактов. Формула выведена из
/// единицы измерения; ни одна эталонная реализация её не применяет, обе
/// пишут сырые числа. [СБОРКА]
void TestOscill::testDelayUnits() {
    QCOMPARE( delayPs( 1, 0x07D0 ), 240000.0 ); // 12 × 20 нс
    QCOMPARE( delayPs( 500, 0x07D0 ), 1.2e8 );
    QCOMPARE( delayPs( 0, 0x07D0 ), 0.0 );
    // Такт не установлен — задержки нет.
    QCOMPARE( delayPs( 500, 0 ), 0.0 );

    QCOMPARE( delayUnitsFromPs( 1.2e8, 0x07D0 ), uint32_t( 500 ) );
    QCOMPARE( delayUnitsFromPs( 240000.0, 0x07D0 ), uint32_t( 1 ) );
    QCOMPARE( delayUnitsFromPs( 0.0, 0x07D0 ), uint32_t( 0 ) );
    QCOMPARE( delayUnitsFromPs( 1.2e8, 0 ), uint32_t( 0 ) );
    QCOMPARE( int( DELAY_UNIT_TICKS ), 12 );
}


/// Значение `V1` равно числу милливольт на деление напрямую. Имена
/// свойств обманывают: `V1h = 10000` — это 10 В/дел, то есть НИЗШАЯ
/// чувствительность.
void TestOscill::testSensitivity() {
    QCOMPARE( milliVoltsPerDiv( 0x2710 ), 10000.0 );
    QCOMPARE( milliVoltsPerDiv( 0x0014 ), 20.0 );
    QCOMPARE( milliVoltsPerDiv( 200 ), 200.0 );

    QCOMPARE( int( v1FromMilliVoltsPerDiv( 10000.0 ) ), 0x2710 );
    QCOMPARE( int( v1FromMilliVoltsPerDiv( 20.0 ) ), 0x0014 );
    QCOMPARE( int( v1FromMilliVoltsPerDiv( 0.0 ) ), 0 );
    // Верхний предел — разрядность регистра, а не предел прибора: свой
    // предел прибор объявляет свойством V1h, и сверять обязан вызывающий.
    QCOMPARE( int( v1FromMilliVoltsPerDiv( 1.0e9 ) ), 0xFFFF );
}


/// Разворот знакового шестнадцатибитного значения. `P1h = 0x0180` = +384,
/// `P1l = 0xFE80` = −384.
void TestOscill::testSignedOffset() {
    QCOMPARE( int( signed16( 0x0180 ) ), 384 );
    QCOMPARE( int( signed16( 0xFE80 ) ), -384 );
    QCOMPARE( int( signed16( 0x0000 ) ), 0 );
    QCOMPARE( int( signed16( 0x7FFF ) ), 32767 );
    QCOMPARE( int( signed16( 0x8000 ) ), -32768 );
    QCOMPARE( int( signed16( 0xFFFF ) ), -1 );
    // Старшие биты отрезаются: знаковое расширение до 32 бит на проводе
    // не живёт, а сюда приходит.
    QCOMPARE( int( signed16( 0xFFFFFE80u ) ), -384 );
}


/// Рамка пересчёта кода в напряжение НЕ ВЫБИРАЕТСЯ молча. Первоисточник
/// даёт единицу `V1` двумя несводимыми числами (8 мВ и 8,53 мВ на диапазон
/// АЦП); спор закрывается одним измерением на приборе, а до него пересчёт
/// обязан отказать, а не построить молча кривой график.
void TestOscill::testOffsetNeedsCodeSpan() {
    double mv = -12345.0;
    QVERIFY( !offsetMillivolts( 384, 1000, CodeSpan::Unverified, mv ) );
    QCOMPARE( mv, -12345.0 ); // числа на выходе нет вовсе

    QVERIFY( offsetMillivolts( 384, 1000, CodeSpan::Codes256, mv ) );
    QCOMPARE( mv, 12000.0 ); // шаг кода 1000×8/256 = 31,25 мВ
    QVERIFY( offsetMillivolts( -384, 1000, CodeSpan::Codes256, mv ) );
    QCOMPARE( mv, -12000.0 );
    QVERIFY( offsetMillivolts( 0, 1000, CodeSpan::Codes256, mv ) );
    QCOMPARE( mv, 0.0 );

    // Рамка не украшение: те же коды при 240 кодах на экран дают другое
    // напряжение, ровно в 256/240 раза большее. Из-за этого отношения у
    // первоисточника и появилась вторая цифра 8,53.
    QVERIFY( offsetMillivolts( 384, 1000, CodeSpan::Codes240, mv ) );
    QVERIFY( near( mv, 12000.0 * 256.0 / 240.0, 1e-9 ) );

    // Вырожденная шкала: величины нет.
    QVERIFY( !offsetMillivolts( 384, 0, CodeSpan::Codes256, mv ) );
    QVERIFY( !offsetMillivolts( 384, 1000, CodeSpan::Codes256, mv, 0 ) );

    int16_t p1 = 0;
    QVERIFY( p1FromOffsetMillivolts( 12000.0, 1000, CodeSpan::Codes256, p1 ) );
    QCOMPARE( int( p1 ), 384 );
    QVERIFY( p1FromOffsetMillivolts( -12000.0, 1000, CodeSpan::Codes256, p1 ) );
    QCOMPARE( int( p1 ), -384 );
    QVERIFY( !p1FromOffsetMillivolts( 12000.0, 1000, CodeSpan::Unverified, p1 ) );
    // Насыщение по разрядности регистра, а не по границам прибора:
    // границы — свойства P1h/P1l, и сверять с ними обязан вызывающий.
    QVERIFY( p1FromOffsetMillivolts( 1.0e9, 1000, CodeSpan::Codes256, p1 ) );
    QCOMPARE( int( p1 ), 32767 );
}


/// Обратный пересчёт уровня синхронизации ОБЯЗАН вычитать нижнюю границу
/// окна. Эталонная реализация этого не делает, и её круговой пересчёт не
/// сходится: 0 мВ даёт `S1 = 0` вместо 128, то есть порог уезжает на край
/// экрана вместо центра.
void TestOscill::testTriggerLevel() {
    QCOMPARE( triggerLevelMv( 128, 1000, 8 ), 0.0 );
    QCOMPARE( triggerLevelMv( 0, 1000, 8 ), -4000.0 );
    QCOMPARE( triggerLevelMv( 255, 1000, 8 ), 3968.75 );
    QCOMPARE( triggerLevelMv( 128, 1000, 0 ), 0.0 ); // делений нет — шкалы нет

    QCOMPARE( int( s1FromTriggerLevelMv( 0.0, 1000, 8 ) ), 128 );
    QCOMPARE( int( s1FromTriggerLevelMv( -4000.0, 1000, 8 ) ), 0 );
    QCOMPARE( int( s1FromTriggerLevelMv( 4000.0, 1000, 8 ) ), 255 );
    QCOMPARE( int( s1FromTriggerLevelMv( -1.0e9, 1000, 8 ) ), 0 );
    QCOMPARE( int( s1FromTriggerLevelMv( 1.0e9, 1000, 8 ) ), 255 );
    // Шкалы нет, а отказать нечем: подпись возвращает голый байт. Центр —
    // единственное защитимое значение, он не смещает порог никуда.
    QCOMPARE( int( s1FromTriggerLevelMv( 0.0, 0, 8 ) ), 128 );

    // Круговой пересчёт обязан сходиться на всей шкале.
    for ( int s1 : { 0, 1, 64, 100, 128, 200, 254, 255 } )
        QCOMPARE( int( s1FromTriggerLevelMv( triggerLevelMv( uint8_t( s1 ), 1000, 8 ), 1000, 8 ) ), s1 );
}


/// Шкала канала: границы окна АЦП и цена кода. Формат нужен потому, что
/// повышенное разрешение кладёт на тот же размах 65536 кодов вместо 256.
void TestOscill::testChannelScaleAndCodes() {
    // Рамка не установлена — шкалы нет, и величина числом не показывается.
    const ChannelScale none = channelScale( 1000, 0, SampleFormat::Normal, CodeSpan::Unverified );
    QVERIFY( !none.known );
    double mv = 1.0;
    QVERIFY( !codeToMillivolts( 128, none, mv ) );
    QCOMPARE( mv, 1.0 );

    const ChannelScale s = channelScale( 1000, 0, SampleFormat::Normal, CodeSpan::Codes256 );
    QVERIFY( s.known );
    QCOMPARE( s.minMv, -4000.0 );
    QCOMPARE( s.maxMv, 4000.0 );
    QCOMPARE( s.stepMv, 31.25 );
    QVERIFY( codeToMillivolts( 128, s, mv ) );
    QCOMPARE( mv, 0.0 );
    QVERIFY( codeToMillivolts( 0, s, mv ) );
    QCOMPARE( mv, -4000.0 );
    QVERIFY( codeToMillivolts( 255, s, mv ) );
    QCOMPARE( mv, 3968.75 );

    // Повышенное разрешение: тот же размах, шаг в 256 раз мельче.
    const ChannelScale hi = channelScale( 1000, 0, SampleFormat::AvgHiRes, CodeSpan::Codes256 );
    QVERIFY( hi.known );
    QCOMPARE( hi.minMv, -4000.0 );
    QCOMPARE( hi.stepMv, 8000.0 / 65536.0 );
    QVERIFY( codeToMillivolts( 32768, hi, mv ) );
    QCOMPARE( mv, 0.0 );

    // Окно движется ВМЕСТЕ со смещением: и низ, и верх получают одну и ту
    // же добавку. [СБОРКА] — знак проверяется тем же измерением, что и рамка.
    const ChannelScale moved = channelScale( 1000, 384, SampleFormat::Normal, CodeSpan::Codes256 );
    QCOMPARE( moved.minMv, 8000.0 );
    QCOMPARE( moved.maxMv, 16000.0 );

    QCOMPARE( codeFullScale( SampleFormat::AvgHiRes ), 0xFFFF );
    QCOMPARE( codeFullScale( SampleFormat::Normal ), 0xFF );
    QCOMPARE( codeFullScale( SampleFormat::PeakPaired ), 0xFF );
}


/// Защитный интервал — вычисленная величина, а не срок «на всякий
/// случай»: ожидание синхронизации плюс задержка развёртки плюс сама
/// оцифровка. При бесконечно ждущем запуске величины НЕТ.
void TestOscill::testCaptureWindow() {
    CaptureTiming t;
    t.mc = 0x07D0; // такт 20 нс
    t.ts = 256;    // период выборки 20 нс
    t.qs = 1000;
    t.rt = TriggerStart::Auto;
    t.ta = 500;
    t.tw = 1000;
    t.td = 0;

    // 500 × 12 × 20 нс = 120 мкс ожидания плюс 1000 × 20 нс = 20 мкс оцифровки.
    QCOMPARE( captureWindowPs( t ), 1.4e8 );

    // Ждущий запуск считает по TW, а не по TA.
    t.rt = TriggerStart::WaitTimeout;
    QCOMPARE( captureWindowPs( t ), 2.6e8 );

    // Свободный запуск не ждёт вовсе: остаётся только оцифровка.
    t.rt = TriggerStart::Free;
    QCOMPARE( captureWindowPs( t ), 2.0e7 );

    // Задержка развёртки отсчитывается ПОСЛЕ синхронизации и потому
    // складывается, а не выбирается.
    t.rt = TriggerStart::Auto;
    t.td = 10;
    QCOMPARE( captureWindowPs( t ), 1.4e8 + 2.4e6 );

    // Бесконечность — не «очень много», а утверждение «величины нет».
    t.rt = TriggerStart::WaitForever;
    QVERIFY( std::isinf( captureWindowPs( t ) ) );

    // Такт не установлен — интервал не вычислим. Ноль означает именно это.
    t.rt = TriggerStart::Auto;
    t.mc = 0;
    QCOMPARE( captureWindowPs( t ), 0.0 );
}


/// Карта быстрых вариантов однократной дискретизации: номер бита РАВЕН
/// числу тактов на выборку, а младший бит означает ПОЛТАКТА. Целых
/// «полтактов» не бывает, поэтому 0 и 1 обязаны различаться.
void TestOscill::testFastRealtimeTicks() {
    const std::vector< uint8_t > got = fastRealtimeTicks( 0x00000104 );
    QCOMPARE( int( got.size() ), 2 );
    QCOMPARE( int( got[ 0 ] ), 2 );
    QCOMPARE( int( got[ 1 ] ), 8 );

    QVERIFY( fastRealtimeTicks( 0 ).empty() );

    const std::vector< uint8_t > half = fastRealtimeTicks( 0x00000001 );
    QCOMPARE( int( half.size() ), 1 );
    QCOMPARE( int( half[ 0 ] ), 0 ); // ноль здесь — полтакта

    // Бит 31 первоисточником не определён («-») и в разбор не входит.
    QVERIFY( fastRealtimeTicks( 0x80000000u ).empty() );
    QCOMPARE( int( fastRealtimeTicks( 0x40000000u ).size() ), 1 );
    QCOMPARE( int( fastRealtimeTicks( 0x40000000u )[ 0 ] ), 30 );
}

// ===========================================================================
// Побитовые раскладки
// ===========================================================================

/// Раскладки читаются по смыслу БИТА, а не по смыслу пожелания.
void TestOscill::testBitLayouts() {
    // RS — ТРИ НЕЗАВИСИМЫХ ПЕРЕКЛЮЧАТЕЛЯ, а не одно состояние из списка.
    // Так их и ставит рабочий клиент вендора (ProcessingTypeMode.java):
    //   бит 0 ProcessingType  REALTIME / RIS
    //   бит 1 DataOutputType  POST_PROCESSING / REALTIME (параллельная)
    //   бит 2 BufferType      SYNC / ROLL
    // Таблица «режимов» в описании протокола — перечень полезных СОЧЕТАНИЙ,
    // а не определение битов; прежняя редакция читала её как определение и
    // объявляла в первоисточнике противоречие, которого там нет.
    //
    // Эти две проверки РАЗЛИЧАЮЩИЕ: код, требовавший все три бита разом,
    // на них падает. Прежние (0x07 и 0x03) истинны при обоих прочтениях и
    // потому дефекта не ловили — он и прожил до сверки.
    QVERIFY( decodeRs( 0x04 ).isRoll() );          // один бит 2 — уже лента
    QVERIFY( decodeRs( 0x06 ).isRoll() );          // параллельная + лента: это мы и пишем
    QVERIFY( !decodeRs( 0x06 ).isRis() );          // стробоскопа в ленте нет

    AcquisitionMode roll;
    roll.ris = false;
    roll.parallel = true;
    roll.roll = true;
    QCOMPARE( int( encode( roll ) ), 0x06 );

    AcquisitionMode allThree;
    allThree.ris = allThree.parallel = allThree.roll = true;
    QCOMPARE( int( encode( allThree ) ), 0x07 );
    QVERIFY( decodeRs( 0x07 ).isRoll() );
    QVERIFY( decodeRs( 0x0F ).isRoll() );
    QVERIFY( !decodeRs( 0x03 ).isRoll() );         // строка таблицы вендора: бит 2 нулевой
    QVERIFY( !decodeRs( 0x00 ).isRoll() );
    QVERIFY( decodeRs( 0x01 ).ris );
    QVERIFY( decodeRs( 0x02 ).parallel );
    QVERIFY( decodeRs( 0x04 ).roll );
    // Биты 7-4 смысла не имеют и в раскладку не попадают.
    QVERIFY( !decodeRs( 0xF0 ).ris );
    QVERIFY( !decodeRs( 0xF0 ).parallel );
    QVERIFY( !decodeRs( 0xF0 ).roll );

    // RT — пара младших бит.
    QCOMPARE( int( encode( TriggerStart::Auto ) ), 0 );
    QCOMPARE( int( encode( TriggerStart::WaitTimeout ) ), 1 );
    QCOMPARE( int( encode( TriggerStart::Free ) ), 2 );
    QCOMPARE( int( encode( TriggerStart::WaitForever ) ), 3 );
    QCOMPARE( decodeRt( 0xFD ), TriggerStart::WaitTimeout ); // старшие биты не участвуют
    QCOMPARE( decodeRt( 0x00 ), TriggerStart::Auto );

    // O1. Бит 0 означает «вход ЗАЗЕМЛЁН», а не «канал включён»: эталонная
    // реализация пишет его инверсией (верно), а читает как есть — и
    // сообщает «канал включён» ровно тогда, когда он заземлён.
    ChannelHwMode gnd;
    gnd.grounded = true;
    QCOMPARE( int( encode( gnd ) ), 0x01 );
    QVERIFY( decodeO1( 0x01 ).grounded );
    QVERIFY( !decodeO1( 0x00 ).grounded );
    ChannelHwMode all;
    all.grounded = all.acCoupling = all.filter3MHz = all.filter3kHz = true;
    QCOMPARE( int( encode( all ) ), 0x0F );
    QVERIFY( decodeO1( 0x02 ).acCoupling );
    QVERIFY( decodeO1( 0x04 ).filter3MHz );
    QVERIFY( decodeO1( 0x08 ).filter3kHz );

    // T1. Гистерезисы и полоса заданы ПАРАМИ бит: одиночный бит пары —
    // не «наполовину включено», и кодирование пишет обе.
    ChannelSyncMode rise;
    rise.onRise = true;
    rise.riseHysteresis = true;
    QCOMPARE( int( encode( rise ) ), int( T1_RISE_WITH_HYSTERESIS ) );
    QCOMPARE( int( T1_RISE_WITH_HYSTERESIS ), 0x2C );
    ChannelSyncMode fall;
    fall.onFall = true;
    fall.fallHysteresis = true;
    QCOMPARE( int( encode( fall ) ), 0x13 );
    ChannelSyncMode lf;
    lf.lowFrequencyOnly = true;
    QCOMPARE( int( encode( lf ) ), 0xC0 );

    const ChannelSyncMode back = decodeT1( T1_RISE_WITH_HYSTERESIS );
    QVERIFY( back.onRise );
    QVERIFY( back.riseHysteresis );
    QVERIFY( !back.onFall );
    QVERIFY( !back.fallHysteresis );
    QVERIFY( !back.lowFrequencyOnly );
    QVERIFY( decodeT1( 0x40 ).lowFrequencyOnly ); // читается младший бит пары
}


/// Маска трёх младших бит ОБЯЗАТЕЛЬНА. Без неё любой установленный
/// резервный бит уводит распознавание в «обычный формат» молча — именно
/// это делает эталонная реализация, сравнивая все восемь бит разом.
void TestOscill::testSampleFormatSizes() {
    SampleFormat f = SampleFormat::PeakPaired;
    QVERIFY( decodeSampleFormat( 0x04, f ) );
    QCOMPARE( f, SampleFormat::Normal );
    QVERIFY( decodeSampleFormat( 0xFC, f ) ); // резервные биты не мешают
    QCOMPARE( f, SampleFormat::Normal );
    QVERIFY( decodeSampleFormat( 0xF8, f ) );
    QCOMPARE( f, SampleFormat::Avg );
    QVERIFY( decodeSampleFormat( 0x01, f ) );
    QCOMPARE( f, SampleFormat::AvgHiRes );
    QVERIFY( decodeSampleFormat( 0x02, f ) );
    QCOMPARE( f, SampleFormat::PeakInterlaced );
    QVERIFY( decodeSampleFormat( 0x03, f ) );
    QCOMPARE( f, SampleFormat::PeakPaired );

    // Комбинации 101, 110, 111 не объявлены ни первоисточником, ни
    // эталонным кодом: `out` не трогается, подставить «наверное обычный»
    // значило бы разобрать чужой массив как свой.
    f = SampleFormat::AvgHiRes;
    QVERIFY( !decodeSampleFormat( 0x05, f ) );
    QCOMPARE( f, SampleFormat::AvgHiRes );
    QVERIFY( !decodeSampleFormat( 0x06, f ) );
    QVERIFY( !decodeSampleFormat( 0x07, f ) );

    // Байт массива на выборку. У поочерёдного пикового он ОДИН: пара
    // «2 байта / 2 выборки» означает, что минимум и максимум занимают
    // свои соседние позиции времени, а не одну общую.
    QCOMPARE( sampleBytes( SampleFormat::Avg ), 1 );
    QCOMPARE( sampleBytes( SampleFormat::AvgHiRes ), 2 );
    QCOMPARE( sampleBytes( SampleFormat::PeakInterlaced ), 1 );
    QCOMPARE( sampleBytes( SampleFormat::PeakPaired ), 2 );
    QCOMPARE( sampleBytes( SampleFormat::Normal ), 1 );

    // Ширина одного ЧИТАЕМОГО целого — другая величина: у повыборочного
    // пикового она единица, хотя байт на выборку два.
    QCOMPARE( sampleWordBytes( SampleFormat::AvgHiRes ), 2 );
    QCOMPARE( sampleWordBytes( SampleFormat::PeakPaired ), 1 );
    QCOMPARE( sampleWordBytes( SampleFormat::Normal ), 1 );

    QCOMPARE( samplesInBytes( SampleFormat::Normal, 10 ), 10 );
    QCOMPARE( samplesInBytes( SampleFormat::AvgHiRes, 10 ), 5 );
    // Деление нацело: недописанная выборка вверх НЕ округляется.
    QCOMPARE( samplesInBytes( SampleFormat::AvgHiRes, 11 ), 5 );
    QCOMPARE( samplesInBytes( SampleFormat::PeakPaired, 5 ), 2 );
    QCOMPARE( samplesInBytes( SampleFormat::Normal, 0 ), 0 );
    QCOMPARE( samplesInBytes( SampleFormat::Normal, -4 ), 0 );

    QCOMPARE( valueHeaderId( 1 ), HeaderId::Value1 );
    QCOMPARE( valueHeaderId( 2 ), HeaderId::Value2 );
    QCOMPARE( valueHeaderId( 4 ), HeaderId::Value4 );
}


/// Порядок записи и граф зависимостей. Связи `RS → …` и `TS → …` в
/// эталонном коде не заведены вовсе, хотя документация их требует.
void TestOscill::testInitOrderAndDependents() {
    const std::vector< Register > &order = initOrder();
    QCOMPARE( int( order.size() ), 17 );

    // Ни один регистр не потерян и ни один не записан дважды.
    std::vector< int > seen( 17, 0 );
    for ( Register r : order )
        ++seen[ size_t( r ) ];
    for ( int n : seen )
        QCOMPARE( n, 1 );

    // QS/TS/TC — последними: записанные раньше, они были бы подрезаны
    // последующими записями.
    QCOMPARE( order[ 0 ], Register::MC );
    QCOMPARE( order[ 14 ], Register::QS );
    QCOMPARE( order[ 15 ], Register::TS );
    QCOMPARE( order[ 16 ], Register::TC );

    const std::vector< Register > afterTs = dependentRegisters( Register::TS );
    QVERIFY( std::find( afterTs.begin(), afterTs.end(), Register::RS ) != afterTs.end() );
    QVERIFY( std::find( afterTs.begin(), afterTs.end(), Register::M1 ) != afterTs.end() );
    QVERIFY( std::find( afterTs.begin(), afterTs.end(), Register::TD ) != afterTs.end() );

    const std::vector< Register > afterV1 = dependentRegisters( Register::V1 );
    QCOMPARE( int( afterV1.size() ), 2 );
    QCOMPARE( afterV1[ 0 ], Register::P1 );
    QCOMPARE( afterV1[ 1 ], Register::S1 );

    // От RT зависит, какой предел ДЕЙСТВУЕТ, но не их значения:
    // перечитывать нечего.
    QVERIFY( dependentRegisters( Register::RT ).empty() );
    QVERIFY( dependentRegisters( Register::S1 ).empty() );

    const std::vector< Property > propsV1 = dependentProperties( Register::V1 );
    QCOMPARE( int( propsV1.size() ), 2 );
    QCOMPARE( propsV1[ 0 ], Property::P1h );
    QCOMPARE( propsV1[ 1 ], Property::P1l );
    QCOMPARE( int( dependentProperties( Register::QS ).size() ), 2 );
    QVERIFY( dependentProperties( Register::MC ).empty() );

    // Имена и ширины — то, чем пакет собирается.
    QCOMPARE( QByteArray( registerName( Register::TS ) ), QByteArray( "TS" ) );
    QCOMPARE( int( registerDef( Register::TS ).width ), 4 );
    QCOMPARE( int( registerDef( Register::P1 ).width ), 2 );
    QVERIFY( registerDef( Register::P1 ).isSigned );
    QVERIFY( !registerDef( Register::S1 ).isSigned ); // уровень БЕЗзнаковый
    QCOMPARE( QByteArray( propertyName( Property::QSh ) ), QByteArray( "QSh" ) );
    QVERIFY( propertyDef( Property::VNM ).isAscii );
    QVERIFY( propertyDef( Property::P1h ).isSigned );
    QCOMPARE( int( allRegisters().size() ), 17 );
    QCOMPARE( int( allProperties().size() ), int( Property::D1m ) + 1 );
}

// ===========================================================================
// Кадр оцифровки
// ===========================================================================

/// Все пять форматов выборок: проверяется и ЧИСЛО выборок, и значения.
void TestOscill::testFrameAllFiveFormats() {
    // Обычный: байт массива = выборка АЦП.
    {
        Frame f;
        QVERIFY( parseFrame( hex( "20 00 04 00 00 04 10 20 30 40" ), FrameLayout::WithSizeField, f ) );
        QVERIFY( f.valid );
        QVERIFY( !f.empty );
        QCOMPARE( f.channels, 1u );
        QCOMPARE( f.trigger, TriggerSource::Rise );
        QVERIFY( f.synchronized() );
        QCOMPARE( int( f.channel.size() ), 1 );
        QVERIFY( f.channel[ 0 ].formatKnown );
        QCOMPARE( f.channel[ 0 ].format, SampleFormat::Normal );
        QCOMPARE( int( f.channel[ 0 ].sample.size() ), 4 );
        QCOMPARE( f.channel[ 0 ].sample, ( std::vector< int >{ 0x10, 0x20, 0x30, 0x40 } ) );
        QVERIFY( !f.channel[ 0 ].envelope );
        QVERIFY( f.channel[ 0 ].sampleMax.empty() );
    }
    // Усреднение с отбрасыванием младших разрядов: тот же байт на выборку.
    {
        Frame f;
        QVERIFY( parseFrame( hex( "10 00 00 00 00 03 fe ff 00" ), FrameLayout::WithSizeField, f ) );
        QCOMPARE( f.channel[ 0 ].format, SampleFormat::Avg );
        QCOMPARE( f.trigger, TriggerSource::Fall );
        QVERIFY( f.synchronized() );
        // Выборки БЕЗЗНАКОВЫЕ: 0xFE — это 254, а не −2.
        QCOMPARE( f.channel[ 0 ].sample, ( std::vector< int >{ 254, 255, 0 } ) );
    }
    // Повышенное разрешение: два байта на выборку, BIG-ENDIAN.
    {
        Frame f;
        QVERIFY( parseFrame( hex( "20 00 01 00 00 04 01 02 fe dc" ), FrameLayout::WithSizeField, f ) );
        QCOMPARE( f.channel[ 0 ].format, SampleFormat::AvgHiRes );
        QCOMPARE( int( f.channel[ 0 ].sample.size() ), 2 );
        QCOMPARE( f.channel[ 0 ].sample, ( std::vector< int >{ 0x0102, 0xFEDC } ) );
    }
    // Пиковый ПООЧЕРЁДНО: минимум и максимум занимают СВОИ соседние
    // позиции времени. Точек ровно столько, сколько байт: эталонная
    // реализация прочла это как «две точки в одну» и потеряла половину.
    {
        Frame f;
        QVERIFY( parseFrame( hex( "20 00 02 00 00 04 10 f0 20 e0" ), FrameLayout::WithSizeField, f ) );
        QCOMPARE( f.channel[ 0 ].format, SampleFormat::PeakInterlaced );
        QCOMPARE( int( f.channel[ 0 ].sample.size() ), 4 );
        QCOMPARE( f.channel[ 0 ].sample, ( std::vector< int >{ 0x10, 0xF0, 0x20, 0xE0 } ) );
        QVERIFY( !f.channel[ 0 ].envelope );
        QVERIFY( f.channel[ 0 ].sampleMax.empty() );
    }
    // Пиковый ПОВЫБОРОЧНО: пара байт на одну позицию времени, ПЕРВЫЙ —
    // минимум, ВТОРОЙ — максимум.
    {
        Frame f;
        QVERIFY( parseFrame( hex( "20 00 03 00 00 04 10 f0 20 e0" ), FrameLayout::WithSizeField, f ) );
        QCOMPARE( f.channel[ 0 ].format, SampleFormat::PeakPaired );
        QVERIFY( f.channel[ 0 ].envelope );
        QCOMPARE( int( f.channel[ 0 ].sample.size() ), 2 );
        QCOMPARE( f.channel[ 0 ].sample, ( std::vector< int >{ 0x10, 0x20 } ) );    // минимумы
        QCOMPARE( f.channel[ 0 ].sampleMax, ( std::vector< int >{ 0xF0, 0xE0 } ) ); // максимумы
    }

    // Тот же порядок проверяется и на распаковке напрямую.
    ChannelData ch;
    QVERIFY( decodeSamples( hex( "10 f0 20 e0" ), SampleFormat::PeakPaired, ch ) );
    QVERIFY( ch.envelope );
    QCOMPARE( ch.sample, ( std::vector< int >{ 0x10, 0x20 } ) );
    QCOMPARE( ch.sampleMax, ( std::vector< int >{ 0xF0, 0xE0 } ) );
    // Пустой массив согласуется с любым форматом: это «выборок нет», а не
    // «разобрать не смогли».
    QVERIFY( decodeSamples( QByteArray(), SampleFormat::AvgHiRes, ch ) );
    QVERIFY( ch.sample.empty() );
}


/// Кадр с числом каналов больше одного. Число каналов — биты 7-6 ПЕРВОГО
/// байта атрибутов; эталонная реализация берёт их сдвигом от
/// шестнадцатибитного слова, попадает в зарезервированный второй байт и
/// всегда получает один канал.
void TestOscill::testFrameTwoChannels() {
    const QByteArray body = hex( "60 00" )              // фронт, ДВА канала
                            + hex( "04 00 00 02 aa bb" ) // канал 1: обычный, 2 байта
                            + hex( "01 00 00 04 01 02 03 04" ); // канал 2: повышенное разрешение
    Frame f;
    QVERIFY( parseFrame( body, FrameLayout::WithSizeField, f ) );
    QVERIFY( f.valid );
    QCOMPARE( f.channels, 2u );
    QCOMPARE( int( f.channel.size() ), 2 );
    QCOMPARE( f.channel[ 0 ].format, SampleFormat::Normal );
    QCOMPARE( f.channel[ 0 ].sample, ( std::vector< int >{ 0xAA, 0xBB } ) );
    QCOMPARE( f.channel[ 1 ].format, SampleFormat::AvgHiRes );
    QCOMPARE( f.channel[ 1 ].sample, ( std::vector< int >{ 0x0102, 0x0304 } ) );

    // Четыре канала объявляются битами 7-6 = 11.
    Frame four;
    const QByteArray b4 = hex( "e0 00" ) + hex( "04 00 00 01 11" ) + hex( "04 00 00 01 22" ) +
                          hex( "04 00 00 01 33" ) + hex( "04 00 00 01 44" );
    QVERIFY( parseFrame( b4, FrameLayout::WithSizeField, four ) );
    QCOMPARE( four.channels, 4u );
    QCOMPARE( int( four.channel.size() ), 4 );
    QCOMPARE( four.channel[ 3 ].sample, ( std::vector< int >{ 0x44 } ) );

    // Раскладка БЕЗ поля размера описывает ровно один канал: границ между
    // каналами в ней нет, и резать наугад нельзя.
    Frame nosize;
    QVERIFY( !parseFrame( hex( "60 00 04 00 aa bb" ), FrameLayout::WithoutSizeField, nosize ) );
    QCOMPARE( nosize.channels, 2u ); // атрибуты разобраны: по ним видно, чей кадр
}


/// Заявленный размер больше пришедшего. Берётся то, что ЕСТЬ, — чтение за
/// концом тела было бы выходом за буфер, — а кадр объявляется
/// несогласованным.
void TestOscill::testFrameDeclaredSizeLargerThanBody() {
    // Раскладка задана ЯВНО: по опыту такое тело определилось бы как
    // раскладка без поля размера, и проверялось бы не то.
    Frame f;
    QVERIFY( !parseFrame( hex( "20 00 04 00 00 10 aa bb" ), FrameLayout::WithSizeField, f ) );
    QVERIFY( !f.valid );
    // Атрибуты разобраны и остались: по ним видно, чей это был кадр.
    QCOMPARE( f.channels, 1u );
    QCOMPARE( f.trigger, TriggerSource::Rise );
    QCOMPARE( int( f.channel.size() ), 1 );
    // Прочитано ровно то, что пришло, и ни байтом больше.
    QCOMPARE( f.channel[ 0 ].raw, hex( "aa bb" ) );
    QCOMPARE( f.channel[ 0 ].sample, ( std::vector< int >{ 0xAA, 0xBB } ) );

    // Хвост, не принадлежащий ни одному каналу: раскладка выбрана неверно,
    // и молча отбросить его нельзя — она определяется один раз на сессию.
    Frame tail;
    QVERIFY( !parseFrame( hex( "20 00 04 00 00 02 aa bb cc" ), FrameLayout::WithSizeField, tail ) );

    // Атрибуты канала объявлены, а их байтов нет: тело противоречит себе.
    Frame torn;
    QVERIFY( !parseFrame( hex( "20 00 04" ), FrameLayout::WithSizeField, torn ) );
    QVERIFY( !parseFrame( hex( "20" ), FrameLayout::WithSizeField, torn ) );
}


/// Массив нечётной длины в двухбайтовом формате: распаковывается всё
/// целое, лишний байт отбрасывается, кадр объявляется несогласованным.
/// Досчитать половину выборки нулём было бы выдумкой значения, которого
/// прибор не присылал.
void TestOscill::testFrameOddArrayInPeakMode() {
    ChannelData ch;
    QVERIFY( !decodeSamples( hex( "10 f0 20" ), SampleFormat::PeakPaired, ch ) );
    QCOMPARE( int( ch.sample.size() ), 1 );
    QCOMPARE( ch.sample, ( std::vector< int >{ 0x10 } ) );
    QCOMPARE( ch.sampleMax, ( std::vector< int >{ 0xF0 } ) );

    QVERIFY( !decodeSamples( hex( "01 02 03" ), SampleFormat::AvgHiRes, ch ) );
    QCOMPARE( ch.sample, ( std::vector< int >{ 0x0102 } ) );

    // Один байт в двухбайтовом формате — ни одной целой выборки.
    QVERIFY( !decodeSamples( hex( "01" ), SampleFormat::PeakPaired, ch ) );
    QVERIFY( ch.sample.empty() );

    // Тот же остаток через разбор кадра: кадр несогласован, но данные,
    // которые были целыми, остаются.
    Frame f;
    QVERIFY( !parseFrame( hex( "20 00 03 00 00 03 10 f0 20" ), FrameLayout::WithSizeField, f ) );
    QVERIFY( !f.valid );
    QCOMPARE( int( f.channel[ 0 ].sample.size() ), 1 );
    QVERIFY( f.channel[ 0 ].envelope );

    // Нечётная длина в ОДНОбайтовом формате остатка не даёт.
    QVERIFY( decodeSamples( hex( "10 f0 20" ), SampleFormat::PeakInterlaced, ch ) );
    QCOMPARE( int( ch.sample.size() ), 3 );
}


/// Спор «есть поле размера массива или нет» бумагой не закрывается:
/// конспект утверждает, что есть, эталонная реализация — что нет, и она
/// работала с живым прибором. Разрешается опытом, на первом же кадре.
void TestOscill::testFrameLayoutDetection() {
    // Разбор по всему телу сходится ровно на его конце — поле размера есть.
    QCOMPARE( detectLayout( hex( "20 00 04 00 00 04 10 20 30 40" ), 1 ), FrameLayout::WithSizeField );

    // Не сходится ни при какой длине массива — раскладка эталонной
    // реализации, и она возможна только при одном канале.
    QCOMPARE( detectLayout( hex( "20 00 04 00 ff ff 10 20 30 40 50 60" ), 1 ),
              FrameLayout::WithoutSizeField );
    QCOMPARE( detectLayout( hex( "20 00 04 00 ff ff 10 20 30 40 50 60" ), 2 ), FrameLayout::Unknown );

    // Ловушка совпадения: тело ровно в шесть байт при объявленном нулевом
    // размере сходится с ОБЕИМИ раскладками — одна видит пустой массив,
    // другая массив из двух байт, и различить их нечем. Опыт вопрос не
    // разрешил, и решает вызывающий.
    QCOMPARE( detectLayout( hex( "20 00 04 00 00 00" ), 1 ), FrameLayout::Unknown );

    // Каналов ноль — сравнивать нечего; короче пяти байт — массива нет ни
    // при одной раскладке.
    QCOMPARE( detectLayout( hex( "20 00 04 00 00 04 10 20 30 40" ), 0 ), FrameLayout::Unknown );
    QCOMPARE( detectLayout( hex( "20 00 04 00" ), 1 ), FrameLayout::Unknown );

    // Раскладка `Unknown` на входе заставляет разбор определить её самому
    // и записать в кадр.
    Frame f;
    QVERIFY( parseFrame( hex( "20 00 04 00 00 04 10 20 30 40" ), FrameLayout::Unknown, f ) );
    QCOMPARE( f.layout, FrameLayout::WithSizeField );

    Frame java;
    QVERIFY( parseFrame( hex( "20 00 04 00 ff ff 10 20 30 40 50 60" ), FrameLayout::Unknown, java ) );
    QCOMPARE( java.layout, FrameLayout::WithoutSizeField );
    QCOMPARE( int( java.channel[ 0 ].sample.size() ), 8 ); // всё после четырёх байт заголовка

    // Опыт не разрешил вопрос — разбирать нечего: вызывающий примет
    // решение и запишет его в журнал явно.
    Frame ambiguous;
    QVERIFY( !parseFrame( hex( "20 00 04 00 00 00" ), FrameLayout::Unknown, ambiguous ) );
    QCOMPARE( ambiguous.layout, FrameLayout::Unknown );
    QCOMPARE( ambiguous.channels, 1u ); // атрибуты всё равно разобраны
}


/// Два законных кадра без единой выборки: начало ленты (четыре байта
/// заголовка) и пустое тело `0x49` по истечении предела ожидания.
void TestOscill::testFrameRollHeadAndEmptyBody() {
    // Начало ленты: прибор передал заголовок и начал оцифровку, данные
    // пойдут следом пакетами Continue и атрибутов больше не повторят.
    Frame head;
    QVERIFY( parseFrame( hex( "27 00 04 00" ), FrameLayout::Unknown, head ) );
    QVERIFY( head.valid );
    QVERIFY( head.empty );
    QVERIFY( head.ris );
    QVERIFY( head.parallel );
    QVERIFY( head.roll );
    QCOMPARE( int( head.channel.size() ), 1 );
    QCOMPARE( head.channel[ 0 ].format, SampleFormat::Normal );

    // Четырёх байт не хватит на атрибуты второго канала.
    Frame two;
    QVERIFY( !parseFrame( hex( "67 00 04 00" ), FrameLayout::Unknown, two ) );

    // Пустое тело: данных нет, СВЯЗЬ ЦЕЛА, и это не ошибка разбора.
    Frame empty;
    QVERIFY( parseFrame( QByteArray(), FrameLayout::Unknown, empty ) );
    QVERIFY( empty.valid );
    QVERIFY( empty.empty );
    QVERIFY( empty.channel.empty() );

    // Один байт — это не кадр, а обрывок: значащий байт атрибутов есть, а
    // второго нет.
    Frame torn;
    QVERIFY( !parseFrame( hex( "20" ), FrameLayout::Unknown, torn ) );
}


/// Неопознанный формат — ТРЕТЬЕ состояние, отличное и от «данных нет», и
/// от «данные есть»: байты пришли, а выборок нет. Сложить его с
/// отсутствием сигнала значило бы объявить непонятый формат тишиной.
void TestOscill::testFrameUnknownFormatIsThirdState() {
    Frame f;
    QVERIFY( parseFrame( hex( "20 00 05 00 00 04 10 20 30 40" ), FrameLayout::WithSizeField, f ) );
    QVERIFY( f.valid );
    QVERIFY( !f.empty ); // байты ЕСТЬ
    QCOMPARE( int( f.channel.size() ), 1 );
    QVERIFY( !f.channel[ 0 ].formatKnown );
    QVERIFY( f.channel[ 0 ].sample.empty() ); // а выборок нет
    QCOMPARE( f.channel[ 0 ].raw, hex( "10 20 30 40" ) );
}


/// П2. Кадр с битами 5-4 = 00 разбирается УСПЕШНО и несёт признак
/// «синхронизации не было». Оцифровка СОСТОЯЛАСЬ по истечении `TA`,
/// данные годны и просто не привязаны к событию синхронизации. Такой кадр
/// показывают с пометкой, а не прячут. Тест обязан упасть, если кто-то
/// когда-нибудь сделает из этого отказ.
void TestOscill::testTimeoutTriggerIsValidFrame() {
    Frame f;
    QVERIFY2( parseFrame( hex( "00 00 04 00 00 04 10 20 30 40" ), FrameLayout::WithSizeField, f ),
              "запуск по таймауту — валидный кадр, а не отказ разбора" );
    QVERIFY( f.valid );
    QVERIFY( !f.empty );
    QCOMPARE( f.trigger, TriggerSource::Timeout );
    QVERIFY2( !f.synchronized(), "признак «синхронизации не было» обязан быть виден" );
    // Данные годны и разобраны полностью — в этом и суть П2.
    QCOMPARE( f.channel[ 0 ].sample, ( std::vector< int >{ 0x10, 0x20, 0x30, 0x40 } ) );

    // Прочие источники запуска.
    QVERIFY( parseFrame( hex( "10 00 04 00 00 01 aa" ), FrameLayout::WithSizeField, f ) );
    QCOMPARE( f.trigger, TriggerSource::Fall );
    QVERIFY( f.synchronized() );
    QVERIFY( parseFrame( hex( "20 00 04 00 00 01 aa" ), FrameLayout::WithSizeField, f ) );
    QCOMPARE( f.trigger, TriggerSource::Rise );
    QVERIFY( f.synchronized() );
    // Комбинация 11 первоисточником не определена — и синхронизацией не
    // считается: приписать ей смысл значило бы его выдумать.
    QVERIFY( parseFrame( hex( "30 00 04 00 00 01 aa" ), FrameLayout::WithSizeField, f ) );
    QCOMPARE( f.trigger, TriggerSource::Undefined );
    QVERIFY( !f.synchronized() );

    QCOMPARE( QByteArray( triggerSourceName( TriggerSource::Timeout ) ),
              QByteArray( "таймаут (не синхронизировано)" ) );
}

// ===========================================================================
// Бэкенд на поддельной оснастке
// ===========================================================================

/// `probe()` ничего в приборе не меняет: порт НЕ открывается. Открытие
/// чужого занятого COM-порта есть вмешательство в работу другого прибора.
void TestOscill::testProbeDoesNotOpenPort() {
    Bench bench;
    OscillBackend backend( bench.transport() );

    QVERIFY( backend.probe() );
    QVERIFY( backend.state().present );
    QVERIFY( backend.state().lastError.isEmpty() );
    QVERIFY2( !backend.transport()->isOpen(), "probe() обязан отвечать, не открывая порт" );
    QVERIFY( !backend.state().linked );

    // Порта в системе нет — причина названа, а не скрыта.
    SerialParams gone;
    gone.port = QStringLiteral( "ПОРТ-КОТОРОГО-НЕТ" );
    gone.baudRate = SerialSpeeds::WORK;
    OscillBackend missing( std::make_unique< SerialTransport >( gone ) );
    QVERIFY( !missing.probe() );
    QVERIFY( !missing.state().present );
    QVERIFY( missing.state().lastError.contains( QStringLiteral( "ПОРТ-КОТОРОГО-НЕТ" ) ) );

    // Порт не задан: догадываться о номере COM-порта нельзя — открытый
    // наугад чужой порт есть вмешательство, которого никто не просил.
    OscillBackend blank( std::make_unique< SerialTransport >( SerialParams() ) );
    QVERIFY( !blank.probe() );
    QVERIFY( !blank.state().lastError.isEmpty() );
}


/// Оснастка не опознана как последовательный порт — причина называется
/// вслух, а не подменяется догадкой. Сюда же попадает сборка без RTTI,
/// где опознать оснастку нечем вовсе.
void TestOscill::testProbeRejectsForeignTransport() {
    OscillBackend foreign( std::make_unique< PlainTransport >() );
    QVERIFY( !foreign.probe() );
    QVERIFY( !foreign.state().present );
    QVERIFY( !foreign.state().lastError.isEmpty() );

    OscillBackend nothing{ TransportPtr() };
    QVERIFY( !nothing.probe() );
    QVERIFY( nothing.state().lastError.contains( QStringLiteral( "оснастка" ) ) );
    // Тип прибора известен всегда: он не зависит от того, есть ли связь.
    QCOMPARE( nothing.type(), Type::Oscill );
    QCOMPARE( nothing.channelCount(), 1u );
}


/// Связь: сброс, CONNECT, паспорт, фактические значения регистров. Прибор
/// не знает `VHW` и `VSW` — и это НЕ отказ линии, а состояние прибора: по
/// нему клиент переходит ко второму списку версий.
void TestOscill::testLinkReadsPassport() {
    Bench bench;
    OscillBackend backend( bench.transport() );

    QVERIFY( backend.link() );
    QVERIFY( backend.state().linked );
    QVERIFY( backend.state().lastError.isEmpty() );
    QCOMPARE( backend.step(), OscillBackend::Step::Idle );

    // Приёмный буфер прибора назван им самим в ответе на CONNECT.
    QVERIFY( backend.deviceRxBuffer().known );
    QCOMPARE( int( backend.deviceRxBuffer().value ), 0x1000 );

    // Опознание.
    QCOMPARE( backend.state().model, QStringLiteral( "Oscill Osc1" ) );
    QCOMPARE( backend.passport().deviceId, QStringLiteral( "Osc1" ) );
    QCOMPARE( backend.passport().serial, QStringLiteral( "0042" ) );
    // Второй список версий опрошен именно потому, что первый отказал.
    QCOMPARE( backend.passport().hardware, QStringLiteral( "VHD 2.00, VHA 2.00" ) );
    QVERIFY( backend.passport().software.contains( QStringLiteral( "VSD 2.31" ) ) );
    QVERIFY( backend.state().firmware.contains( QStringLiteral( "аппаратура" ) ) );
    // Ровно два отказа `0xD1`: VHW и VSW. Связь при этом цела.
    QCOMPARE( backend.linkStats().notImplemented, 2u );

    // Числовые свойства с признаком «прочитано».
    QVERIFY( backend.passport().valid );
    QVERIFY( backend.passport().samplesMax.known );
    QCOMPARE( int( backend.passport().samplesMax.value ), 0x0800 );
    QCOMPARE( int( backend.passport().tickDefault.value ), 0x07D0 );
    QCOMPARE( int( backend.passport().tickMin.value ), 0x03E8 );
    QCOMPARE( int( backend.passport().comparatorDelay.value ), 100 );
    // Знаковые свойства развёрнуты один раз и там, где известно, что они
    // знаковые.
    QCOMPARE( int( backend.passport().offsetMax.value ), 384 );
    QCOMPARE( int( backend.passport().offsetMin.value ), -384 );

    // Границы чувствительности — ИНТЕРВАЛ, а не пара в порядке имён.
    // Имена обманывают: V1h = 10 В/дел, то есть БОЛЬШЕЕ число мВ/дел.
    // Эталонная реализация построила интервал по именам, получила
    // [10000; 20], и её отсекатель поднимает любую чувствительность до
    // 10 В/дел.
    uint16_t lo = 0, hi = 0;
    QVERIFY( backend.passport().sensitivityRange( lo, hi ) );
    QCOMPARE( int( lo ), 20 );
    QCOMPARE( int( hi ), 10000 );

    // Фактические значения регистров прочитаны у прибора.
    QCOMPARE( int( backend.settings().mc ), 0x07D0 );
    QCOMPARE( int( backend.settings().ts ), 0x2000 );
    QCOMPARE( int( backend.settings().qs ), 0x0200 );
    QCOMPARE( int( backend.settings().m1 ), 0x04 );
    QCOMPARE( int( backend.settings().s1 ), 0x80 );

    // Скорость выборки, Sps: обратная периоду TS при такте MC.
    QCOMPARE( backend.samplerate(), 1562500.0 );
}


/// После разрыва паспорт и значения регистров неизвестны: прибор мог быть
/// выключен, заменён или перенастроен другим клиентом. А вот последний
/// кадр не стирается — он уже отдан наверх, и гасить показанное вместе со
/// связью нельзя; его возраст виден по номеру, который не растёт.
void TestOscill::testUnlinkForgetsPassportKeepsFrame() {
    Bench bench;
    OscillBackend backend( bench.transport() );
    QVERIFY( backend.link() );
    backend.update();
    backend.update();
    QCOMPARE( backend.frameSerial(), 1ull );
    QVERIFY( backend.frame().valid );

    backend.unlink();
    QVERIFY( !backend.state().linked );
    QCOMPARE( backend.step(), OscillBackend::Step::Offline );
    QVERIFY( !backend.transport()->isOpen() );

    QVERIFY( !backend.passport().valid );
    QVERIFY( !backend.passport().samplesMax.known );
    QCOMPARE( int( backend.settings().mc ), 0 );
    QVERIFY( !backend.deviceRxBuffer().known );

    QVERIFY2( backend.frame().valid, "показанный кадр гасить вместе со связью нельзя" );
    QCOMPARE( backend.frameSerial(), 1ull );

    // Повторный разрыв безопасен: `setTransport()` зовёт его безусловно.
    backend.unlink();
    QVERIFY( !backend.state().linked );

    // И повторная связь начинается с чистого состояния.
    QVERIFY( backend.link() );
    QVERIFY( backend.passport().valid );
}


/// Отсутствие связи не роняет обход: `update()` без связи ничего не
/// делает и ничего не портит.
void TestOscill::testUpdateWithoutLinkIsSafe() {
    Bench bench;
    OscillBackend backend( bench.transport() );

    for ( int i = 0; i < 5; ++i )
        backend.update();

    QCOMPARE( backend.step(), OscillBackend::Step::Offline );
    QCOMPARE( backend.frameSerial(), 0ull );
    QVERIFY( !backend.frame().valid );
    QVERIFY( backend.state().lastError.isEmpty() ); // это не беда, а состояние
    QCOMPARE( backend.linkStats().framesReceived, 0u );

    // И вовсе без оснастки — тоже.
    OscillBackend nothing{ TransportPtr() };
    nothing.update();
    QCOMPARE( nothing.step(), OscillBackend::Step::Offline );
    QCOMPARE( nothing.frameSerial(), 0ull );
}


/// Один шаг `update()` — одно действие: отдать команду либо забрать
/// ответ. Кадр появляется на втором шаге, и раскладка определяется опытом
/// на первом же кадре.
void TestOscill::testFramesThroughBackend() {
    Bench bench;
    OscillBackend backend( bench.transport() );
    QVERIFY( backend.link() );
    QCOMPARE( backend.frameLayout(), FrameLayout::Unknown );

    backend.update();
    QCOMPARE( backend.step(), OscillBackend::Step::Requested );
    QCOMPARE( backend.frameSerial(), 0ull );

    backend.update();
    QCOMPARE( backend.step(), OscillBackend::Step::Idle );
    QCOMPARE( backend.frameSerial(), 1ull );
    QCOMPARE( backend.linkStats().framesReceived, 1u );
    QVERIFY( backend.frame().valid );
    QVERIFY( backend.frame().synchronized() );
    QCOMPARE( int( backend.frame().channel[ 0 ].sample.size() ), 4 );
    QCOMPARE( backend.frame().channel[ 0 ].sample, ( std::vector< int >{ 0x10, 0x20, 0x30, 0x40 } ) );
    QCOMPARE( backend.frameLayout(), FrameLayout::WithSizeField );
    QVERIFY( backend.state().lastError.isEmpty() );

    // Кадр повышенного разрешения: два байта на выборку, big-endian.
    bench.device.digitizeBody = hex( "20 00 01 00 00 04 01 02 fe dc" );
    backend.update();
    backend.update();
    QCOMPARE( backend.frameSerial(), 2ull );
    QCOMPARE( backend.frame().channel[ 0 ].sample, ( std::vector< int >{ 0x0102, 0xFEDC } ) );

    // Рамка кодов установлена по спецификации изготовителя: 256 кодов на
    // 8 делений. Прежде умолчанием был отказ считать (`Unverified`), потому
    // что описание протокола давало единицу V1 двумя несводимыми числами;
    // таблица аттенюатора с сайта решила спор — девять строк из девяти
    // сходятся с делением на 256 и ни одна на 240.
    QCOMPARE( backend.codeSpan(), CodeSpan::Codes256 );
    QVERIFY( backend.channelScale().known );

    // Отказ считать никуда не делся и остаётся доступен: он нужен, когда
    // прибор отвечает не так, как написано в его же документации.
    backend.setCodeSpan( CodeSpan::Unverified );
    QVERIFY( !backend.channelScale().known );

    backend.setCodeSpan( CodeSpan::Codes256 );
    const ChannelScale scale = backend.channelScale();
    QVERIFY( scale.known );
    // Формат берётся у ПОСЛЕДНЕГО кадра: повышенное разрешение кладёт на
    // тот же размах 65536 кодов вместо 256.
    QCOMPARE( scale.stepMv, double( backend.settings().v1 ) * 8.0 / 65536.0 );
}


/// П2 через бэкенд. Ни запуск по таймауту, ни истёкший предел ожидания НЕ
/// ставят `lastError` и не роняют связь: «нет сигнала на входе» не есть
/// «прибор отвалился». Тест обязан упасть, если из этого сделают отказ.
void TestOscill::testBackendTimeoutFrameIsNotAnError() {
    Bench bench;
    OscillBackend backend( bench.transport() );
    QVERIFY( backend.link() );

    // Запуск по таймауту: данные ЕСТЬ и они годны.
    bench.device.digitizeBody = hex( "00 00 04 00 00 04 10 20 30 40" );
    backend.update();
    backend.update();
    QCOMPARE( backend.frameSerial(), 1ull );
    QVERIFY( backend.frame().valid );
    QVERIFY( !backend.frame().empty );
    QVERIFY2( !backend.frame().synchronized(), "признак «синхронизации не было» обязан дойти наверх" );
    QCOMPARE( backend.frame().channel[ 0 ].sample, ( std::vector< int >{ 0x10, 0x20, 0x30, 0x40 } ) );
    QVERIFY2( backend.state().lastError.isEmpty(), "запуск по таймауту — не ошибка" );
    QVERIFY( backend.state().linked );
    QCOMPARE( backend.linkStats().framesUnsynchronized, 1u );
    QCOMPARE( backend.linkStats().framesEmpty, 0u );

    // Ждущий запуск не дождался: Success с ПУСТЫМ телом `0x49`. Кадра
    // нет, связь цела. Отличие от предыдущего случая проходит по ДЛИНЕ
    // тела, а не по битам 5-4.
    bench.device.digitizeBody = QByteArray();
    backend.update();
    backend.update();
    QCOMPARE( backend.frameSerial(), 2ull );
    QVERIFY( backend.frame().valid );
    QVERIFY( backend.frame().empty );
    QVERIFY2( backend.state().lastError.isEmpty(), "истёкший предел ожидания — не ошибка" );
    QVERIFY( backend.state().linked );
    QCOMPARE( backend.linkStats().framesEmpty, 1u );
    QCOMPARE( backend.linkStats().framesReceived, 2u );
}


/// Длинный ответ приходит частями `0x48`, закрывается `0x49` и склеивается
/// в один объект. В словаре по идентификатору повторные `0x48` затирали бы
/// друг друга, и кадр не собрался бы вовсе.
void TestOscill::testLongAnswerAssembled() {
    Bench bench;
    bench.device.chunk = 4; // тело в 10 байт уедет тремя порциями
    OscillBackend backend( bench.transport() );
    QVERIFY( backend.link() );

    backend.update();
    backend.update();

    QCOMPARE( backend.frameSerial(), 1ull );
    QVERIFY( backend.frame().valid );
    QCOMPARE( int( backend.frame().channel[ 0 ].sample.size() ), 4 );
    QCOMPARE( backend.frame().channel[ 0 ].sample, ( std::vector< int >{ 0x10, 0x20, 0x30, 0x40 } ) );
    QVERIFY( backend.state().lastError.isEmpty() );
    // Порции добираются в том же вызове: растягивать их на обходы значило
    // бы сделать объявленную частоту кадров враньём.
    QCOMPARE( backend.step(), OscillBackend::Step::Idle );

    // Продолжение можно запрашивать обеими формами: первоисточник её не
    // уточняет, эталонные реализации разошлись.
    backend.setContinueWithFinalBit( true );
    backend.update();
    backend.update();
    QCOMPARE( backend.frameSerial(), 2ull );
    QCOMPARE( int( backend.frame().channel[ 0 ].sample.size() ), 4 );
}


/// П3. Прибор корректирует — клиент перечитывает. Записанное значение
/// наружу НЕ возвращается: возвращается ФАКТ, а расхождение показывается,
/// а не прячется. Подрезанный предел, о котором не сказали, есть враньё об
/// измерении.
void TestOscill::testDeviceCorrectionReturnsFact() {
    Bench bench;
    OscillBackend backend( bench.transport() );
    QVERIFY( backend.link() );
    QCOMPARE( backend.registerWriteStyle(), WriteStyle::CombinedGet );

    // Чувствительность выше предела прибора: он подрежет её до V1h.
    uint32_t actual = 0;
    QVERIFY( backend.writeRegister( Register::V1, 50000, actual ) );
    QVERIFY2( actual != 50000u, "бэкенд обязан вернуть факт прибора, а не запрошенное" );
    QCOMPARE( actual, 10000u );
    QCOMPARE( int( backend.settings().v1 ), 10000 );
    QCOMPARE( int( bench.device.regValue( "V1" ) ), 10000 );

    QCOMPARE( int( backend.mismatches().size() ), 1 );
    QCOMPARE( backend.mismatches()[ 0 ].reg, Register::V1 );
    QCOMPARE( backend.mismatches()[ 0 ].wanted, 50000u );
    QCOMPARE( backend.mismatches()[ 0 ].actual, 10000u );

    // Размер массива больше предела QSh.
    QVERIFY( backend.writeRegister( Register::QS, 60000, actual ) );
    QCOMPARE( actual, 2048u );
    QCOMPARE( int( backend.settings().qs ), 2048 );
    QCOMPARE( int( backend.mismatches().size() ), 2 );

    // Знаковый регистр: подрезка по нижней границе P1l = −384, и в
    // настройках он лежит числом СО ЗНАКОМ, а не сырыми битами.
    QVERIFY( backend.writeRegister( Register::P1, uint32_t( uint16_t( -400 ) ), actual ) );
    QCOMPARE( actual, uint32_t( uint16_t( -384 ) ) );
    QCOMPARE( int( backend.settings().p1 ), -384 );

    // Знаковая подрезка — тоже расхождение, и она тоже названа.
    QCOMPARE( int( backend.mismatches().size() ), 3 );

    // Принятое дословно расхождения НЕ даёт: перечень показывает только
    // то, что прибор поправил.
    QVERIFY( backend.writeRegister( Register::TC, 0x0100, actual ) );
    QCOMPARE( actual, 0x0100u );
    QCOMPARE( int( backend.mismatches().size() ), 3 );

    QVERIFY( backend.state().lastError.isEmpty() );
}


/// Тот же П3 при записи пакетом PUT: факта в ответе нет, и клиент обязан
/// прочитать его отдельным запросом, а не записать себе пожелание.
void TestOscill::testDeviceCorrectionOnPutStyle() {
    Bench bench;
    OscillBackend backend( bench.transport() );
    QVERIFY( backend.link() );
    backend.setRegisterWriteStyle( WriteStyle::PutFinal );
    QCOMPARE( backend.registerWriteStyle(), WriteStyle::PutFinal );

    uint32_t actual = 0;
    QVERIFY( backend.writeRegister( Register::V1, 5, actual ) );
    QVERIFY2( actual != 5u, "прибор поднял чувствительность до своей границы — вернуть обязан её" );
    QCOMPARE( actual, 20u ); // V1l = 20 мВ/дел, наивысшая чувствительность
    QCOMPARE( int( backend.settings().v1 ), 20 );
    QCOMPARE( int( backend.mismatches().size() ), 1 );
    QCOMPARE( backend.mismatches()[ 0 ].wanted, 5u );
    QCOMPARE( backend.mismatches()[ 0 ].actual, 20u );
}


/// Настройки пишутся целиком в объявленном порядке, и КАЖДАЯ подрезка
/// оказывается в перечне расхождений. Отказом линии подрезка не является:
/// `applySettings()` возвращает истину, а показать расхождения обязан
/// вызывающий.
void TestOscill::testApplySettingsShowsEveryCorrection() {
    Bench bench;
    OscillBackend backend( bench.transport() );
    QVERIFY( backend.link() );

    OscillSettings wanted;
    wanted.mc = 0x07D0;
    wanted.ts = 0x2000;
    wanted.rs = 0x00;
    wanted.ap = 0;
    wanted.ar = 1;
    wanted.qs = 60000; // больше QSh = 2048
    wanted.tc = 0x0100;
    wanted.td = 0;
    wanted.rt = uint8_t( encode( TriggerStart::Auto ) );
    wanted.ta = 100000;
    wanted.tw = 100000;
    wanted.o1 = 0;
    wanted.v1 = 50000; // больше V1h = 10000
    wanted.p1 = 0;
    wanted.m1 = uint8_t( encode( SampleFormat::Normal ) );
    wanted.t1 = T1_RISE_WITH_HYSTERESIS;
    wanted.s1 = 128;

    QVERIFY2( backend.applySettings( wanted ), "подрезка прибором отказом линии не является" );

    // Ровно две подрезки, и обе названы.
    QCOMPARE( int( backend.mismatches().size() ), 2 );
    QVector< Register > corrected;
    for ( const RegisterMismatch &m : backend.mismatches() )
        corrected.append( m.reg );
    QVERIFY( corrected.contains( Register::QS ) );
    QVERIFY( corrected.contains( Register::V1 ) );

    // В настройках лежат ФАКТЫ, а не пожелания.
    QCOMPARE( int( backend.settings().qs ), 2048 );
    QCOMPARE( int( backend.settings().v1 ), 10000 );
    QCOMPARE( int( backend.settings().t1 ), int( T1_RISE_WITH_HYSTERESIS ) );
    QCOMPARE( int( backend.settings().s1 ), 128 );

    // Повторное применение перечень расхождений обнуляет и набирает
    // заново: он описывает ПОСЛЕДНЮЮ попытку, а не историю.
    QVERIFY( backend.applySettings( wanted ) );
    QCOMPARE( int( backend.mismatches().size() ), 2 );

    // Настройки без связи не применяются, и причина названа.
    backend.unlink();
    QVERIFY( !backend.applySettings( wanted ) );
    QVERIFY( backend.state().lastError.contains( QStringLiteral( "связи нет" ) ) );
}


/// П8. Испорченный ответ лечится перезапросом `0x92` — однократным. Байты
/// пришли, значит ответ БЫЛ: повторять сам запрос нельзя, повторяется
/// ответ.
void TestOscill::testCorruptedAnswerIsRepeatedOnce() {
    Bench bench;
    OscillBackend backend( bench.transport() );
    QVERIFY( backend.link() );

    const OscillBackend::LinkStats before = backend.linkStats();
    bench.device.corruptNext = true;

    uint32_t value = 0;
    QVERIFY2( backend.readRegister( Register::QS, value ), "перезапрос обязан вытащить ответ целым" );
    QCOMPARE( value, 0x0200u );
    QCOMPARE( backend.linkStats().badChecksum, before.badChecksum + 1 );
    QCOMPARE( backend.linkStats().repeatsSent, before.repeatsSent + 1 );
    QVERIFY( backend.state().linked );

    // Порча кадра лечится тем же средством, и кадр доходит.
    bench.device.corruptNext = true;
    backend.update();
    backend.update(); // перезапрос ушёл, ответа ещё нет
    backend.update();
    QCOMPARE( backend.frameSerial(), 1ull );
    QVERIFY( backend.frame().valid );
}


/// Молчание прибора — ОТСУТСТВИЕ ответа, а не ошибочный ответ: повторяется
/// сам запрос, и ровно один раз. Связь при этом не объявляется живой.
void TestOscill::testSilentDeviceFailsLinkWithReason() {
    Bench bench( QStringLiteral( "ПОРТ-МОЛЧУН" ) );
    bench.device.mute = true;
    OscillBackend backend( bench.transport() );

    // Порт в системе есть — значит `probe()` отвечает утвердительно:
    // он спрашивает про порт, а не про прибор.
    QVERIFY( backend.probe() );

    QVERIFY( !backend.link() );
    QVERIFY( !backend.state().linked );
    QVERIFY( backend.state().lastError.contains( QStringLiteral( "CONNECT" ) ) );
    // Молчание сосчитано: по счётчикам видно, что связь на грани, задолго
    // до её обрыва.
    QVERIFY( backend.linkStats().silentIntervals >= 2u );
    // Перезапрашивать ответ, которого не было, нельзя.
    QCOMPARE( backend.linkStats().repeatsSent, 0u );
    QVERIFY( !backend.transport()->isOpen() );
}


/// Темп считается из ТЕКУЩЕГО `RS`, а не берётся константой: в
/// бесконечной оцифровке прибор говорит сам, и опрашивать его нельзя.
void TestOscill::testFlowPaceFollowsAcquisitionMode() {
    Bench bench;
    OscillBackend backend( bench.transport() );
    QVERIFY( backend.link() );

    const Flow before = backend.flow();
    QCOMPARE( before.pace, Pace::OnRequest );
    // Байт на порцию: размер массива на байт выборки в текущем формате.
    // QS = 0x200, M1 = обычный формат — один байт на выборку.
    QCOMPARE( before.bytesPerPortion, std::size_t( 512 ) );
    QVERIFY2( before.decimatedAtSource, "усреднение и пиковый режим идут ВНУТРИ прибора" );
    QVERIFY( before.ratePerSecond > 0.0 );

    // Формат меняет размер порции вдвое, темп — нет.
    uint32_t actual = 0;
    QVERIFY( backend.writeRegister( Register::M1, encode( SampleFormat::AvgHiRes ), actual ) );
    QCOMPARE( backend.flow().bytesPerPortion, std::size_t( 1024 ) );
    QCOMPARE( backend.flow().pace, Pace::OnRequest );

    // Бесконечная оцифровка: параллельная передача плюс бесконечность.
    // Стробоскопический бит не ставится — он служит периодике высокой
    // частоты и к ленте отношения не имеет.
    AcquisitionMode roll;
    roll.ris = false;
    roll.parallel = true;
    roll.roll = true;
    QVERIFY( backend.writeRegister( Register::RS, encode( roll ), actual ) );
    QCOMPARE( int( actual ), 0x06 );
    QCOMPARE( backend.flow().pace, Pace::Stream );
    QVERIFY( backend.flow().ratePerSecond > 0.0 );

    // И обход такой прибор больше не трогает: команда 'D' запустила бы
    // ленту, которую неблокирующий шаг забирать не умеет.
    const unsigned long long serial = backend.frameSerial();
    backend.update();
    backend.update();
    QCOMPARE( backend.frameSerial(), serial );
    QVERIFY( backend.state().lastError.contains( QStringLiteral( "лента" ) ) );

    QVERIFY( !backend.rollRunning() );
}


/// Модель объявляется ОДНОЙ строкой и ЯВНО. Статическим инициализатором
/// нельзя: тест дерева приборов объявляет `Type::Oscill` своей поддельной
/// фабрикой и проверяет, что повторное объявление отклоняется, —
/// саморегистрация столкнулась бы с этой проверкой, и падать начал бы
/// тест, а не виноватый код.
void TestOscill::testRegistryDeclaration() {
    QVERIFY( declareOscill() );
    QVERIFY2( !declareOscill(), "повторное объявление типа обязано быть отклонено" );
    QCOMPARE( Registry::instance().name( Type::Oscill ), QStringLiteral( "Oscill" ) );

    // Оснастки слоту сегодня взять неоткуда: группы параметров «oscill»
    // (port, baud) в реестре нет. [ПЛАН] Догадаться о номере COM-порта
    // функция не вправе — открытый наугад чужой порт есть вмешательство.
    QVERIFY( makeOscillTransport( 0 ) == nullptr );

    // Прибор создаётся и без оснастки: отсутствие порта видно сразу и
    // названо, а не выясняется зависанием.
    std::unique_ptr< Backend > made = Registry::instance().create( Type::Oscill, 0 );
    QVERIFY( made != nullptr );
    QCOMPARE( made->type(), Type::Oscill );
    QVERIFY( !made->probe() );
    QVERIFY( !made->state().lastError.isEmpty() );
    QCOMPARE( made->bus(), Bus::None );
}

// Явный GUILESS, как у соседних тестов: `QTEST_MAIN` при `QT_GUI_LIB`
// поднимал бы `QGuiApplication` и требовал бы дисплея, которого на
// сборочной машине нет. Ни одного виджета здесь не создаётся.
QTEST_GUILESS_MAIN( TestOscill )
#include "test_oscill.moc"
