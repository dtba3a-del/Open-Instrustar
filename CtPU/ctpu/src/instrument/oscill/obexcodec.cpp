// SPDX-License-Identifier: GPL-3.0-or-later

/// \file obexcodec.cpp
/// \brief Реализация проводного формата OBEX прибора Oscill.
///
/// ЧТО РЕШЕНО ЗДЕСЬ, А НЕ В ЗАГОЛОВКЕ
///
/// Заголовок задаёт правило, а правило не отвечает на вопрос «что делать
/// с тем, чего правило не предусмотрело». Тех мест ровно четыре, и все
/// они решены в одну сторону — В СТОРОНУ ОТКАЗА, а не догадки:
///
///   1. **Пакет длиннее 65535 байт собрать нельзя** — поле длины
///      двухбайтовое. Сборщик в этом случае становится негодным, а не
///      обрезает длину по младшим битам: обрезанная длина есть другой
///      пакет, и прибор разберёт его как другой пакет.
///   2. **Тело не по классу идентификатора** (два байта у `0xB1`, три у
///      `0xF1`) не дополняется нулями и не режется. Дополнить значило бы
///      отправить прибору ДРУГОЕ ЧИСЛО, а он его примет и подтвердит.
///   3. **Шаг разбора заголовка ноль или за буфером** — `MalformedHeader`.
///      Это ровно то место, где эталонная реализация уходит в вечный
///      цикл: у неё ветка класса 00 не сдвигает индекс вовсе.
///   4. **Скорость, для которой нет целого коэффициента**, даёт ноль.
///      Подставить «ближайшее» молча нельзя: хост откроет порт на одной
///      скорости, прибор перейдёт на другую, и линия умрёт молча — а
///      молчание линии здесь неотличимо от молчания прибора.
///
/// АРИФМЕТИКА ПРИМЕРОВ ПЕРВОИСТОЧНИКА НЕ ВОСПРОИЗВОДИТСЯ НАМЕРЕННО.
/// Проверено пересчётом: из его примеров правилу удовлетворяет ровно
/// один — запрос CONNECT `80 00 09 10 00 10 00 B0 A7` (сумма 512 ≡ 0).
/// В прочих либо сумма не сходится (ответ CONNECT даёт 6, а не 0), либо
/// объявленная длина не равна фактической (запись регистра `TS`
/// объявляет `0x0E`, тогда как по правилу выходит `0x0F` = 3 + 5 + 5 + 2 —
/// та же структура, что у согласованного примера чтения регистра, где
/// напечатано именно `0x0F`). Кодек считает по правилу; тест, написанный
/// по числам из примеров, будет ошибочным тестом.
///
/// СОСТОЯНИЕ УТВЕРЖДЕНИЙ. Прибора нет: всё здесь — **[СБОРКА]**.
/// Проверяемость и есть причина существования этого файла: у него на
/// входе байты и на выходе байты, живого прибора он не требует.

#include "obexcodec.h"

#include <cstddef>

namespace Oscill {

namespace {

/// Байт буфера как беззнаковый. `QByteArray` хранит `char`, знаковость
/// которого зависит от платформы: на ARM `char` беззнаковый, на x86 —
/// знаковый, и `packet[ i ] > 0x7F` там означало бы разное. Все чтения
/// байт в этом файле идут через эту функцию, других нет.
inline uint8_t byteAt( const QByteArray &b, int i ) { return uint8_t( b.at( i ) ); }

/// Двухбайтовое поле big-endian. Границы проверяет вызывающий: здесь
/// проверка была бы второй, а решение «что делать при выходе за буфер» у
/// каждого вызывающего своё (`Incomplete` против `MalformedHeader`).
inline int be16( const QByteArray &b, int i ) { return ( int( byteAt( b, i ) ) << 8 ) | int( byteAt( b, i + 1 ) ); }

/// Четыре байта числа старшим вперёд. Порядок задан первоисточником для
/// всего класса `11xxxxxx`, а не только для `0xF1`.
QByteArray be32Bytes( uint32_t v ) {
    const char raw[ 4 ] = { char( ( v >> 24 ) & 0xFF ), char( ( v >> 16 ) & 0xFF ), char( ( v >> 8 ) & 0xFF ), char( v & 0xFF ) };
    return QByteArray( raw, 4 );
}

/// Наибольшая длина, представимая двухбайтовым полем. Ограничение
/// формата, а не выбор реализации.
constexpr int MAX_WIRE_LENGTH = 0xFFFF;

/// Проводной размер заголовка контрольной суммы: идентификатор плюс
/// значение. Класс `0xB0` — `10xxxxxx`, поля длины у него нет.
constexpr int CRC_WIRE_SIZE = 2;

/// Допуск на расхождение желаемой скорости с достижимой, заданный
/// ОБРАТНОЙ величиной: 50 означает 1/50 = 2 %.
///
/// Почему вообще допуск, а не точное деление: `1842000 / 16 = 115125`, а
/// не 115200 — то есть у объявленной первоисточником пары «коэффициент 16
/// ↔ 115200» точного деления нет ни при каком прочтении. Величина прибора
/// здесь — КОЭФФИЦИЕНТ, скорость же есть его следствие, и требовать от
/// следствия целочисленности бессмысленно.
///
/// Почему именно 2 %: кадр UART несёт 10 бит, приёмник ловит середину
/// бита, и накопленное за кадр расхождение частот обязано остаться меньше
/// половины бита — это 5 % на обе стороны линии вместе. Половина этого
/// запаса отдана прибору, половина хосту, отсюда 2 % на нашу сторону.
/// Пара 115200/115125 расходится на 0,065 % и проходит с запасом в
/// тридцать раз. [СБОРКА]
constexpr uint64_t SPEED_TOLERANCE_INV = 50;

/// Сколько полей пакета CONNECT стоит перед его заголовками: версия,
/// флаги и двухбайтовый приёмный буфер. Это единственная раскладка, у
/// которой область заголовков начинается не с третьего байта.
constexpr int CONNECT_FIXED = 4;

} // namespace

// ===========================================================================
// Класс заголовка и его проводной размер
// ===========================================================================

HeaderClass headerClass( uint8_t id ) {
    // Два старших бита — это ПРАВИЛО формата, а не таблица известных
    // идентификаторов. Поэтому здесь нет ни одного имени заголовка:
    // новый идентификатор кодируется и разбирается без правки кодека,
    // и ровно этим здесь закрыта вторая грабля эталонного сборщика —
    // там поле длины писалось по списку исключений из четырёх имён, и
    // любой пятый идентификатор класса 11 собрался бы битым.
    switch ( id & 0xC0 ) {
    case 0x00:
        return HeaderClass::Unicode;
    case 0x40:
        return HeaderClass::ByteSeq;
    case 0x80:
        return HeaderClass::Byte1;
    default:
        return HeaderClass::Byte4;
    }
}


int headerWireSize( uint8_t id, int bodySize ) {
    switch ( headerClass( id ) ) {
    case HeaderClass::Byte1:
        return 2; // идентификатор + значение; поля длины нет
    case HeaderClass::Byte4:
        return 5; // идентификатор + четыре байта; поля длины нет
    case HeaderClass::Unicode:
    case HeaderClass::ByteSeq:
    default:
        // Поле длины считает и сам идентификатор, и оба своих байта,
        // поэтому проводной размер и объявленная длина здесь — одно и то
        // же число. Отрицательный размер тела — не «минус два байта», а
        // пустое тело: у пакета нет способа занять меньше места, чем его
        // собственный заголовок.
        return BASE_PACKET_LENGTH + ( bodySize > 0 ? bodySize : 0 );
    }
}

// ===========================================================================
// Заголовок
// ===========================================================================

uint8_t Header::byteValue() const {
    // Ноль при неподходящем размере — это «значения нет», и вызывающий
    // обязан знать, что спрашивает: у класса `Byte1` тело ровно
    // однобайтовое по построению разбора, поэтому иной размер означает,
    // что заголовок вообще не того класса.
    return body.size() == 1 ? byteAt( body, 0 ) : uint8_t( 0 );
}


uint32_t Header::uint32Value() const {
    if ( body.size() != 4 )
        return 0;
    return ( uint32_t( byteAt( body, 0 ) ) << 24 ) | ( uint32_t( byteAt( body, 1 ) ) << 16 ) |
           ( uint32_t( byteAt( body, 2 ) ) << 8 ) | uint32_t( byteAt( body, 3 ) );
}


uint16_t Header::oscillWordValue() const {
    // Дословно первоисточник: «двухбайтовое значение Oscill-a...
    // Передаётся 4 байта, первые два: 00 00!». Значащие — младшие два, и
    // берутся они именно как младшие, а не как «те, что не нули»: если
    // старшие два пришли ненулевыми, это порча линии, и подставлять
    // вместо младших старшие означало бы вернуть число, которого прибор
    // не посылал. Порчу ловит контрольная сумма, а не эта функция.
    if ( body.size() == 4 )
        return uint16_t( ( int( byteAt( body, 2 ) ) << 8 ) | int( byteAt( body, 3 ) ) );
    // Двухбайтовое тело первоисточником не предусмотрено, но допущено
    // здесь: `0xF0` — единственный заголовок, у которого объявленная
    // ширина значения и проводная ширина поля расходятся, и разбор
    // записи, сделанной другой реализацией, не должен на этом падать.
    if ( body.size() == 2 )
        return uint16_t( ( int( byteAt( body, 0 ) ) << 8 ) | int( byteAt( body, 1 ) ) );
    return 0;
}


QByteArray Header::asciiValue() const {
    // Строка класса 00 завершается двумя нулями, ASCII-паспорт прибора
    // приходит дополненным нулями до четырёх байт. Обрезается хвост, а
    // не все нули: ноль ВНУТРИ значения — признак того, что это не текст,
    // и молчаливая склейка половинок скрыла бы это от вызывающего.
    int end = body.size();
    while ( end > 0 && byteAt( body, end - 1 ) == 0 )
        --end;
    return body.left( end );
}

// ===========================================================================
// Сборка пакета
// ===========================================================================

Builder &Builder::add( uint8_t id, const QByteArray &body ) {
    // Негодный сборщик остаётся негодным до конца: продолжать собирать
    // пакет, у которого одно поле уже неверно, — значит отправить его
    // прибору целым с виду.
    if ( !m_valid )
        return *this;

    const HeaderClass cls = headerClass( id );
    const int wire = headerWireSize( id, body.size() );

    // Проверка соответствия тела классу. Именно ОТКАЗ, а не дополнение
    // нулями: у заголовка значения `0xB1`/`0xF1` дополненное тело есть
    // другое число, прибор его примет, запишет в регистр и подтвердит —
    // ошибка станет наблюдаемой только как неверная осциллограмма.
    switch ( cls ) {
    case HeaderClass::Byte1:
        if ( body.size() != 1 ) {
            m_valid = false;
            return *this;
        }
        break;
    case HeaderClass::Byte4:
        if ( body.size() != 4 ) {
            m_valid = false;
            return *this;
        }
        break;
    case HeaderClass::Unicode:
    case HeaderClass::ByteSeq:
        // Тело произвольного размера, но объявленная длина обязана
        // влезть в два байта вместе с добавкой в три.
        if ( wire > MAX_WIRE_LENGTH ) {
            m_valid = false;
            return *this;
        }
        break;
    }

    // Место под контрольную сумму резервируется ЗАРАНЕЕ, хотя ставить её
    // или нет решает `build()`: иначе пакет, влезающий без суммы и не
    // влезающий с ней, оказался бы годным до самой отправки.
    if ( BASE_PACKET_LENGTH + m_headers.size() + wire + CRC_WIRE_SIZE > MAX_WIRE_LENGTH ) {
        m_valid = false;
        return *this;
    }

    m_headers.append( char( id ) );
    if ( cls == HeaderClass::Unicode || cls == HeaderClass::ByteSeq ) {
        m_headers.append( char( ( wire >> 8 ) & 0xFF ) );
        m_headers.append( char( wire & 0xFF ) );
    }
    m_headers.append( body );
    return *this;
}


Builder &Builder::addByte( HeaderId id, uint8_t value ) {
    // Класс проверяется ЗДЕСЬ, а не в `add()`, и вот почему: `add()`
    // судит тело по классу идентификатора, а однобайтовое тело у класса
    // `ByteSeq` совершенно законно. То есть `addByte( Register, x )`
    // прошёл бы через `add()` целым и дал бы на проводе `71 00 04 xx` —
    // байтовую последовательность из одного байта вместо однобайтового
    // значения. Пакет вышел бы формально верным и по смыслу другим, а
    // такую ошибку на живой линии не отличить от помехи.
    if ( headerClass( id ) != HeaderClass::Byte1 ) {
        m_valid = false;
        return *this;
    }
    return add( uint8_t( id ), QByteArray( 1, char( value ) ) );
}


Builder &Builder::addUint32( HeaderId id, uint32_t value ) {
    // То же самое и по той же причине: четырёхбайтовое тело законно и у
    // класса `ByteSeq`, поэтому проверка класса обязана стоять до `add()`.
    if ( headerClass( id ) != HeaderClass::Byte4 ) {
        m_valid = false;
        return *this;
    }
    return add( uint8_t( id ), be32Bytes( value ) );
}


Builder &Builder::addOscillWord( uint16_t value ) {
    // Старшие два байта нулевые — это не выравнивание и не запас на
    // будущее, а буквальное требование первоисточника, и обе эталонные
    // реализации его исполняют.
    return add( uint8_t( HeaderId::Value2 ), be32Bytes( uint32_t( value ) ) );
}


Builder &Builder::addName( HeaderId id, const QByteArray &ascii ) {
    // Длина имени (3 у свойства, 2 у регистра, 1 у команды) здесь не
    // проверяется намеренно: имена объявлены таблицей в
    // `oscillprotocol.h`, и вторая проверка здесь была бы вторым местом,
    // порождающим одно и то же знание.
    return add( uint8_t( id ), ascii );
}


Builder &Builder::addBody( const QByteArray &data, bool last ) {
    // Пустое тело законно и осмысленно: `49 00 03` — это штатный ответ
    // ждущего запуска, у которого истёк предел ожидания синхронизации.
    return add( uint8_t( last ? HeaderId::BodyEnd : HeaderId::BodyPart ), data );
}


Builder &Builder::addRaw( const QByteArray &data ) {
    if ( !m_valid )
        return *this;
    if ( BASE_PACKET_LENGTH + m_headers.size() + data.size() + CRC_WIRE_SIZE > MAX_WIRE_LENGTH ) {
        m_valid = false;
        return *this;
    }
    // Голое поле делает область непроходимой для `parseHeaders()`: разбор
    // примет первый его байт за идентификатор. Так и задумано — эту форму
    // имеет ровно один пакет (`0x91`), и разбирать его как набор
    // заголовков нельзя ни при каком прочтении.
    m_headers.append( data );
    return *this;
}


int Builder::size( bool withChecksum ) const {
    // У негодного сборщика размер ноль, а не «сколько получилось бы»:
    // `build()` вернёт пустой массив, и два ответа обязаны сходиться,
    // иначе вызывающий, спросивший только размер, решит, что есть что
    // отправлять.
    if ( !m_valid )
        return 0;
    return BASE_PACKET_LENGTH + m_headers.size() + ( withChecksum ? CRC_WIRE_SIZE : 0 );
}


bool Builder::fits( bool withChecksum, int limit ) const {
    const int n = size( withChecksum );
    return n > 0 && n <= limit;
}


QByteArray Builder::build( bool withChecksum ) const {
    if ( !m_valid )
        return {};

    const int total = size( withChecksum );
    if ( total > MAX_WIRE_LENGTH )
        return {};

    QByteArray packet;
    packet.reserve( total );
    packet.append( char( m_opcode ) );
    packet.append( char( ( total >> 8 ) & 0xFF ) );
    packet.append( char( total & 0xFF ) );
    packet.append( m_headers );

    if ( withChecksum ) {
        // Сумма ставится ЗДЕСЬ и только здесь. Добавить её заголовком
        // через `add()` нельзя не потому, что запрещено, а потому что
        // невозможно верно: её значение зависит от длины пакета, а длина
        // — от наличия самой суммы. Единственная точка, где известно и
        // то, и другое, — этот момент.
        packet.append( char( uint8_t( HeaderId::Crc ) ) );
        packet.append( char( checksumValue( packet ) ) );
    }
    return packet;
}


QByteArray makeBare( Op op ) {
    // Длина 3 — это сам код плюс два байта длины. Пустой пакет есть
    // законная форма, а не вырожденная: Abort и Disconnect иных полей не
    // имеют вовсе.
    QByteArray p;
    p.reserve( BASE_PACKET_LENGTH );
    p.append( char( uint8_t( op ) ) );
    p.append( char( 0x00 ) );
    p.append( char( BASE_PACKET_LENGTH ) );
    return p;
}


QByteArray makeConnect( uint16_t clientRxBuffer, bool withChecksum ) {
    Builder b( Op::Connect );
    QByteArray fixed;
    fixed.reserve( 4 );
    fixed.append( char( 0x10 ) ); // версия OBEX
    fixed.append( char( 0x00 ) ); // флаги
    fixed.append( char( ( clientRxBuffer >> 8 ) & 0xFF ) );
    fixed.append( char( clientRxBuffer & 0xFF ) );
    // Четыре байта идут ГОЛЫМИ, до всяких заголовков: это поля пакета
    // CONNECT, а не заголовки. Отсюда же и то, что разбор ответа на
    // CONNECT начинает читать заголовки с седьмого байта, а не с
    // четвёртого.
    b.addRaw( fixed );
    return b.build( withChecksum );
}


QByteArray makeSetSpeed( uint8_t divisor, bool withChecksum ) {
    // Коэффициент — голое поле, а не заголовок: первоисточник относит
    // опкод к пользовательским расширениям, и оформления заголовком у
    // него нет. Отсюда длина 4 без суммы и 6 с ней.
    Builder b( Op::SetSpeed );
    b.addRaw( QByteArray( 1, char( divisor ) ) );
    return b.build( withChecksum );
}


QByteArray makeRepeatLast( bool withChecksum ) { return Builder( Op::RepeatLast ).build( withChecksum ); }


uint8_t speedDivisor( uint32_t baud ) {
    if ( baud == 0 )
        return 0;

    // Ближайший целый коэффициент, посчитанный без плавающей точки:
    // округление вверх при остатке не меньше половины делителя.
    const uint64_t k = ( uint64_t( SPEED_BASE ) + uint64_t( baud ) / 2 ) / uint64_t( baud );
    if ( k < 1 || k > 255 )
        return 0;

    // Достижимая скорость есть 1842000/k, число вообще не целое. Поэтому
    // сравниваются не скорости, а произведения: |1842000 − k·baud| / (k·baud)
    // — это и есть относительное расхождение, и целочисленная форма
    // проверки избавляет от вопроса, что считать равенством у double.
    const uint64_t want = k * uint64_t( baud );
    const uint64_t diff = want > uint64_t( SPEED_BASE ) ? want - uint64_t( SPEED_BASE ) : uint64_t( SPEED_BASE ) - want;
    if ( diff * SPEED_TOLERANCE_INV > want )
        return 0;

    return uint8_t( k );
}


uint32_t baudFromDivisor( uint8_t divisor ) {
    if ( divisor == 0 )
        return 0; // деления на ноль здесь нет, и «бесконечной скорости» тоже
    // Округление, а не отбрасывание: 1842000/192 = 9593,75, и ближайшее
    // целое 9594 описывает линию точнее, чем 9593. Обратный пересчёт при
    // этом сходится: 9594 снова даёт коэффициент 192.
    return ( SPEED_BASE + uint32_t( divisor ) / 2 ) / uint32_t( divisor );
}

// ===========================================================================
// Контрольная сумма 0xB0
// ===========================================================================

uint8_t checksumValue( const QByteArray &packetUpToAndIncludingCrcId ) {
    unsigned sum = 0;
    for ( int i = 0; i < packetUpToAndIncludingCrcId.size(); ++i )
        sum += byteAt( packetUpToAndIncludingCrcId, i );
    // Дополнение суммы до нуля по модулю 256. Внешний `& 0xFF` нужен
    // ровно для одного случая: сумма, уже кратная 256, даёт 256, а не 0.
    return uint8_t( ( 256u - ( sum & 0xFFu ) ) & 0xFFu );
}


namespace {

/// Чем кончается область заголовков, начатая с `headersAt`.
enum class TailScan {
    NotWalkable, ///< область не разбирается: раскладка угадана неверно или пакет испорчен
    CrcLast,   ///< последний заголовок — сумма
    OtherLast, ///< область разобралась целиком, и суммы в конце нет
};

/// \brief Пройти область заголовков и посмотреть, что в ней последнее.
///
/// ПОЧЕМУ ПРОХОДОМ, А НЕ ВЗГЛЯДОМ НА ХВОСТ. Соблазн велик: заголовок
/// суммы всегда последний и всегда двухбайтовый, значит достаточно
/// посмотреть на предпоследний байт пакета. Так и было сделано в первой
/// редакции — и прогонка показала цену: байт `0xB0` попадает на это место
/// как ЧАСТЬ ДАННЫХ примерно в одном ответе из двухсот пятидесяти шести.
/// Такой ответ объявлялся бы битым, уходил бы в перезапрос `0x92`, а
/// прибор повторял бы тот же самый пакет — и кадр терялся бы навсегда,
/// выглядя при этом как помеха на линии, то есть как свойство сигнала.
///
/// Проход по структуре ответ ЗНАЕТ, а не угадывает: тело заголовка
/// перешагивается по объявленной длине целиком, поэтому данные внутри
/// него не могут быть приняты за идентификатор. Цена — разбор области
/// дважды, что при пакете в три десятка байт не стоит обсуждения.
TailScan scanTail( const QByteArray &packet, int headersAt ) {
    if ( headersAt < 0 || headersAt > packet.size() )
        return TailScan::NotWalkable;
    std::vector< Header > headers;
    if ( parseHeaders( packet.mid( headersAt ), headers ) != ParseError::None )
        return TailScan::NotWalkable;
    if ( headers.empty() )
        return TailScan::OtherLast; // заголовков нет вовсе — значит и суммы нет
    const Header &last = headers.back();
    return ( last.id == uint8_t( HeaderId::Crc ) && last.body.size() == 1 ) ? TailScan::CrcLast : TailScan::OtherLast;
}


/// Несёт ли пакет заголовок суммы последним. Раскладок области
/// заголовков ровно две (обычная с третьего байта и CONNECT-ответ с
/// седьмого), и ПОРЯДОК ПОПЫТОК задаёт тот, кто знает, какой пакет
/// держит в руках: первая же раскладка, по которой область разобралась
/// целиком, даёт точный ответ, и дальше идти незачем.
bool crcLast( const QByteArray &packet, int firstTry, int secondTry ) {
    // Короче пяти байт пакет с суммой не бывает: три своих плюс два её.
    if ( packet.size() < BASE_PACKET_LENGTH + CRC_WIRE_SIZE )
        return false;
    // Пакет обязан быть целым: у недочитанного конца ещё нет, а вопрос
    // «что стоит последним» без конца не имеет смысла.
    if ( declaredLength( packet ) != packet.size() )
        return false;

    for ( const int at : { firstTry, secondTry } ) {
        switch ( scanTail( packet, at ) ) {
        case TailScan::CrcLast:
            return true;
        case TailScan::OtherLast:
            return false;
        case TailScan::NotWalkable:
            break; // эта раскладка не подошла — пробуем следующую
        }
    }

    // Ни одна раскладка не разобралась. Остаётся единственное, на что
    // можно опереться, — байт на месте идентификатора суммы. Сюда
    // попадают только пакеты, у которых область заголовков не проходится
    // ни так, ни так: испорченные (их всё равно ждёт отказ разбора) и
    // расширение `0x91`, у которого после длины стоит голое поле, а не
    // заголовок. Для `0x91` это и есть верный ответ, и другого способа
    // его получить нет: голое поле структуры не имеет. [СБОРКА]
    return byteAt( packet, packet.size() - CRC_WIRE_SIZE ) == uint8_t( HeaderId::Crc );
}


/// Сумма всех байт пакета по модулю 256. Правило первоисточника требует
/// нуля — вместе с идентификатором `0xB0` и самим байтом суммы.
bool sumIsZero( const QByteArray &packet ) {
    unsigned sum = 0;
    for ( int i = 0; i < packet.size(); ++i )
        sum += byteAt( packet, i );
    return ( sum & 0xFFu ) == 0u;
}

} // namespace


bool hasChecksum( const QByteArray &packet ) {
    // Раскладка неизвестна, поэтому сначала пробуется обычная: ответов на
    // CONNECT за сессию ровно один, а прочих — тысячи.
    return crcLast( packet, BASE_PACKET_LENGTH, BASE_PACKET_LENGTH + CONNECT_FIXED );
}


bool checksumValid( const QByteArray &packet ) {
    // Пакет без суммы НЕ проходит проверку — и это не придирка: вопрос
    // «сошлась ли сумма» у такого пакета не имеет ответа, а «да» на
    // вопрос без ответа есть враньё. Различать «суммы нет» и «сумма не
    // сошлась» обязан вызывающий, и для этого есть `hasChecksum()`.
    return hasChecksum( packet ) && sumIsZero( packet );
}

// ===========================================================================
// Разбор
// ===========================================================================

const char *parseErrorName( ParseError e ) {
    switch ( e ) {
    case ParseError::None:
        return "разобрано";
    case ParseError::Incomplete:
        return "пакет не дочитан";
    case ParseError::LengthMismatch:
        return "длина не совпала с объявленной";
    case ParseError::BadChecksum:
        return "контрольная сумма не сошлась";
    case ParseError::MalformedHeader:
        return "заголовок нулевой длины или за границей пакета";
    case ParseError::NotConnect:
        return "ответ на CONNECT не является успешным";
    }
    return "неизвестная ошибка разбора";
}


const Header *Response::find( HeaderId id ) const {
    for ( const Header &h : headers )
        if ( h.id == uint8_t( id ) )
            return &h;
    return nullptr;
}


int declaredLength( const QByteArray &buffer ) {
    if ( buffer.size() < BASE_PACKET_LENGTH )
        return -1;
    return be16( buffer, 1 );
}


ParseError parseHeaders( const QByteArray &area, std::vector< Header > &out ) {
    out.clear();

    int i = 0;
    while ( i < area.size() ) {
        const uint8_t id = byteAt( area, i );
        const HeaderClass cls = headerClass( id );

        int step = 0;
        int bodyAt = 0;
        int bodyLen = 0;

        switch ( cls ) {
        case HeaderClass::Unicode:
        case HeaderClass::ByteSeq: {
            // Поле длины само по себе может не поместиться в остаток
            // области — тогда заголовка нет, а есть обрывок.
            if ( i + BASE_PACKET_LENGTH > area.size() )
                return ParseError::MalformedHeader;
            const int declared = be16( area, i + 1 );
            // Длина считает идентификатор и оба своих байта, поэтому
            // меньше трёх она быть не может физически. Здесь и закрыта
            // первая грабля эталонного разбора: там ветка класса 00 не
            // сдвигала индекс вовсе, и любой такой заголовок вешал
            // разбор навсегда. Ноль и отрицательный шаг невозможны по
            // построению — шаг не меньше трёх.
            if ( declared < BASE_PACKET_LENGTH )
                return ParseError::MalformedHeader;
            if ( i + declared > area.size() )
                return ParseError::MalformedHeader;
            step = declared;
            bodyAt = i + BASE_PACKET_LENGTH;
            bodyLen = declared - BASE_PACKET_LENGTH; // ноль законен: пустое тело
            break;
        }
        case HeaderClass::Byte1:
            if ( i + 2 > area.size() )
                return ParseError::MalformedHeader;
            step = 2;
            bodyAt = i + 1;
            bodyLen = 1;
            break;
        case HeaderClass::Byte4:
            if ( i + 5 > area.size() )
                return ParseError::MalformedHeader;
            step = 5;
            bodyAt = i + 1;
            bodyLen = 4;
            break;
        }

        // Заголовки складываются ПОСЛЕДОВАТЕЛЬНОСТЬЮ, а не словарём по
        // идентификатору. Словарь — третья грабля эталонного кода:
        // повторные `0x48` затирают друг друга, и длинный ответ там не
        // склеивается вовсе, ни при каких настройках.
        Header h;
        h.id = id;
        h.body = area.mid( bodyAt, bodyLen );
        out.push_back( h );

        i += step;
    }
    return ParseError::None;
}


ParseError parseResponse( const QByteArray &packet, Response &out ) {
    out = Response{};

    if ( packet.size() < BASE_PACKET_LENGTH )
        return ParseError::Incomplete;

    const int declared = declaredLength( packet );
    // Объявленная длина меньше собственного заголовка пакета — это порча,
    // а не незавершённость: сколько ни жди, такой пакет не станет целым.
    if ( declared < BASE_PACKET_LENGTH )
        return ParseError::LengthMismatch;
    if ( packet.size() < declared )
        return ParseError::Incomplete;
    // Лишние байты — тоже расхождение длины: читатель порта обязан взять
    // ровно объявленное, потому что сразу за пакетом в потоке может
    // лежать начало следующего, и склеенная пара разобралась бы как один
    // испорченный пакет.
    if ( packet.size() > declared )
        return ParseError::LengthMismatch;

    // Код ответа сохраняется как пришёл, даже если старший бит в нём не
    // стоит (а по первоисточнику он стоит всегда). Отвергать по этому
    // признаку нечем: отдельного исхода «код ответа неправдоподобен» в
    // договоре нет, а занимать под него чужое имя ошибки значило бы
    // соврать вызывающему о причине. Рассинхронизацию потока ловят длина
    // и сумма — оба признака здесь проверены.
    out.code = byteAt( packet, 0 );
    out.declaredLength = declared;
    out.checksumPresent = hasChecksum( packet );
    out.checksumOk = out.checksumPresent && checksumValid( packet );

    // Сумма проверяется ДО разбора заголовков: если она не сошлась, байты
    // области заголовков недостоверны, и `MalformedHeader` на них назвал
    // бы следствие вместо причины. Вызывающему нужна причина — по ней он
    // шлёт перезапрос `0x92`.
    if ( out.checksumPresent && !out.checksumOk )
        return ParseError::BadChecksum;

    return parseHeaders( packet.mid( BASE_PACKET_LENGTH, declared - BASE_PACKET_LENGTH ), out.headers );
}


ParseError parseConnectResponse( const QByteArray &packet, ConnectInfo &out ) {
    out = ConnectInfo{};

    // Длина и сумма проверяются тем же порядком, что у обычного ответа,
    // но разбирать заголовки `parseResponse()` здесь нельзя: у ответа на
    // CONNECT перед ними стоят четыре поля пакета, и проход от третьего
    // байта принял бы номер версии за идентификатор заголовка.
    if ( packet.size() < BASE_PACKET_LENGTH )
        return ParseError::Incomplete;

    const int declared = declaredLength( packet );
    if ( declared < BASE_PACKET_LENGTH )
        return ParseError::LengthMismatch;
    if ( packet.size() < declared )
        return ParseError::Incomplete;
    if ( packet.size() > declared )
        return ParseError::LengthMismatch;

    out.response.code = byteAt( packet, 0 );
    out.response.declaredLength = declared;
    // Раскладка здесь ИЗВЕСТНА, поэтому она и пробуется первой. Разница
    // не умозрительная: у ответа на CONNECT байт версии `0x10` попадает в
    // класс 00, и область от третьего байта при некоторых объявленных
    // буферах разбирается целиком — а вывод из такого разбора будет
    // неверным, потому что разбиралась не область заголовков.
    out.response.checksumPresent = crcLast( packet, BASE_PACKET_LENGTH + CONNECT_FIXED, BASE_PACKET_LENGTH );
    out.response.checksumOk = out.response.checksumPresent && sumIsZero( packet );

    // Сумма проверяется ДО кода ответа намеренно: испорченный байт кода —
    // ровно то, от чего сумма и защищает. Вернуть здесь `NotConnect`
    // значило бы назвать порчу линии отказом прибора и лишить вызывающего
    // единственного верного действия — перезапроса `0x92`.
    if ( out.response.checksumPresent && !out.response.checksumOk )
        return ParseError::BadChecksum;

    // Успех — единственный код, при котором у пакета есть фиксированные
    // поля. `0xC0`, `0xD1` и прочие несут отказ, а не соединение, и
    // читать из них версию с буфером было бы чтением чужих байт. Код при
    // этом уже сохранён: вызывающему нужен именно он, чтобы отличить
    // «прибор отказал» от «прибора нет».
    if ( out.response.code != uint8_t( Rsp::Success ) )
        return ParseError::NotConnect;
    if ( declared < BASE_PACKET_LENGTH + CONNECT_FIXED )
        return ParseError::MalformedHeader;

    out.version = byteAt( packet, 3 );
    out.flags = byteAt( packet, 4 );

    const int rx = be16( packet, 5 );
    // Нулевой буфер прибором не объявлен, а не объявлен нулевым: прибора,
    // способного принять ноль байт, не существует, а вот потерянный при
    // передаче байт даёт ноль легко. Поэтому здесь остаётся нижний предел
    // OBEX и признак «прибор буфер не назвал» — резать исходящее всё
    // равно придётся по `SAFE_TX_LIMIT`.
    if ( rx > 0 ) {
        out.deviceRxBuffer = uint16_t( rx );
        out.deviceRxKnown = true;
    }

    const int headersAt = BASE_PACKET_LENGTH + CONNECT_FIXED;
    const ParseError err = parseHeaders( packet.mid( headersAt, declared - headersAt ), out.response.headers );
    if ( err != ParseError::None )
        return err;

    // Идентификатор соединения берётся ТОЛЬКО отсюда. В документах
    // вендора его нет вовсе, и в перечне поддерживаемых прибором
    // заголовков он не значится: добавлять его в запросы по своей воле
    // — догадка, а догадка в запросе к прибору стоит сессии.
    //
    // Берётся ПОСЛЕДНИЙ, если их почему-то несколько: повтор здесь
    // патологичен и означает порчу, а эталонная реализация в этом случае
    // оставляет у себя последний — совпадение поведения дороже
    // произвольного выбора между двумя одинаково безосновательными.
    for ( const Header &h : out.response.headers ) {
        if ( h.id != uint8_t( HeaderId::ConnectionId ) )
            continue;
        if ( h.body.size() != 4 )
            continue; // не четырёхбайтовый — это не идентификатор соединения
        out.hasConnectionId = true;
        out.connectionId = h.uint32Value();
    }

    return ParseError::None;
}

// ===========================================================================
// Склейка длинного ответа
// ===========================================================================

void BodyAssembler::reset() {
    m_body.clear();
    m_parts = 0;
    m_complete = false;
}


bool BodyAssembler::feed( const Response &r ) {
    // Законченный объект больше не растёт. Дописать к нему пришедшее
    // после `0x49` значило бы склеить два разных объекта в один и
    // показать эту склейку как одну осциллограмму; ошибка при этом
    // выглядела бы как помеха на входе, то есть как свойство сигнала.
    if ( m_complete )
        return false;

    bool took = false;
    for ( const Header &h : r.headers ) {
        if ( h.id == uint8_t( HeaderId::BodyPart ) ) {
            m_body.append( h.body );
            ++m_parts;
            took = true;
            continue;
        }
        if ( h.id == uint8_t( HeaderId::BodyEnd ) ) {
            // Пустое тело `0x49` — тоже часть: это штатный ответ ждущего
            // запуска, у которого истёк предел ожидания. Кадра нет, связь
            // цела, и отличить одно от другого можно только по тому, что
            // часть ПРИШЛА при пустом теле.
            m_body.append( h.body );
            ++m_parts;
            m_complete = true;
            took = true;
            break; // всё, что стоит за концом объекта, к нему не относится
        }
    }
    return took;
}

} // namespace Oscill
