// SPDX-License-Identifier: GPL-3.0-or-later

#include "oscillprotocol.h"

#include <algorithm>
#include <cmath>
#include <limits>

/// Реализация знания о приборе Oscill. Ни одного системного вызова: на
/// входе байты и числа, на выходе байты и числа. Это и есть причина, по
/// которой файл существует отдельно от бэкенда — всё здесь проверяется
/// тестами без прибора, а прибора в работе нет.
///
/// СОСТОЯНИЕ УТВЕРЖДЕНИЙ. Всё ниже — **[СБОРКА]**: выведено из
/// первоисточника и двух эталонных реализаций, проверяется сборкой и
/// тестами, но не измерено на приборе. Там, где источники расходятся,
/// выбор не делается молча: он либо вынесен в аргумент типа `CodeSpan` /
/// `FrameLayout` / `WriteStyle`, либо выражен явной проверкой, которая
/// ОТКАЗЫВАЕТ. Отказ вслух дешевле молчаливо кривого числа: у измерения
/// ошибка не слышна (`docs/СЛЫШИМОСТЬ.md`).

namespace Oscill {

namespace {

// ---------------------------------------------------------------------------
// Мелочи, которых не хватает стандартной библиотеке для этой задачи
// ---------------------------------------------------------------------------

/// Байт тела как БЕЗЗНАКОВЫЙ. `QByteArray` отдаёт `char`, знаковость
/// которого зависит от платформы: на ARM `char` беззнаковый, на x86
/// знаковый, и байт 0x80 без этого приведения превратился бы в −128.
/// Все выборки прибора беззнаковые — путать нельзя.
inline uint8_t u8( const QByteArray &b, int index ) {
    return uint8_t( b.at( index ) );
}

/// Два байта big-endian. Порядок задан первоисточником для всех
/// многобайтовых полей прибора и не является предметом выбора.
inline int be16( const QByteArray &b, int index ) {
    return ( int( u8( b, index ) ) << 8 ) | int( u8( b, index + 1 ) );
}

/// Округление в беззнаковое с ЯВНОЙ проверкой диапазона.
///
/// `std::lround` на числе, не влезающем в `long`, даёт неопределённое
/// поведение, а не насыщение, — то есть на большом периоде выборки
/// молча вернул бы произвольное значение регистра. Здесь предел
/// проверяется до преобразования. NaN отсеивается сравнением
/// `!( v > 0.0 )`: у NaN ложны ВСЕ сравнения, и это единственный
/// способ его поймать, не подключая проверку отдельно.
uint32_t roundToU32( double v ) {
    if ( !( v > 0.0 ) )
        return 0;
    const double r = std::floor( v + 0.5 );
    if ( r >= 4294967295.0 )
        return 0xFFFFFFFFu;
    return uint32_t( r );
}

uint16_t roundToU16( double v, uint16_t maxValue ) {
    if ( !( v > 0.0 ) )
        return 0;
    const double r = std::floor( v + 0.5 );
    if ( r >= double( maxValue ) )
        return maxValue;
    return uint16_t( r );
}

int16_t roundToI16( double v ) {
    if ( !std::isfinite( v ) )
        return 0;
    const double r = std::floor( v + 0.5 );
    if ( r <= -32768.0 )
        return int16_t( -32768 );
    if ( r >= 32767.0 )
        return int16_t( 32767 );
    return int16_t( r );
}

/// Милливольты на ОДИН код АЦП при заданной рамке пересчёта.
///
/// Единственное место, где рамка `CodeSpan` превращается в число, — и
/// потому единственное место, которое нужно править, когда измерение на
/// приборе закроет спор «8 мВ против 8,53 мВ». Значение перечисления
/// само есть число кодов на экран, а `Unverified == 0` попадает в общую
/// проверку `codes <= 0`: отдельной ветки «а если не установлено» здесь
/// нет, потому что второй ветки — второй источник решения.
///
/// Экран занимает `divs` делений по `v1` милливольт, и на него отведено
/// `codes` кодов. Тогда код стоит `v1 × divs / codes` милливольт. При
/// `Codes256` это ровно `V1 × 8 / 256` обеих эталонных реализаций; при
/// `Codes240` — то же самое, отнесённое к 240 кодам экрана, отчего
/// полный диапазон АЦП в 256 кодов оказывается в 256/240 = 1,067 раза
/// шире экрана. Отсюда и вторая цифра первоисточника: 8 × 256/240 = 8,53.
bool codeStepMv( uint16_t v1, CodeSpan span, int divs, double &out ) {
    const int codes = int( span );
    if ( codes <= 0 || divs <= 0 || v1 == 0 )
        return false;
    out = double( v1 ) * double( divs ) / double( codes );
    return true;
}

// ---------------------------------------------------------------------------
// Таблицы имён
// ---------------------------------------------------------------------------

/// Заглушка для значения перечисления, которого в таблице нет. Возникнуть
/// может только приведением постороннего числа к типу перечисления, но
/// возвращать в таком случае первую строку таблицы нельзя: молчаливая
/// подмена `MC` на что угодно отправила бы прибору не тот регистр.
/// Нулевая ширина не собирается ни в один пакет — сборщики её проверяют.
const PropertyDef UNKNOWN_PROPERTY{ "???", 0, false, false, "" };
const RegisterDef UNKNOWN_REGISTER{ "??", 0, false, "" };

/// Свойства в порядке перечисления. Ширина — сколько байт значение
/// занимает в заголовке ответа; четырёхбайтовые ASCII-свойства приходят
/// заголовком `0xF1`, чьё тело есть текст, а не число.
const PropertyDef PROPERTY_TABLE[] = {
    // Паспорт: эталонный код спрашивает эти четыре.
    { "VNM", 4, false, true, "" },
    { "VSN", 4, false, true, "" },
    { "VHW", 4, false, true, "" },
    { "VSW", 4, false, true, "" },
    // Версии узлов: их перечисляет конспект протокола, и их не
    // спрашивает ни одна эталонная реализация. Какой из двух списков
    // поддержан прибором, показывает ответ `0xD1` на остальные, —
    // выясняется опросом, а не выбором на бумаге.
    { "VHD", 4, false, true, "" },
    { "VHA", 4, false, true, "" },
    { "VSD", 4, false, true, "" },
    { "VSI", 4, false, true, "" },
    { "VSC", 4, false, true, "" },
    { "VSO", 4, false, true, "" },
    // Времязадающая часть.
    { "MCd", 2, false, false, "10 пс" },
    { "MCl", 2, false, false, "10 пс" },
    { "TOl", 2, false, false, "такты×256" },
    // TOv — не величина, а битовая карта: единицы у неё нет, и
    // подставлять сюда «такты» значило бы разрешить пересчёт, которого
    // не существует. Разбирает её `fastRealtimeTicks()`.
    { "TOv", 4, false, false, "" },
    { "TMl", 2, false, false, "такты×256" },
    { "TMh", 2, false, false, "такты×256" },
    { "TPl", 4, false, false, "такты×256" },
    { "TCh", 2, false, false, "выборка" },
    { "QSh", 2, false, false, "выборка" },
    { "TDl", 4, false, false, "12 тактов" },
    { "TDh", 4, false, false, "12 тактов" },
    // Канал. Имена V1h/V1l обманывают: V1h — это 10 В/дел, то есть
    // БОЛЬШЕЕ число милливольт на деление и НИЗШАЯ чувствительность.
    { "V1h", 2, false, false, "мВ/дел" },
    { "V1l", 2, false, false, "мВ/дел" },
    { "P1h", 2, true, false, "1/256 АЦП" },
    { "P1l", 2, true, false, "1/256 АЦП" },
    { "D1m", 2, false, false, "10 пс" },
};

static_assert( sizeof( PROPERTY_TABLE ) / sizeof( PROPERTY_TABLE[ 0 ] ) == size_t( Property::D1m ) + 1,
               "таблица свойств разошлась с перечислением Property" );

/// Регистры в порядке перечисления (он же порядок приоритета; порядок
/// ЗАПИСИ другой и живёт в `initOrder()`).
const RegisterDef REGISTER_TABLE[] = {
    { "MC", 2, false, "10 пс" },
    { "TS", 4, false, "такты×256" },
    // Побитовые регистры единицы не имеют: у набора флагов её нет.
    // Пустая строка здесь не «забыли заполнить», а утверждение.
    { "RS", 1, false, "" },
    { "AP", 1, false, "проходов−1" },
    { "AR", 1, false, "оцифровок" },
    { "QS", 2, false, "выборка" },
    { "TC", 2, false, "выборка" },
    { "TD", 4, false, "12 тактов" },
    { "RT", 1, false, "" },
    { "TA", 4, false, "12 тактов" },
    { "TW", 4, false, "12 тактов" },
    { "O1", 1, false, "" },
    { "V1", 2, false, "мВ/дел" },
    // Единственный знаковый регистр прибора.
    { "P1", 2, true, "1/256 АЦП" },
    { "M1", 1, false, "" },
    { "T1", 1, false, "" },
    { "S1", 1, false, "1/256 АЦП" },
};

static_assert( sizeof( REGISTER_TABLE ) / sizeof( REGISTER_TABLE[ 0 ] ) == size_t( Register::S1 ) + 1,
               "таблица регистров разошлась с перечислением Register" );

// ---------------------------------------------------------------------------
// Поиск значения в ответе
// ---------------------------------------------------------------------------

bool isValueHeader( uint8_t id ) {
    return id == uint8_t( HeaderId::Value1 ) || id == uint8_t( HeaderId::Value2 ) ||
           id == uint8_t( HeaderId::Value4 );
}

/// Заголовок значения из ответа. Предпочитается тот, чей идентификатор
/// отвечает объявленной ширине; если прибор ответил заголовком другой
/// ширины, берётся он — приведение всё равно делается по ширине из
/// таблицы, а не по тому, что пришло. Первоисточник такого случая не
/// описывает, и отбрасывать значение молча было бы хуже, чем привести
/// его и дать вызывающему сверить факт (П3).
const Header *findValueHeader( const Response &r, HeaderId preferred ) {
    const Header *any = nullptr;
    for ( const Header &h : r.headers ) {
        if ( !isValueHeader( h.id ) )
            continue;
        if ( h.id == uint8_t( preferred ) )
            return &h;
        if ( !any )
            any = &h;
    }
    return any;
}

uint32_t rawValue( const Header &h ) {
    switch ( HeaderId( h.id ) ) {
    case HeaderId::Value1:
        return h.byteValue();
    // 0xF0: на проводе четыре байта, значащие — младшие два. Старшие два
    // по первоисточнику всегда нулевые; если прибор пришлёт иное, берутся
    // всё равно младшие — так делают обе эталонные реализации.
    case HeaderId::Value2:
        return h.oscillWordValue();
    case HeaderId::Value4:
        return h.uint32Value();
    default:
        return 0;
    }
}

uint32_t maskByWidth( uint32_t v, uint8_t width ) {
    switch ( width ) {
    case 1:
        return v & 0xFFu;
    case 2:
        return v & 0xFFFFu;
    default:
        return v;
    }
}

/// Имя в ответе, если прибор его назвал, обязано совпадать с
/// запрошенным. Отсутствие имени ошибкой НЕ считается: первоисточник
/// требует, чтобы имя предшествовало значению в ЗАПРОСЕ, а про ответ
/// такого требования не даёт, и его пример имя содержит. Строго там,
/// где источник говорит; терпимо там, где он молчит.
bool nameAgrees( const Response &r, HeaderId nameId, const char *name, int nameLen ) {
    const Header *h = r.find( nameId );
    if ( !h )
        return true;
    return h->asciiValue() == QByteArray( name, nameLen );
}

} // namespace

// ===========================================================================
// Свойства
// ===========================================================================

const PropertyDef &propertyDef( Property p ) {
    const size_t i = size_t( p );
    if ( i >= sizeof( PROPERTY_TABLE ) / sizeof( PROPERTY_TABLE[ 0 ] ) )
        return UNKNOWN_PROPERTY;
    return PROPERTY_TABLE[ i ];
}


const std::vector< Property > &allProperties() {
    static const std::vector< Property > all = [] {
        std::vector< Property > v;
        for ( int i = 0; i <= int( Property::D1m ); ++i )
            v.push_back( Property( i ) );
        return v;
    }();
    return all;
}


std::vector< uint8_t > fastRealtimeTicks( uint32_t tov ) {
    // Раскладка первоисточника выглядит нерегулярной, но регулярна:
    // младший бит слова — это «полтакта», и дальше номер бита РАВЕН
    // числу тактов на выборку. Проверка по таблице вендора: байт TOv+3
    // бит 0 = 1/2, бит 7 = 7 тактов; байт TOv+2 бит 0 = 8 тактов
    // (это бит 8 слова), бит 7 = 15; байт TOv+1 биты 0…7 = 16…23; байт
    // TOv бит 0 = 24, бит 6 = 30, бит 7 не определён («-»).
    //
    // Отсюда перебор идёт прямо по битам слова, а не по байтам: байтовая
    // раскладка — способ ИЗЛОЖЕНИЯ в документе, а не устройство величины.
    //
    // Двухбайтовый пример вендора при четырёхбайтовом объявленном формате
    // разбирается тем же кодом: старшие нули просто не дают единиц.
    // Дополнять значение нулями отдельно не требуется — оно уже
    // расширено до 32 бит типом аргумента.
    std::vector< uint8_t > out;
    for ( int bit = 0; bit <= 30; ++bit ) {
        if ( tov & ( uint32_t( 1 ) << bit ) )
            // Нуль здесь означает ПОЛТАКТА, а не «ноль тактов»: целых
            // «полтактов» не бывает, и вызывающий обязан различать 0 и 1.
            // Различие живёт в объявлении функции, а не в этом значении.
            out.push_back( uint8_t( bit ) );
    }
    return out;
}

// ===========================================================================
// Регистры
// ===========================================================================

const RegisterDef &registerDef( Register r ) {
    const size_t i = size_t( r );
    if ( i >= sizeof( REGISTER_TABLE ) / sizeof( REGISTER_TABLE[ 0 ] ) )
        return UNKNOWN_REGISTER;
    return REGISTER_TABLE[ i ];
}


const std::vector< Register > &allRegisters() {
    static const std::vector< Register > all = [] {
        std::vector< Register > v;
        for ( int i = 0; i <= int( Register::S1 ); ++i )
            v.push_back( Register( i ) );
        return v;
    }();
    return all;
}


HeaderId valueHeaderId( uint8_t width ) {
    switch ( width ) {
    case 1:
        return HeaderId::Value1;
    case 2:
        return HeaderId::Value2;
    case 4:
        return HeaderId::Value4;
    default:
        break;
    }
    // Иной ширины среди объявленных регистров нет. Возврат `Value1` —
    // не молчаливая подстановка: сборщик пакета ширину проверяет сам и
    // регистр негодной ширины собирать отказывается, так что это
    // значение до провода не доходит.
    return HeaderId::Value1;
}


int16_t signed16( uint32_t raw ) {
    // Первоисточник и эталонный код записывают разворот как
    // `(v << 16) >> 16`, что опирается на арифметический сдвиг знакового
    // числа — поведение, определённое стандартом только с C++20.
    // Здесь то же самое выражено вычитанием: результат тот же на любой
    // машине, а неопределённого поведения нет.
    const uint32_t low = raw & 0xFFFFu;
    if ( low < 0x8000u )
        return int16_t( low );
    return int16_t( int32_t( low ) - 0x10000 );
}

// ===========================================================================
// Сборка запросов
// ===========================================================================

QByteArray makeReadProperty( Property p, bool withChecksum ) {
    const PropertyDef &def = propertyDef( p );
    if ( def.width == 0 )
        return QByteArray();
    // Свойства только читаются, и читаются пакетом GET: единственный
    // пример вендора на чтение — `0x83`.
    Builder b( Op::GetFinal );
    b.addName( HeaderId::Property, QByteArray( def.name, 3 ) );
    // Предел исходящего пакета спрашивается, а не подразумевается:
    // прибору нельзя слать больше `SAFE_TX_LIMIT`, и место этого числа
    // одно — кодек.
    if ( !b.fits( withChecksum ) )
        return QByteArray();
    return b.build( withChecksum );
}


QByteArray makeReadRegister( Register r, bool withChecksum ) {
    const RegisterDef &def = registerDef( r );
    if ( def.width == 0 )
        return QByteArray();
    Builder b( Op::GetFinal );
    b.addName( HeaderId::Register, QByteArray( def.name, 2 ) );
    if ( !b.fits( withChecksum ) )
        return QByteArray();
    return b.build( withChecksum );
}


QByteArray makeWriteRegister( Register r, uint32_t value, WriteStyle style, bool withChecksum ) {
    const RegisterDef &def = registerDef( r );
    if ( def.width != 1 && def.width != 2 && def.width != 4 )
        return QByteArray();

    // Обе формы записи документированы первоисточником, и эталонные
    // реализации разошлись: одна пишет GET, другая PUT. Выбор пришёл
    // аргументом — значения по умолчанию у него нет намеренно. [СБОРКА]
    Builder b( style == WriteStyle::CombinedGet ? Op::GetFinal : Op::PutFinal );

    // Имя ОБЯЗАНО предшествовать значению в том же пакете. Обеспечивает
    // это порядок вызовов: сборщик заголовки не сортирует.
    b.addName( HeaderId::Register, QByteArray( def.name, 2 ) );

    // Знаковый регистр (P1) приходит сюда уже в дополнительном коде,
    // расширенном до 32 бит; маска отрезает знаковое расширение,
    // которого на проводе нет: −384 = 0xFFFFFE80 → 0xFE80. Для
    // беззнаковых та же маска отрезает то, что в регистр не влезло бы, —
    // и подрезка видна вызывающему обратным чтением факта (П3), потому
    // что сравнивать он обязан не с желаемым, а с прочитанным.
    switch ( def.width ) {
    case 1:
        b.addByte( HeaderId::Value1, uint8_t( value & 0xFFu ) );
        break;
    case 2:
        b.addOscillWord( uint16_t( value & 0xFFFFu ) );
        break;
    default:
        b.addUint32( HeaderId::Value4, value );
        break;
    }

    if ( !b.fits( withChecksum ) )
        return QByteArray();
    return b.build( withChecksum );
}


QByteArray makeCommand( char cmd, bool asPut, bool withChecksum ) {
    // Имя команды — один печатаемый символ ASCII. Байт вне этого
    // диапазона прибор прочтёт не как «другая команда», а как
    // испорченный пакет; отправлять такое — расходовать защитный
    // интервал на заведомо отвергнутый запрос.
    const unsigned char c = static_cast< unsigned char >( cmd );
    if ( c < 0x21 || c > 0x7E )
        return QByteArray();

    // 'F' — загрузка фрагмента прошивки. Отвергается здесь намеренно и
    // явно: к измерению она отношения не имеет, ведётся многопакетной
    // передачей образа, а ошибка в ней стоит прибора. Прошивку пишут
    // отдельным инструментом, а не побочным действием осциллографа.
    if ( cmd == 'F' )
        return QByteArray();

    Builder b( asPut ? Op::PutFinal : Op::GetFinal );
    b.addName( HeaderId::Command, QByteArray( &cmd, 1 ) );
    if ( !b.fits( withChecksum ) )
        return QByteArray();
    return b.build( withChecksum );
}


bool valueFromResponse( const Response &r, Register reg, uint32_t &out ) {
    const RegisterDef &def = registerDef( reg );
    if ( def.width == 0 )
        return false;
    if ( !nameAgrees( r, HeaderId::Register, def.name, 2 ) )
        return false;

    const Header *h = findValueHeader( r, valueHeaderId( def.width ) );
    if ( !h )
        // Значения в ответе нет. Причин две — `0xD1 Not Implemented`
        // (такого регистра у прибора нет) и просто пустой ответ, — но
        // исход один: величины НЕТ. Ноль сюда не пишется: ноль есть
        // законное значение всех регистров прибора, и отличить его от
        // «не прочитано» вызывающий тогда не смог бы.
        return false;

    out = maskByWidth( rawValue( *h ), def.width );
    return true;
}


bool valueFromResponse( const Response &r, Property prop, uint32_t &out ) {
    const PropertyDef &def = propertyDef( prop );
    if ( def.width == 0 )
        return false;
    if ( def.isAscii )
        // У текстового свойства числа нет вовсе. Вернуть четыре байта
        // ASCII как целое — значит выдать за величину то, что величиной
        // не является; текст берут `asciiFromResponse()`.
        return false;
    if ( !nameAgrees( r, HeaderId::Property, def.name, 3 ) )
        return false;

    const Header *h = findValueHeader( r, valueHeaderId( def.width ) );
    if ( !h )
        return false;

    // Знаковые свойства (P1h/P1l) отдаются сырыми шестнадцатью битами:
    // разворачивает их `signed16()` в точке, где известно, что величина
    // знаковая. Двух мест для одного разворота не заводится.
    out = maskByWidth( rawValue( *h ), def.width );
    return true;
}


bool asciiFromResponse( const Response &r, QByteArray &out ) {
    for ( const Header &h : r.headers ) {
        if ( !isValueHeader( h.id ) )
            continue;
        // Имя свойства (`0x70`) в ответе тоже ASCII, и брать его вместо
        // значения — готовая ошибка: прибор ответил бы «MCd» на запрос
        // «MCd». Поэтому перебираются только заголовки ЗНАЧЕНИЯ.
        out = h.asciiValue();
        // Пустая строка — законный исход: прибор ответил четырьмя
        // нулями. Это «значение прочитано и оно пусто», а не «не
        // прочитано»; различие несёт возвращаемое значение.
        return true;
    }
    return false;
}

// ===========================================================================
// Побитовые раскладки
// ===========================================================================

uint8_t encode( const AcquisitionMode &m ) {
    uint8_t v = 0;
    if ( m.ris )
        v |= 0x01;
    if ( m.parallel )
        v |= 0x02;
    if ( m.roll )
        v |= 0x04;
    return v;
}


AcquisitionMode decodeRs( uint8_t rs ) {
    AcquisitionMode m;
    m.ris = ( rs & 0x01 ) != 0;
    m.parallel = ( rs & 0x02 ) != 0;
    m.roll = ( rs & 0x04 ) != 0;
    // Бит 3 первоисточник в записи `RS = 0b0000X011` объявляет
    // безразличным, биты 7-4 не определяет вовсе. Они не читаются и не
    // пишутся: приписать им смысл здесь значило бы выдумать его.
    return m;
}


uint8_t encode( TriggerStart t ) {
    // Значение перечисления и есть пара бит 1-0, других бит у RT нет.
    return uint8_t( t );
}


TriggerStart decodeRt( uint8_t rt ) {
    return TriggerStart( uint8_t( rt & 0x03 ) );
}


uint8_t encode( const ChannelHwMode &m ) {
    uint8_t v = 0;
    // Бит 0 — «вход заземлён», а НЕ «канал включён». Поле названо по
    // смыслу бита: эталонная реализация пишет его инверсией (верно), а
    // читает как есть — и сообщает «канал включён» ровно тогда, когда
    // он заземлён.
    if ( m.grounded )
        v |= 0x01;
    if ( m.acCoupling )
        v |= 0x02;
    if ( m.filter3MHz )
        v |= 0x04;
    if ( m.filter3kHz )
        v |= 0x08;
    return v;
}


ChannelHwMode decodeO1( uint8_t o1 ) {
    ChannelHwMode m;
    m.grounded = ( o1 & 0x01 ) != 0;
    m.acCoupling = ( o1 & 0x02 ) != 0;
    m.filter3MHz = ( o1 & 0x04 ) != 0;
    m.filter3kHz = ( o1 & 0x08 ) != 0;
    return m;
}


uint8_t encode( const ChannelSyncMode &m ) {
    uint8_t v = 0;
    if ( m.onFall )
        v |= 0x10; // бит 4
    if ( m.onRise )
        v |= 0x20; // бит 5
    // Гистерезисы и полоса заданы ПАРАМИ бит: первоисточник знает у них
    // только комбинации 00 и 11. Одиночный бит пары — не «наполовину
    // включено», а состояние, о котором документ не говорит ничего;
    // поэтому пишутся обе. [СБОРКА]
    if ( m.fallHysteresis )
        v |= 0x03; // биты 1,0
    if ( m.riseHysteresis )
        v |= 0x0C; // биты 3,2
    if ( m.lowFrequencyOnly )
        v |= 0xC0; // биты 7,6
    return v;
}


ChannelSyncMode decodeT1( uint8_t t1 ) {
    ChannelSyncMode m;
    m.onFall = ( t1 & 0x10 ) != 0;
    m.onRise = ( t1 & 0x20 ) != 0;
    // Читается младший бит пары. Старший при этом игнорируется: пара
    // 01 или 10 первоисточником не описана, и достраивать по ней
    // «наверное включено» — догадка. Кому важно расхождение пары, тот
    // видит сырой байт: он остаётся в настройках как есть. [СБОРКА]
    m.fallHysteresis = ( t1 & 0x01 ) != 0;
    m.riseHysteresis = ( t1 & 0x04 ) != 0;
    m.lowFrequencyOnly = ( t1 & 0x40 ) != 0;
    return m;
}


bool decodeSampleFormat( uint8_t raw, SampleFormat &out ) {
    // Маска обязательна. Без неё любой установленный резервный бит 7-3
    // уводит распознавание в `Normal` МОЛЧА — именно это делает
    // эталонная реализация на Java, сравнивая все восемь бит разом. [СБОРКА]
    switch ( uint8_t( raw & 0x07 ) ) {
    case uint8_t( SampleFormat::Avg ):
        out = SampleFormat::Avg;
        return true;
    case uint8_t( SampleFormat::AvgHiRes ):
        out = SampleFormat::AvgHiRes;
        return true;
    case uint8_t( SampleFormat::PeakInterlaced ):
        out = SampleFormat::PeakInterlaced;
        return true;
    case uint8_t( SampleFormat::PeakPaired ):
        out = SampleFormat::PeakPaired;
        return true;
    case uint8_t( SampleFormat::Normal ):
        out = SampleFormat::Normal;
        return true;
    default:
        break;
    }
    // Коды 101, 110, 111 не объявлены ни первоисточником, ни эталонным
    // кодом. `out` не трогается: подставить сюда «наверное Normal»
    // значило бы разобрать чужой массив как свой.
    return false;
}


uint8_t encode( SampleFormat f ) { return uint8_t( f ); }


const char *sampleFormatName( SampleFormat f ) {
    switch ( f ) {
    case SampleFormat::Avg:
        return "усреднение с отбрасыванием младших разрядов";
    case SampleFormat::AvgHiRes:
        return "усреднение с повышением разрешения";
    case SampleFormat::PeakInterlaced:
        return "пиковый поочерёдно";
    case SampleFormat::PeakPaired:
        return "пиковый повыборочно";
    case SampleFormat::Normal:
        return "обычный";
    }
    return "формат не объявлен";
}


int sampleBytes( SampleFormat f ) {
    // Сколько БАЙТ массива приходится на одну выборку. У
    // `PeakInterlaced` байт один: «2 байта / 2 выборки» первоисточника
    // означает, что минимум и максимум занимают СВОИ соседние позиции
    // на оси времени, а не одну общую. Эталонная реализация на Python
    // прочла это как «две точки в одну» и потеряла половину точек. [СБОРКА]
    switch ( f ) {
    case SampleFormat::AvgHiRes:
    case SampleFormat::PeakPaired:
        return 2;
    default:
        break;
    }
    return 1;
}


int sampleWordBytes( SampleFormat f ) {
    // Ширина одного ЧИТАЕМОГО целого. У `PeakPaired` она равна единице,
    // хотя байт на выборку два: поток читается побайтно, а на выборку
    // приходится два байтовых элемента — минимум и максимум. Величины
    // разные, и путать их нельзя: по первой считают число выборок, по
    // второй — как достать значение.
    return f == SampleFormat::AvgHiRes ? 2 : 1;
}


int samplesInBytes( SampleFormat f, int sizeBytes ) {
    if ( sizeBytes <= 0 )
        return 0;
    // Деление нацело: массив нечётной длины в двухбайтовом формате
    // содержит недописанную выборку, и она НЕ округляется вверх.
    // Расхождение остатка обнаруживает `decodeSamples()` и объявляет
    // его отказом — здесь считается только то, что заведомо целое.
    return sizeBytes / sampleBytes( f );
}


int codeFullScale( SampleFormat f ) {
    return f == SampleFormat::AvgHiRes ? 0xFFFF : 0xFF;
}

// ===========================================================================
// Пересчёты в физические единицы
// ===========================================================================

double tickPs( uint16_t mc ) { return double( mc ) * TICK_UNIT_PS; }


uint16_t mcFromTickPs( double ps ) {
    if ( !( ps > 0.0 ) )
        return 0; // такта нет: ноль означает «не установлено»

    double n = std::floor( ps / TICK_UNIT_PS + 0.5 );
    // Регистр двухбайтовый, и значение свыше 65535 не «почти влезло»:
    // оно переписалось бы в совершенно другой такт, то есть в другую
    // скорость выборки, о которой никто не узнает. Подрезка явная, а её
    // факт вызывающий видит обратным пересчётом `tickPs()`: сравнивать
    // он обязан с прочитанным, а не с желаемым.
    if ( n < 1.0 )
        n = 1.0;
    if ( n > double( MC_MAX ) )
        n = double( MC_MAX );
    return uint16_t( n );
}


uint16_t mcFromMHz( double megahertz ) {
    if ( !( megahertz > 0.0 ) )
        return 0;
    // Тактовая частота КОНТРОЛЛЕРА, не скорость выборки: единица здесь
    // MHz, а не MSps, и смешивать их запрещено правилом проекта.
    // t[пс] = 1e12 / f[Гц] = 1e6 / f[МГц]; MC = t / 10 = 1e5 / f[МГц] —
    // та же формула, что в эталонном коде. [СБОРКА]
    return mcFromTickPs( 1.0e6 / megahertz );
}


double samplePeriodPs( uint32_t ts, uint16_t mc ) {
    // TS задан в тактах, умноженных на 256. Совпадает в обеих эталонных
    // реализациях. [СБОРКА]
    return double( ts ) * ( double( mc ) * TICK_UNIT_PS ) / double( TS_TICK_SCALE );
}


uint32_t tsFromSamplePeriodPs( double ps, uint16_t mc ) {
    const double tick = tickPs( mc );
    if ( !( tick > 0.0 ) || !( ps > 0.0 ) )
        return 0;
    return roundToU32( ps * double( TS_TICK_SCALE ) / tick );
}


double sampleRateSps( uint32_t ts, uint16_t mc ) {
    const double period = samplePeriodPs( ts, mc );
    if ( !( period > 0.0 ) )
        // Период не установлен. Ноль здесь означает именно это, а не
        // «ноль отсчётов в секунду»: «бесконечно быстро» не бывает, и
        // деления на ноль тут нет.
        return 0.0;
    // Единица результата — Sps (выборок в секунду), не Hz: скорость
    // выборки и частота сигнала — разные величины, и правило проекта
    // запрещает их смешивать. 1e12 — перевод пикосекунд в секунды.
    return 1.0e12 / period;
}


double timePerDivPs( uint32_t ts, uint16_t mc, int samplesPerDiv ) {
    if ( samplesPerDiv <= 0 )
        return 0.0;
    // Число выборок на деление — величина ПРЕДСТАВЛЕНИЯ, а не прибора:
    // одна эталонная реализация взяла 32, другая до 100. Умолчания у
    // аргумента нет намеренно.
    return samplePeriodPs( ts, mc ) * double( samplesPerDiv );
}


double delayPs( uint32_t units, uint16_t mc ) {
    // ВНИМАНИЕ. Единица «12 машинных тактов» объявлена первоисточником,
    // но НИ ОДНА эталонная реализация её не применяет: обе пишут в
    // TD/TA/TW сырые числа (500) и в секунды их не переводят. Формула
    // выведена из единицы измерения и кодом не подтверждена. Ошибка
    // здесь даст неверный защитный интервал ожидания — то есть лишний
    // повтор запроса, а не сбой связи. [СБОРКА]
    return double( units ) * double( DELAY_UNIT_TICKS ) * tickPs( mc );
}


uint32_t delayUnitsFromPs( double ps, uint16_t mc ) {
    const double unitPs = double( DELAY_UNIT_TICKS ) * tickPs( mc );
    if ( !( unitPs > 0.0 ) || !( ps > 0.0 ) )
        return 0;
    return roundToU32( ps / unitPs );
}


double milliVoltsPerDiv( uint16_t v1 ) {
    // Значение регистра равно числу милливольт на деление напрямую:
    // V1l = 20 ↔ 20 мВ/дел, V1h = 10000 ↔ 10 В/дел. Проверено обоими
    // примерами первоисточника. [СБОРКА]
    return double( v1 );
}


uint16_t v1FromMilliVoltsPerDiv( double mvPerDiv ) {
    // Верхний предел — разрядность регистра, а не предел прибора:
    // прибор объявляет свой предел свойством V1h, и сравнивать с ним
    // обязан вызывающий. Подставлять сюда 10000 значило бы завести
    // второе место, порождающее границу чувствительности.
    return roundToU16( mvPerDiv, 0xFFFF );
}


bool offsetMillivolts( int16_t p1, uint16_t v1, CodeSpan span, double &outMv, int divs ) {
    double stepMv = 0.0;
    if ( !codeStepMv( v1, span, divs, stepMv ) )
        // Рамка не установлена (`Unverified`) либо шкала вырождена
        // (v1 = 0, divs ≤ 0). В обоих случаях числа НЕТ, и `outMv` не
        // трогается: величина без установленной единицы не выдаётся.
        return false;
    // Единица P1 — «1/256 диапазона АЦП», то есть ровно один код АЦП.
    // При `Codes256` выражение сводится к `(V1 × 8 / 256) × P1`, которое
    // считают обе эталонные реализации.
    outMv = stepMv * double( p1 );
    return true;
}


bool p1FromOffsetMillivolts( double mv, uint16_t v1, CodeSpan span, int16_t &outP1, int divs ) {
    double stepMv = 0.0;
    if ( !codeStepMv( v1, span, divs, stepMv ) )
        return false;
    if ( !std::isfinite( mv ) )
        return false;
    // Насыщение по разрядности регистра, а не по границам прибора:
    // границы — свойства P1h/P1l, они зависят от V1 и перечитываются
    // после каждой его записи. Сверять с ними обязан вызывающий.
    outP1 = roundToI16( mv / stepMv );
    return true;
}


double triggerLevelMv( uint8_t s1, uint16_t v1, int divs ) {
    if ( divs <= 0 )
        return 0.0;
    const double screenMv = double( v1 ) * double( divs );
    const double vMin = -screenMv / 2.0;
    // Делитель 256 здесь — собственное квантование регистра S1
    // («1/256 диапазона АЦП», 0…255), а не число кодов на экран.
    // Поэтому `CodeSpan` в эту формулу НЕ входит: так объявлено
    // контрактом. Если измерение на приборе покажет рамку `Codes240`,
    // формула получит множитель 256/240 и подлежит пересмотру —
    // здесь это названо, а не подшито молча. [СБОРКА]
    return vMin + ( screenMv / 256.0 ) * double( s1 );
}


uint8_t s1FromTriggerLevelMv( double mv, uint16_t v1, int divs ) {
    const double screenMv = double( v1 ) * double( divs );
    if ( divs <= 0 || !( screenMv > 0.0 ) || !std::isfinite( mv ) )
        // Шкалы нет, а отказать нечем: подпись возвращает голый байт.
        // Центр — единственное защитимое значение: он не смещает порог
        // ни вверх, ни вниз. Величины из этого не следует, и вызывающий
        // обязан проверять шкалу до вызова.
        return 128;

    const double vMin = -screenMv / 2.0;
    // Вычитание vMin ОБЯЗАТЕЛЬНО. Эталонная реализация его пропускает,
    // и её круговой пересчёт не сходится: 0 мВ даёт S1 = 0 вместо 128,
    // то есть порог уезжает на край экрана вместо центра. [СБОРКА]
    double n = std::floor( ( mv - vMin ) / ( screenMv / 256.0 ) + 0.5 );
    if ( n < 0.0 )
        n = 0.0;
    if ( n > 255.0 )
        n = 255.0;
    return uint8_t( n );
}


double comparatorDelayPs( uint16_t d1m ) { return double( d1m ) * TICK_UNIT_PS; }


ChannelScale channelScale( uint16_t v1, int16_t p1, SampleFormat fmt, CodeSpan span, int divs ) {
    ChannelScale s;

    double codeMv = 0.0; // милливольт на один код АЦП (восьмибитный)
    if ( !codeStepMv( v1, span, divs, codeMv ) )
        // `known` остаётся ложным. Пересчёт кодов в напряжение запрещён
        // до тех пор, пока рамка не установлена измерением: величина без
        // единицы, происхождения и годности числом не показывается.
        return s;

    // Полный диапазон АЦП — всегда 256 кодов, сколько бы из них ни было
    // отведено экрану. При `Codes256` он совпадает с экраном; при
    // `Codes240` он в 256/240 раза шире — те самые «16 кодов запаса на
    // выбросы», из-за которых у первоисточника и появилась вторая
    // цифра 8,53 мВ.
    const double adcSpanMv = codeMv * 256.0;

    double offsetMv = 0.0;
    // Проверку уже прошли выше: та же рамка, те же аргументы.
    offsetMillivolts( p1, v1, span, offsetMv, divs );

    // Знак: окно АЦП движется ВМЕСТЕ со смещением — так его строит
    // эталонная реализация (vMax и vMin получают одну и ту же добавку).
    // На приборе не проверено; проверяется тем же единственным опытом,
    // что и рамка: подать известное напряжение и посмотреть, куда
    // уехал луч. [СБОРКА]
    s.minMv = offsetMv - adcSpanMv / 2.0;
    s.maxMv = offsetMv + adcSpanMv / 2.0;

    // Шаг кода ТЕКУЩЕГО формата: `AvgHiRes` кладёт на тот же размах
    // 65536 кодов вместо 256. Совпадает с эталонным
    // `vStep = (vMax − vMin) / (vRes + 1)`. [СБОРКА]
    s.stepMv = adcSpanMv / ( double( codeFullScale( fmt ) ) + 1.0 );
    s.known = true;
    return s;
}


bool codeToMillivolts( int code, const ChannelScale &scale, double &outMv ) {
    if ( !scale.known )
        return false;
    // Код вне 0…полная шкала не отсекается: при рамке `Codes240` коды
    // запаса лежат ЗА экраном и остаются законными выборками АЦП —
    // именно ими виден выброс. Отсечение здесь срезало бы то, ради чего
    // запас и заведён.
    outMv = scale.minMv + double( code ) * scale.stepMv;
    return true;
}


double sampleTimePs( int index, double samplePeriodPsValue, uint16_t tc ) {
    // t[i] = T_s × i − TC × T_s. Момент синхронизации приходится на
    // t = 0, TC — центровка в выборках.
    return samplePeriodPsValue * ( double( index ) - double( tc ) );
}

// ===========================================================================
// Кадр оцифровки
// ===========================================================================

const char *triggerSourceName( TriggerSource t ) {
    switch ( t ) {
    case TriggerSource::Timeout:
        // Не «ошибка» и не «нет данных»: оцифровка состоялась по
        // истечении TA, данные годны и просто не привязаны к событию
        // синхронизации.
        return "таймаут (не синхронизировано)";
    case TriggerSource::Fall:
        return "спад";
    case TriggerSource::Rise:
        return "фронт";
    case TriggerSource::Undefined:
        break;
    }
    return "не определён";
}


FrameLayout detectLayout( const QByteArray &body, unsigned channels ) {
    const int n = int( body.size() );

    // Каналов ноль — сравнивать нечего: число каналов берётся из
    // атрибутов оцифровки и меньше единицы там не бывает, а ноль сюда
    // приходит только от того, кто атрибутов ещё не разобрал.
    if ( channels == 0u )
        return FrameLayout::Unknown;

    // Меньше пяти байт: массива нет ни при одной раскладке (четыре
    // байта — это ровно заголовок Java-раскладки без единой выборки).
    // Опыт вопрос не разрешил.
    if ( n < 5 )
        return FrameLayout::Unknown;

    // Гипотеза «поле размера есть» проверяется проходом по ВСЕМУ телу:
    // разбор обязан закончиться ровно на его конце. Для одного канала
    // это в точности проверка «u16 big-endian по смещению 4 равен
    // n − 6», описанная как способ разрешения спора. [СБОРКА]
    bool sized = true;
    int pos = 2;
    for ( unsigned c = 0; c < channels; ++c ) {
        if ( pos + 4 > n ) {
            sized = false;
            break;
        }
        const int size = be16( body, pos + 2 );
        pos += 4 + size;
        if ( pos > n ) {
            sized = false;
            break;
        }
    }
    if ( sized && pos != n )
        sized = false;

    // Ловушка совпадения. Тело ровно в шесть байт при объявленном
    // нулевом размере сходится с обеими раскладками: одна видит пустой
    // массив, другая — массив из двух байт, и различить их нечем.
    // Опыт вопрос не разрешил — решает вызывающий и пишет решение в
    // журнал явно.
    if ( sized && channels == 1u && n == 6 )
        return FrameLayout::Unknown;

    if ( sized )
        return FrameLayout::WithSizeField;

    // Раскладка эталонной реализации на Java описывает РОВНО один
    // канал: размеров в ней нет, а значит нет и границ между каналами.
    // Для многоканального кадра она не «менее вероятна», а невозможна.
    return channels == 1u ? FrameLayout::WithoutSizeField : FrameLayout::Unknown;
}


bool parseFrame( const QByteArray &body, FrameLayout layout, Frame &out ) {
    out = Frame();
    out.layout = layout;

    const int n = int( body.size() );

    // П2. Пустое тело `0x49` — ШТАТНЫЙ ответ ждущего запуска, у которого
    // истёк TW: данных нет, связь цела, ошибки нет. Отличие от «данные
    // есть, но не синхронизированы» проходит здесь, по длине тела, а не
    // по битам 5-4 атрибутов.
    if ( n == 0 ) {
        out.valid = true;
        out.empty = true;
        return true;
    }

    // Атрибуты оцифровки — два байта, из которых значащ первый; второй
    // первоисточником зарезервирован («будет определён позднее») и не
    // читается. Один байт — это не кадр, а обрывок.
    if ( n < 2 )
        return false;

    const uint8_t a0 = u8( body, 0 );
    out.ris = ( a0 & 0x01 ) != 0;
    out.parallel = ( a0 & 0x02 ) != 0;
    out.roll = ( a0 & 0x04 ) != 0;
    out.trigger = TriggerSource( uint8_t( ( a0 >> 4 ) & 0x03 ) );
    // Число каналов — биты 7-6 ПЕРВОГО байта. Эталонная реализация на
    // Python берёт их сдвигом на 6 от шестнадцатибитного слова и
    // попадает в зарезервированный второй байт, отчего у неё всегда
    // получается один канал. Сдвиг здесь применён к байту, а не к слову.
    out.channels = unsigned( ( a0 >> 6 ) & 0x03 ) + 1u;

    // Атрибуты первого канала стоят по смещению 2 в ОБЕИХ раскладках —
    // поле размера, если оно есть, идёт ПОСЛЕ них. Значит формат канала
    // читается до того, как раскладка выбрана.
    if ( n < 4 )
        // Канал объявлен, а двух байт его атрибутов нет: тело само себе
        // противоречит. Атрибуты оцифровки при этом разобраны и остаются
        // в `out` — по ним видно, чей это был кадр.
        return false;

    if ( n == 4 ) {
        // Ровно заголовок и ни одной выборки. Так выглядит НАЧАЛО ленты
        // (roll): прибор передаёт заголовок и начинает оцифровку, а
        // данные идут следом пакетами Continue и атрибутов больше не
        // повторяют. Это законный кадр, а не обрубок. [СБОРКА]
        if ( out.channels != 1u )
            // Четырёх байт не хватит на атрибуты второго канала.
            return false;
        ChannelData ch;
        ch.formatKnown = decodeSampleFormat( u8( body, 2 ), ch.format );
        out.channel.push_back( ch );
        out.empty = true;
        out.valid = true;
        // Раскладка остаётся такой, какой пришла: различать её не на
        // чем, массива нет.
        return true;
    }

    if ( out.layout == FrameLayout::Unknown )
        out.layout = detectLayout( body, out.channels );

    int rawTotal = 0;      // байт массивов, а НЕ распакованных выборок
    bool consistent = true; // сошлось ли тело само с собой

    if ( out.layout == FrameLayout::WithoutSizeField ) {
        if ( out.channels != 1u )
            // Размеров в раскладке нет — границы между каналами
            // неизвестны. Резать наугад нельзя: получилось бы два
            // массива, ни один из которых не принадлежит своему каналу.
            return false;

        ChannelData ch;
        ch.formatKnown = decodeSampleFormat( u8( body, 2 ), ch.format );
        ch.raw = body.mid( 4 );
        if ( ch.formatKnown && !decodeSamples( ch.raw, ch.format, ch ) )
            consistent = false;
        rawTotal += int( ch.raw.size() );
        out.channel.push_back( ch );
    } else if ( out.layout == FrameLayout::WithSizeField ) {
        int pos = 2;
        for ( unsigned c = 0; c < out.channels; ++c ) {
            // Атрибуты канала (2 байта) плюс размер массива (2 байта).
            if ( pos + 4 > n ) {
                consistent = false;
                break;
            }
            ChannelData ch;
            ch.formatKnown = decodeSampleFormat( u8( body, pos ), ch.format );
            const int declared = be16( body, pos + 2 );
            pos += 4;

            // Заявленный размер больше пришедшего — обрыв передачи или
            // чужая раскладка. Берётся то, что ЕСТЬ, а не то, что
            // обещано: чтение за концом тела было бы выходом за буфер.
            // Кадр при этом объявляется несогласованным.
            const int avail = std::min( declared, n - pos );
            if ( avail < declared )
                consistent = false;

            ch.raw = body.mid( pos, avail );
            pos += avail;

            if ( ch.formatKnown && !decodeSamples( ch.raw, ch.format, ch ) )
                consistent = false;

            rawTotal += avail;
            out.channel.push_back( ch );
        }
        // Хвост, не принадлежащий ни одному каналу, означает, что
        // раскладка выбрана неверно. Молча его отбросить — значит
        // закрепить ошибку выбора на всё время сессии: раскладка
        // определяется один раз.
        if ( consistent && pos != n )
            consistent = false;
    } else {
        // Раскладка не определена опытом и не задана снаружи. Атрибуты
        // разобраны и остаются в `out` — по ним вызывающий примет
        // решение и запишет его в журнал.
        return false;
    }

    // «Пусто» считается по БАЙТАМ массивов, а не по распакованным
    // выборкам: у канала с неопознанным форматом байты пришли, а
    // выборок нет, и это третье состояние, отличное и от «данных нет»,
    // и от «данные есть». Складывать их в одно значило бы объявить
    // непонятый формат отсутствием сигнала. [СБОРКА]
    out.empty = ( rawTotal == 0 );
    out.valid = consistent;
    return consistent;
}


bool decodeSamples( const QByteArray &raw, SampleFormat f, ChannelData &out ) {
    out.sample.clear();
    out.sampleMax.clear();
    out.envelope = false;
    // Формат пришёл аргументом — значит он опознан вызывающим.
    // `out.raw` НЕ трогается: массив принадлежит тому, кто разбирал
    // кадр, и второго места, которое его порождает, здесь не заводится.
    out.format = f;
    out.formatKnown = true;

    const int n = int( raw.size() );
    if ( n == 0 )
        // Пустой массив согласуется с любым форматом: это «выборок нет»,
        // а не «разобрать не смогли».
        return true;

    const int step = sampleBytes( f );
    const int count = n / step;
    // Остаток означает недописанную выборку: массив нечётной длины в
    // двухбайтовом формате (`AvgHiRes`, `PeakPaired`). Распаковывается
    // всё целое, а лишний байт отбрасывается — но кадр объявляется
    // несогласованным. Досчитать половину выборки нулём было бы
    // выдумкой значения, которого прибор не присылал.
    const bool whole = ( n % step ) == 0;

    switch ( f ) {
    case SampleFormat::Avg:
    case SampleFormat::Normal:
    case SampleFormat::PeakInterlaced:
        // Один байт на выборку, беззнаковый. `PeakInterlaced` здесь же и
        // намеренно: «2 байта / 2 выборки» первоисточника означает, что
        // минимум и максимум стоят на СВОИХ соседних позициях оси
        // времени. Схлопывать их в пару нельзя — это выбросило бы
        // половину точек и вдвое исказило шаг по времени. Кому нужны
        // отдельные огибающие, тот берёт чётные и нечётные индексы,
        // помня, что их шаг вдвое крупнее шага выборок.
        out.sample.reserve( size_t( count ) );
        for ( int i = 0; i < count; ++i )
            out.sample.push_back( int( u8( raw, i ) ) );
        break;

    case SampleFormat::AvgHiRes:
        // Два байта на выборку, BIG-ENDIAN, беззнаковые 0…65535.
        out.sample.reserve( size_t( count ) );
        for ( int i = 0; i < count; ++i )
            out.sample.push_back( be16( raw, i * 2 ) );
        break;

    case SampleFormat::PeakPaired:
        // Пара байт на одну позицию времени: ПЕРВЫЙ — минимум, ВТОРОЙ —
        // максимум. Порядок дословно из первоисточника («нечётная
        // выборка пары – минимальное значение, чётная – максимальное»)
        // и совпадает с эталонной реализацией на Java. [СБОРКА]
        out.sample.reserve( size_t( count ) );
        out.sampleMax.reserve( size_t( count ) );
        for ( int i = 0; i < count; ++i ) {
            out.sample.push_back( int( u8( raw, i * 2 ) ) );
            out.sampleMax.push_back( int( u8( raw, i * 2 + 1 ) ) );
        }
        out.envelope = true;
        break;
    }

    return whole;
}

// ===========================================================================
// Ожидание и порядок записи
// ===========================================================================

double captureWindowPs( const CaptureTiming &t ) {
    // Бесконечно ждущий запуск предела не имеет вовсе: прибор вправе
    // молчать сколько угодно, и повторять запрос по истечении чего бы то
    // ни было здесь нельзя. Бесконечность — не «очень много», а
    // утверждение «величины нет». [СБОРКА]
    if ( t.rt == TriggerStart::WaitForever )
        return std::numeric_limits< double >::infinity();

    const double tick = tickPs( t.mc );
    if ( !( tick > 0.0 ) )
        // Такт не установлен — интервал не вычислим. Ноль означает
        // именно это; вызывающий обязан взять НАЗВАННУЮ величину
        // (`OscillTiming::SHORT_REPLY_MS`), а не считать, что ждать
        // нечего. Подставить сюда «ну секунду» значило бы завести
        // второе место, порождающее таймаут.
        return 0.0;

    // Ожидание синхронизации. При автозапуске предел — TA, при ждущем —
    // TW; свободный запуск не ждёт вовсе.
    double waitPs = 0.0;
    switch ( t.rt ) {
    case TriggerStart::Auto:
        waitPs = delayPs( t.ta, t.mc );
        break;
    case TriggerStart::WaitTimeout:
        waitPs = delayPs( t.tw, t.mc );
        break;
    case TriggerStart::Free:
        waitPs = 0.0;
        break;
    case TriggerStart::WaitForever:
        break; // отсечено выше
    }

    // Задержка развёртки отсчитывается ПОСЛЕ синхронизации, до начала
    // оцифровки, и потому складывается, а не выбирается.
    const double delay = delayPs( t.td, t.mc );

    // Собственно оцифровка: период выборки на их количество. QS объявлен
    // в выборках — на число байт он здесь не умножается, время не
    // зависит от того, сколько байт занимает выборка.
    const double digitize = samplePeriodPs( t.ts, t.mc ) * double( t.qs );

    return waitPs + delay + digitize;
}


const std::vector< Register > &initOrder() {
    // Порядок взят из рабочего эталонного кода. QS/TS/TC стоят
    // ПОСЛЕДНИМИ — на этом месте в эталоне стоит пометка «set last»:
    // они зависят почти от всего, и записанные раньше были бы подрезаны
    // последующими записями. После всего полагается калибровка 'C', а
    // после неё — перечитывание P1 и S1; это делает бэкенд, здесь
    // только порядок регистров. [СБОРКА]
    static const std::vector< Register > order = {
        Register::MC, Register::RS, Register::TD, Register::TA, Register::TW, Register::AR,
        Register::AP, Register::O1, Register::M1, Register::V1, Register::P1, Register::T1,
        Register::S1, Register::RT, Register::QS, Register::TS, Register::TC,
    };
    static_assert( size_t( Register::S1 ) + 1 == 17, "порядок записи разошёлся с перечислением" );
    return order;
}


std::vector< Register > dependentRegisters( Register r ) {
    // Граф собран по ОПИСАНИЯМ первоисточника, а не по эталонному коду:
    // там связи RS → {TD, M1} и TS → {RS, M1, TD} не заведены вовсе,
    // хотя документация их требует, и после записи RS или TS зависимые
    // регистры не перечитываются. Здесь они есть. [СБОРКА]
    switch ( r ) {
    case Register::MC:
        // Сырое значение TS от смены такта не меняется, а физический
        // период выборки меняется целиком: TS задан В ТАКТАХ. Прибор при
        // этом вправе подрезать TS по своим пределам — и единственный
        // способ узнать, подрезал ли, это спросить.
        return { Register::TS };
    case Register::TS:
        // Наивысший приоритет: TS ни от чего не зависит, а от него
        // зависит почти всё.
        return { Register::RS, Register::M1, Register::TD, Register::QS, Register::TC };
    case Register::RS:
        // Способ оцифровки решает, возможны ли многопроходность (AP) и
        // обработка в приборе (M1), и сдвигает границы TD, QS, TC.
        return { Register::M1, Register::AP, Register::TD, Register::QS, Register::TC };
    case Register::M1:
    case Register::AP:
        // Формат выборки и число проходов меняют размер, который
        // помещается в память прибора.
        return { Register::QS };
    case Register::QS:
        // Размер массива задаёт, куда можно поставить центровку и
        // задержку.
        return { Register::TC, Register::TD };
    case Register::O1:
        // Заземлённый вход отменяет чувствительность: прибор вправе
        // подрезать V1.
        return { Register::V1 };
    case Register::V1:
        // Смещение P1 и уровень синхронизации S1 выражены долями
        // диапазона АЦП, а его цена делится чувствительностью.
        return { Register::P1, Register::S1 };
    case Register::AR:
    case Register::TC:
    case Register::TD:
    case Register::RT:
    case Register::TA:
    case Register::TW:
    case Register::P1:
    case Register::T1:
    case Register::S1:
        // От RT зависит, какой предел ДЕЙСТВУЕТ (TA или TW), но не их
        // значения: перечитывать нечего.
        break;
    }
    return {};
}


std::vector< Property > dependentProperties( Register r ) {
    // Список ровно тот, что объявлен контрактом. Расширять его здесь
    // нельзя: заявленная первоисточником зависимость TPl от MC, числа
    // каналов и M1 в контракт не вошла, и завести её молча значило бы
    // развести объявление и реализацию одной и той же связи. [СБОРКА]
    switch ( r ) {
    case Register::V1:
        // Границы смещения сдвигаются при каждой смене чувствительности:
        // «рекомендуется запрашивать свойства после установки
        // чувствительности».
        return { Property::P1h, Property::P1l };
    case Register::RS:
    case Register::TS:
        return { Property::QSh, Property::TCh, Property::TDl, Property::TDh };
    case Register::M1:
    case Register::AP:
        return { Property::QSh, Property::TCh };
    case Register::QS:
        return { Property::TDl, Property::TDh };
    case Register::S1:
    case Register::T1:
        // Конструктивная задержка компаратора зависит от уровня и режима
        // синхронизации.
        return { Property::D1m };
    case Register::MC:
    case Register::AR:
    case Register::TC:
    case Register::TD:
    case Register::RT:
    case Register::TA:
    case Register::TW:
    case Register::O1:
    case Register::P1:
        break;
    }
    return {};
}

} // namespace Oscill
