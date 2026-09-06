// SPDX-License-Identifier: GPL-3.0-or-later

/// \file oscillbackend.cpp
/// \brief Реализация бэкенда Oscill: сессия OBEX поверх байтового потока.
///
/// ЧТО РЕШЕНО ЗДЕСЬ, А НЕ В ЗАГОЛОВКЕ
///
/// Заголовок задаёт договор, а договор не отвечает на вопрос «что делать
/// с тем, чего он не предусмотрел». Таких мест ровно шесть, и все они
/// решены В СТОРОНУ ОТКАЗА С НАЗВАННОЙ ПРИЧИНОЙ, а не догадки. Прибора в
/// работе нет, и молчаливая догадка здесь неотличима от работающего кода
/// ровно до того дня, когда прибор появится.
///
///   1. **Оснастку опознаёт язык, а не шина.** Последовательному порту
///      нужно то, чего в договоре транспорта нет: проверка существования
///      порта без открытия, вычистка буфера, чтение ровно N байт со своим
///      сроком, смена скорости. Взять их можно только у `SerialTransport`,
///      а опознать его — только приведением типа. Приведение по `bus()`
///      было бы неверным: `Bus::Serial` объявляет о себе и поддельная
///      оснастка тестов, а `static_cast` на неё есть неопределённое
///      поведение. Поэтому `dynamic_cast`, а при сборке без RTTI (RELEASE
///      собирается с `-fno-rtti`) — честный отказ опознания: часть,
///      которой нужен именно порт, отвечает «оснастка не опознана» и
///      называет причину. Байтовая часть (`write`/`read`) работает и там,
///      поэтому бэкенд остаётся проверяемым против поддельной оснастки.
///   2. **Идентификатор соединения `0xCB` в запросы не дописывается.**
///      Прибор мог назвать его в ответе на CONNECT — тогда он запомнен и
///      виден снаружи. Но перечень поддерживаемых прибором заголовков его
///      не содержит, сборщики запросов в `oscillprotocol.h` его не
///      предусматривают, а дописать заголовок к ГОТОВОМУ пакету нельзя:
///      пришлось бы пересчитывать длину и сумму, то есть завести второе
///      место, которое собирает пакет. Если прибор его потребует, это
///      будет видно ответом `0xC0` на первый же запрос — и лечиться
///      правкой сборщиков, а не дописыванием байт.
///   3. **Ожидание меряется вычисленным интервалом, а не сном.** Сколько
///      прибор вправе молчать после `'D'`, считает
///      `Oscill::captureWindowPs()` из регистров. К нему прибавляется
///      названный срок ответа на короткий запрос: окно говорит, сколько
///      прибор ОЦИФРОВЫВАЕТ, и ничего не говорит о том, за сколько он
///      соберётся ответить, — прогон против поддельного прибора показал,
///      что без этого слагаемого клиент объявляет молчанием ответ,
///      которого ещё не начали слать. При бесконечно ждущем запуске
///      предела нет вовсе — тогда шаг только заглядывает в буфер и НЕ
///      считает молчание отказом: повторять запрос там не по чему.
///   4. **Недочитанных байт шаг не хранит.** В договоре нет поля под
///      приёмный буфер, а заводить его — менять договор, по которому
///      сейчас пишут остальные части. Поэтому пакет забирается ЦЕЛИКОМ в
///      том вызове, в котором пришёл его первый байт: первоисточник
///      оснастки говорит, что пакет OBEX приходит одной очередью, и
///      молчание дольше межбайтового срока означает конец очереди, а не
///      медленную линию. Незавершённый пакет — это `lengthMismatch` и
///      перезапрос `0x92`, а не сохранённый огрызок.
///   5. **Хвост ленты живёт в сыром массиве своего канала.** В roll
///      порция может кончиться посередине двухбайтовой выборки. Досчитать
///      её нулём — выдумать значение; выбросить — сдвинуть все
///      последующие. Остаток остаётся в `frame().channel[0].raw`, то есть
///      ровно там, где и объявлен «массив как пришёл, до распаковки».
///      Второго места под него не заводится.
///   6. **Счётчик повторов — это `Step`, а не отдельное поле.** Договор
///      разрешает ровно один перезапрос (`OscillTiming::REPEAT_LIMIT`), и
///      состояние `Step::Recovering` само по себе означает «повтор уже
///      был». Отдельный счётчик был бы вторым местом, порождающим ту же
///      величину.
///
/// СОСТОЯНИЕ УТВЕРЖДЕНИЙ. Прибора Oscill в работе нет: всё здесь —
/// **[СБОРКА]** (проверяется сборкой и прогоном против поддельной
/// оснастки) либо **[ПЛАН]** там, где кода ещё не существует — таков
/// `makeOscillTransport()`, которому неоткуда взять порт слота. Ни одно
/// число не перенесено со стеков ISDS205: путь A (`vdso.dll`) и путь B
/// (fx2lafw + libusb) к Oscill отношения не имеют.

#include "oscillbackend.h"
#include "../instrumentregistry.h"

#include <QDebug>
#include <QStringList>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Instrument {

namespace {

// ---------------------------------------------------------------------------
// Опознание оснастки
// ---------------------------------------------------------------------------

/// Есть ли в этой сборке информация о типах времени выполнения. Проверяются
/// все три признака, потому что макрос у каждого компилятора свой:
/// `__cpp_rtti` — стандартный признак возможности языка (C++17),
/// `__GXX_RTTI` — GCC и Clang, `_CPPRTTI` — MSVC. Приложение в
/// конфигурации RELEASE собирается с `-fno-rtti`, и `dynamic_cast` там не
/// компилируется вовсе — не отказывает в работе, а именно не собирается.
/// Поэтому признак проверяется препроцессором, а не условием. [СБОРКА]
#if defined( __cpp_rtti ) || defined( __GXX_RTTI ) || defined( _CPPRTTI )
#define OSCILL_HAS_RTTI 1
#else
#define OSCILL_HAS_RTTI 0
#endif

/// Оснастка как последовательный порт — или `nullptr`, если это не он.
///
/// Сравнение по `bus()` здесь было бы ошибкой: `Bus::Serial` объявляет о
/// себе всякая байтовая оснастка, включая поддельную, а `static_cast` на
/// чужой объект есть неопределённое поведение — то самое «наверное
/// сработает», которого в коде без прибора быть не должно.
SerialTransport *asSerial( Transport *t ) {
#if OSCILL_HAS_RTTI
    return dynamic_cast< SerialTransport * >( t );
#else
    // Сборка без RTTI. Опознать оснастку нечем, и притвориться, что она
    // последовательная, нельзя: вызывающий получит честный `nullptr` и
    // назовёт причину пользователю.
    ( void )t;
    return nullptr;
#endif
}

// ---------------------------------------------------------------------------
// Чтение байт
// ---------------------------------------------------------------------------

/// Чем кончилась попытка забрать пакет. Три исхода, а не два: правило
/// первоисточника различает «ноль байт» и «хотя бы один байт», и на этом
/// различии стоит вся политика повторов — ноль означает отсутствие
/// ответа, один байт означает ошибочный ответ.
enum class Intake {
    Silence,  ///< не пришло ни одного байта
    Fragment, ///< байты есть, целого пакета из них не вышло
    Packet,   ///< пакет длиной ровно как объявлено
};

/// Прочитать ровно `count` байт, ожидая не дольше `timeoutMs`.
///
/// У последовательного порта для этого есть `readExactly()` с его
/// собственными сроками. У обобщённой оснастки сроков в договоре нет
/// намеренно, и подменять их сном здесь нельзя: сон — это выдуманное
/// время там, где вызывающий про время ничего не обещал. Поэтому обобщённая
/// ветка читает, пока оснастка отдаёт байты, и останавливается на первом
/// пустом ответе. Цикл конечен по построению: каждый его проход обязан
/// добавить хотя бы один байт.
QByteArray readRaw( Transport *t, SerialTransport *serial, int count, int timeoutMs ) {
    if ( !t || count <= 0 )
        return QByteArray();
    if ( serial )
        return serial->readExactly( count, timeoutMs );

    QByteArray got;
    while ( got.size() < count ) {
        const QByteArray chunk = t->read( count - got.size() );
        if ( chunk.isEmpty() )
            break;
        got.append( chunk );
    }
    return got;
}

/// Забрать один пакет: три байта заголовка, затем объявленный им хвост.
///
/// Первого байта ждём `firstWaitMs` — столько прибор вправе молчать.
/// Хвост ждём `tailWaitMs`: пакет OBEX приходит одной очередью, и молчание
/// внутри него означает обрыв, а не медленную линию.
Intake takePacket( Transport *t, SerialTransport *serial, int firstWaitMs, int tailWaitMs,
                   QByteArray &packet ) {
    packet.clear();
    if ( !t )
        return Intake::Silence;

    const QByteArray head = readRaw( t, serial, Oscill::BASE_PACKET_LENGTH, firstWaitMs );
    if ( head.isEmpty() )
        return Intake::Silence;
    if ( head.size() < Oscill::BASE_PACKET_LENGTH ) {
        packet = head;
        return Intake::Fragment;
    }

    const int declared = Oscill::declaredLength( head );
    if ( declared < Oscill::BASE_PACKET_LENGTH ) {
        // Объявленная длина меньше собственного заголовка пакета: сколько
        // ни жди, целым он не станет. Это порча, и лечится она
        // перезапросом, а не ожиданием.
        packet = head;
        return Intake::Fragment;
    }

    packet = head;
    const int tail = declared - Oscill::BASE_PACKET_LENGTH;
    if ( tail > 0 ) {
        const QByteArray rest = readRaw( t, serial, tail, tailWaitMs );
        packet.append( rest );
        if ( rest.size() < tail )
            return Intake::Fragment;
    }
    return Intake::Packet;
}

/// Выбросить всё, что накопилось в приёмном буфере.
///
/// У порта для этого есть своё средство. У обобщённой оснастки его нет, и
/// вычитка ограничена объёмом, который мы САМИ объявили прибору в CONNECT:
/// больше этого в буфере лежать не должно, а если лежит — линия несёт не
/// наш обмен, и вычитывать её бесконечно значило бы висеть в разборе мусора.
void drainInput( Transport *t, SerialTransport *serial ) {
    if ( !t )
        return;
    if ( serial ) {
        serial->purge();
        return;
    }
    int dropped = 0;
    while ( dropped < int( Oscill::CLIENT_RX_BUFFER ) ) {
        const QByteArray chunk = t->read( int( Oscill::CLIENT_RX_BUFFER ) - dropped );
        if ( chunk.isEmpty() )
            break;
        dropped += chunk.size();
    }
}

// ---------------------------------------------------------------------------
// Настройки: регистр ↔ поле
// ---------------------------------------------------------------------------

/// Желаемое значение регистра из настроек. Соответствие «регистр ↔ поле»
/// объявлено ровно здесь и в `storeActual()`; третьего места, где оно
/// повторяется, нет.
uint32_t wantedOf( const OscillSettings &s, Oscill::Register r ) {
    switch ( r ) {
    case Oscill::Register::MC:
        return s.mc;
    case Oscill::Register::TS:
        return s.ts;
    case Oscill::Register::RS:
        return s.rs;
    case Oscill::Register::AP:
        return s.ap;
    case Oscill::Register::AR:
        return s.ar;
    case Oscill::Register::QS:
        return s.qs;
    case Oscill::Register::TC:
        return s.tc;
    case Oscill::Register::TD:
        return s.td;
    case Oscill::Register::RT:
        return s.rt;
    case Oscill::Register::TA:
        return s.ta;
    case Oscill::Register::TW:
        return s.tw;
    case Oscill::Register::O1:
        return s.o1;
    case Oscill::Register::V1:
        return s.v1;
    case Oscill::Register::P1:
        // Единственный знаковый регистр прибора. На провод он уходит
        // дополнительным кодом ШИРИНЫ РЕГИСТРА: расширение знака до 32 бит
        // сборщик обрежет маской, но обрезать он должен уже готовое
        // двухбайтовое представление, а не случайные старшие биты.
        return uint32_t( uint16_t( s.p1 ) );
    case Oscill::Register::M1:
        return s.m1;
    case Oscill::Register::T1:
        return s.t1;
    case Oscill::Register::S1:
        return s.s1;
    }
    return 0;
}

/// Положить ФАКТ, прочитанный у прибора, в настройки. Именно факт: то, что
/// просили, в `OscillSettings` не попадает никогда (П3).
void storeActual( OscillSettings &s, Oscill::Register r, uint32_t v ) {
    switch ( r ) {
    case Oscill::Register::MC:
        s.mc = uint16_t( v );
        break;
    case Oscill::Register::TS:
        s.ts = v;
        break;
    case Oscill::Register::RS:
        s.rs = uint8_t( v );
        break;
    case Oscill::Register::AP:
        s.ap = uint8_t( v );
        break;
    case Oscill::Register::AR:
        s.ar = uint8_t( v );
        break;
    case Oscill::Register::QS:
        s.qs = uint16_t( v );
        break;
    case Oscill::Register::TC:
        s.tc = uint16_t( v );
        break;
    case Oscill::Register::TD:
        s.td = v;
        break;
    case Oscill::Register::RT:
        s.rt = uint8_t( v );
        break;
    case Oscill::Register::TA:
        s.ta = v;
        break;
    case Oscill::Register::TW:
        s.tw = v;
        break;
    case Oscill::Register::O1:
        s.o1 = uint8_t( v );
        break;
    case Oscill::Register::V1:
        s.v1 = uint16_t( v );
        break;
    case Oscill::Register::P1:
        // Разворот дополнительного кода делается ровно один раз и здесь:
        // в настройках P1 хранится числом со знаком, а не сырыми битами.
        s.p1 = Oscill::signed16( v );
        break;
    case Oscill::Register::M1:
        s.m1 = uint8_t( v );
        break;
    case Oscill::Register::T1:
        s.t1 = uint8_t( v );
        break;
    case Oscill::Register::S1:
        s.s1 = uint8_t( v );
        break;
    }
}

// ---------------------------------------------------------------------------
// Лента
// ---------------------------------------------------------------------------

/// Выбрать из сырого массива канала все ЦЕЛЫЕ коды, оставив в нём хвост
/// незавершённого.
///
/// Коды выдаются в том порядке, в каком лежат в массиве: у пикового
/// повыборочного формата это пары «минимум, максимум», у прочих — просто
/// выборки. Схлопывать пары здесь нельзя — договор отдаёт наружу ОДИН
/// вектор, а формат порции вызывающий берёт из `frame()`, где он и
/// объявлен. Ширина одного кода — `sampleWordBytes()`: два байта только у
/// формата повышенного разрешения, у пикового повыборочного поток читается
/// побайтно (величины разные, и путать их нельзя). [СБОРКА]
void harvestRoll( Oscill::Frame &frame, std::vector< int > &out ) {
    if ( frame.channel.empty() )
        return;
    Oscill::ChannelData &ch = frame.channel.front();
    if ( !ch.formatKnown )
        // Формат не опознан — байты есть, а кодов нет. Выдумывать
        // раскладку по умолчанию значило бы объявить непонятое обычным.
        return;

    const int word = Oscill::sampleWordBytes( ch.format );
    if ( word <= 0 )
        return;
    const int whole = ( int( ch.raw.size() ) / word ) * word;
    if ( whole <= 0 )
        return;

    out.reserve( out.size() + std::size_t( whole / word ) );
    for ( int i = 0; i < whole; i += word ) {
        if ( word == 2 )
            out.push_back( ( int( uint8_t( ch.raw.at( i ) ) ) << 8 ) |
                           int( uint8_t( ch.raw.at( i + 1 ) ) ) );
        else
            out.push_back( int( uint8_t( ch.raw.at( i ) ) ) );
    }

    // Хвост остаётся ровно там, где лежал: у своего канала. Он и есть
    // «массив как пришёл, до распаковки» — начало следующей выборки.
    ch.raw = ch.raw.mid( whole );
    // Распакованных выборок у кадра ленты нет: коды ушли вызывающему.
    ch.sample.clear();
    ch.sampleMax.clear();
    ch.envelope = false;
}

} // namespace

// ===========================================================================
// Паспорт
// ===========================================================================

bool OscillPassport::sensitivityRange( uint16_t &minMvPerDiv, uint16_t &maxMvPerDiv ) const {
    if ( !sensitivityCoarse.known || !sensitivityFine.known )
        // Границы без обеих величин не существует. Подставить сюда
        // прочитанную половину значило бы объявить одну границу обеими.
        return false;

    // Порядок восстанавливается ПО ЗНАЧЕНИЮ, а не по именам. Имена
    // обманывают: `V1h` = 10000 мВ/дел (10 В/дел, низшая чувствительность),
    // `V1l` = 20 мВ/дел (наивысшая). Эталонная реализация построила
    // интервал прямо по именам, получила [10000; 20] и её общий отсекатель
    // поднимает любую чувствительность до 10 В/дел. [СБОРКА]
    minMvPerDiv = std::min( sensitivityCoarse.value, sensitivityFine.value );
    maxMvPerDiv = std::max( sensitivityCoarse.value, sensitivityFine.value );
    return true;
}

// ===========================================================================
// Жизненный цикл
// ===========================================================================

OscillBackend::OscillBackend( TransportPtr transport ) : Backend( std::move( transport ) ) {
    m_state.model = QStringLiteral( "Oscill" );
    if ( m_transport )
        m_state.transport = m_transport->description();
}


OscillBackend::~OscillBackend() {
    // Порт закрывается явно: разрушение объекта с открытым портом
    // оставляет его занятым до перезапуска приложения.
    unlink();
}

// ===========================================================================
// Instrument::Backend
// ===========================================================================

bool OscillBackend::probe() {
    m_state.present = false;

    SerialTransport *serial = asSerial( m_transport.get() );
    if ( !serial ) {
        // Две разные причины, и обе называются вслух: оснастки нет вовсе
        // либо она не последовательный порт (в том числе потому, что
        // сборка идёт без RTTI и опознать её нечем).
        setError( m_transport ? QStringLiteral( "оснастка не опознана как последовательный порт: "
                                                "проверить наличие порта нечем" )
                              : QStringLiteral( "оснастка слота не задана" ) );
        return false;
    }

    const QString port = serial->params().port;
    if ( port.isEmpty() ) {
        setError( QStringLiteral( "порт слота не задан" ) );
        return false;
    }

    // Порт НЕ открывается. Открыть чужой занятый COM-порт значило бы
    // вмешаться в работу другого прибора, а `probe()` ничего менять не
    // вправе.
    m_state.present = SerialTransport::exists( port );
    if ( !m_state.present ) {
        setError( QStringLiteral( "порта %1 в системе нет" ).arg( port ) );
        return false;
    }
    m_state.lastError.clear();
    return true;
}


bool OscillBackend::link() {
    unlink(); // повторная связь начинается с чистого состояния
    m_state.lastError.clear();
    // Имя модели возвращается к родовому: опознание прибора ещё впереди, и
    // держать здесь идентификатор прошлой сессии значило бы показывать
    // прежний прибор на месте нового.
    m_state.model = QStringLiteral( "Oscill" );

    if ( !m_transport ) {
        setError( QStringLiteral( "оснастка слота не задана" ) );
        return false;
    }
    if ( !m_transport->isOpen() && !m_transport->open() ) {
        const QString why = m_transport->lastError();
        setError( why.isEmpty() ? QStringLiteral( "порт не открылся" )
                                : QStringLiteral( "порт не открылся: %1" ).arg( why ) );
        return false;
    }
    m_state.transport = m_transport->description();

    SerialTransport *serial = asSerial( m_transport.get() );

    // Сброс. Обе эталонные реализации перед CONNECT шлют голый Abort и
    // ждут: прибор в этот момент может дописывать хвост прошлой сессии, и
    // этот хвост нельзя принять за начало ответа на CONNECT.
    if ( !sendPacket( Oscill::makeAbort() ) ) {
        m_transport->close();
        return false;
    }
    QThread::msleep( OscillTiming::RESET_SETTLE_MS );
    drainInput( m_transport.get(), serial );

    // CONNECT. Разбирается отдельной функцией кодека: перед заголовками
    // здесь стоят четыре поля пакета, и проход от третьего байта принял бы
    // номер версии за идентификатор заголовка. Поэтому и `transact()` тут
    // не годится — он собирает обычный ответ.
    //
    // Политика повторов та же, что у прочих запросов (П8), и разделена
    // так же: молчание лечится повтором ЗАПРОСА, порча — перезапросом
    // ОТВЕТА (`0x92`). Каждое — ровно один раз.
    const QByteArray connect = Oscill::makeConnect( Oscill::CLIENT_RX_BUFFER, m_checksum );
    Oscill::ConnectInfo info;
    bool connected = false;
    int requestRepeats = 0;
    int answerRepeats = 0;

    if ( sendPacket( connect ) ) {
        for ( ;; ) {
            QByteArray packet;
            const Intake in = takePacket( m_transport.get(), serial, OscillTiming::SHORT_REPLY_MS,
                                          OscillTiming::CONTINUE_REPLY_MS, packet );

            if ( in == Intake::Silence ) {
                // Ноль байт есть отсутствие ответа: перезапрашивать
                // нечего, повторяется сам запрос.
                ++m_stats.silentIntervals;
                if ( requestRepeats >= OscillTiming::REPEAT_LIMIT || !sendPacket( connect ) )
                    break;
                ++requestRepeats;
                continue;
            }

            Oscill::ParseError err = Oscill::ParseError::None;
            if ( in == Intake::Fragment ) {
                // Хотя бы один байт есть — ответ был, но пришёл битым.
                err = Oscill::ParseError::LengthMismatch;
            } else {
                err = Oscill::parseConnectResponse( packet, info );
            }

            if ( err == Oscill::ParseError::NotConnect ) {
                // Прибор ответил, и ответил отказом. Порчи линии тут нет,
                // и повторять нечего: причина в запросе или в приборе.
                setError( QStringLiteral( "прибор отклонил CONNECT: код ответа 0x%1" )
                              .arg( info.response.code, 2, 16, QLatin1Char( '0' ) ) );
                m_transport->close();
                return false;
            }
            if ( err == Oscill::ParseError::None ) {
                connected = true;
                break;
            }

            if ( err == Oscill::ParseError::BadChecksum )
                ++m_stats.badChecksum;
            else
                ++m_stats.lengthMismatch;

            if ( answerRepeats >= OscillTiming::REPEAT_LIMIT )
                break;
            // Хвост испорченного ответа выбрасывается до перезапроса:
            // иначе он встанет впереди повторённого и испортит уже его.
            drainInput( m_transport.get(), serial );
            ++answerRepeats;
            ++m_stats.repeatsSent;
            if ( !sendPacket( Oscill::makeRepeatLast( m_checksum ) ) )
                break;
        }
    }

    if ( !connected ) {
        setError( QStringLiteral( "прибор не ответил на CONNECT либо ответ не разобран" ) );
        m_transport->close();
        return false;
    }

    // Приёмный буфер прибора. Пока он не назван, исходящее режется по
    // `SAFE_TX_LIMIT` — этот предел действует всегда, независимо от того,
    // что прибор объявил.
    m_deviceRx = Oscill::Known< uint16_t >( info.deviceRxBuffer, info.deviceRxKnown );
    m_hasConnectionId = info.hasConnectionId;
    m_connectionId = info.connectionId;
    if ( m_hasConnectionId )
        // Запомнен и показан. В запросы НЕ добавляется: см. пункт 2 шапки.
        qDebug() << "Oscill: прибор назвал идентификатор соединения" << m_connectionId
                 << "- в запросы он не добавляется";

    m_state.linked = true;
    m_state.present = true;
    m_step = Step::Idle;

    // Паспорт и фактические значения регистров. Отказ прибора отвечать на
    // отдельное свойство связь не рвёт: `0xD1` — это «такого свойства
    // нет», состояние прибора, а не отказ линии.
    readPassport();

    for ( Oscill::Register r : Oscill::allRegisters() ) {
        uint32_t value = 0;
        if ( readRegister( r, value ) )
            storeActual( m_settings, r, value );
    }

    // `firmware` собирается только из прочитанного. Пустая строка честнее
    // выдуманной версии: по ней видно, что прибор о себе не сказал.
    QStringList marks;
    if ( !m_passport.software.isEmpty() )
        marks << QStringLiteral( "ПО %1" ).arg( m_passport.software );
    if ( !m_passport.hardware.isEmpty() )
        marks << QStringLiteral( "аппаратура %1" ).arg( m_passport.hardware );
    m_state.firmware = marks.join( QStringLiteral( ", " ) );
    if ( !m_passport.deviceId.isEmpty() )
        m_state.model = QStringLiteral( "Oscill %1" ).arg( m_passport.deviceId );

    return true;
}


void OscillBackend::unlink() {
    if ( m_transport && m_transport->isOpen() ) {
        SerialTransport *serial = asSerial( m_transport.get() );

        // Лента прекращается ТОЛЬКО по требованию хоста. Перестать читать
        // недостаточно: прибор продолжит говорить, и порт останется забит
        // недосказанным ответом — следующая сессия начнётся с чужих байт.
        if ( m_step == Step::Rolling || m_step == Step::Assembling ) {
            sendPacket( Oscill::makeAbort() );
            QThread::msleep( OscillTiming::RESET_SETTLE_MS );
            drainInput( m_transport.get(), serial );
        }

        // Disconnect отправляется «как получится»: связь всё равно
        // разрывается, и отказ записи здесь ничего не меняет.
        sendPacket( Oscill::makeDisconnect() );
        m_transport->close();
    }

    m_state.linked = false;
    m_step = Step::Offline;
    m_assembler.reset();
    m_rollSamples.clear();
    m_deviceRx.forget();
    m_hasConnectionId = false;
    m_connectionId = 0;

    // Паспорт и фактические значения регистров после разрыва неизвестны:
    // прибор мог быть выключен, заменён или перенастроен другим клиентом.
    // Показывать вчерашние числа как сегодняшние — то же враньё об
    // измерении, что и молчаливая подрезка предела.
    m_passport = OscillPassport();
    m_settings = OscillSettings();
    m_mismatches.clear();

    // Последний кадр НЕ стирается: он уже отдан наверх, и обнулять его
    // значило бы гасить показанное вместе со связью. Его возраст виден по
    // `frameSerial()`, который не растёт.
}


void OscillBackend::update() {
    if ( !m_state.linked ) {
        m_step = Step::Offline;
        return;
    }
    if ( m_step == Step::Rolling )
        // Ленту прибор гонит сам. Опрашивать его нельзя: `flow().pace`
        // здесь `Pace::Stream`, и общий обход этот слот не трогает вовсе.
        return;
    if ( m_step == Step::Offline )
        m_step = Step::Idle;

    if ( m_step == Step::Idle ) {
        if ( Oscill::decodeRs( m_settings.rs ).isRoll() ) {
            // Прибор настроен на бесконечную оцифровку: команда 'D'
            // запустила бы ленту, которую этот шаг не умеет забирать.
            // Лента запускается только `startRoll()`.
            setError( QStringLiteral( "RS задаёт бесконечную оцифровку: кадр по запросу не берётся, "
                                      "лента запускается startRoll()" ) );
            return;
        }
        const QByteArray request = Oscill::makeDigitize( m_checksum );
        if ( request.isEmpty() ) {
            setError( QStringLiteral( "команда оцифровки не собралась" ) );
            return;
        }
        m_assembler.reset();
        if ( !sendPacket( request ) )
            return; // причина уже названа
        m_step = Step::Requested;
        return;
    }

    // Сюда шаг попадает в состояниях `Requested` и `Recovering`: команда
    // отдана, ждём первого байта ответа. Ожидание меряется ВЫЧИСЛЕННЫМ
    // защитным интервалом — тем, сколько прибор вправе молчать при
    // нынешних регистрах, — а не сроком «на всякий случай».
    const LinkStats before = m_stats;
    const int wait = guardMs();

    Oscill::Response r;
    if ( !receiveResponse( r, wait ) ) {
        const bool silent = m_stats.silentIntervals > before.silentIntervals;

        if ( silent ) {
            if ( Oscill::decodeRt( m_settings.rt ) == Oscill::TriggerStart::WaitForever )
                // Бесконечно ждущий запуск: предела молчанию нет, повторять
                // нечего. Шаг просто заглянул в буфер и вышел.
                return;
            if ( m_step == Step::Recovering ) {
                setError( QStringLiteral( "прибор молчит дольше защитного интервала (%1 мс) и после "
                                          "повтора запроса" )
                              .arg( wait ) );
                // Запрос брошен — значит ответ на него, если он всё-таки
                // придёт, ничей. Не выбросить его значило бы отдать чужие
                // байты следующему запросу и разбирать их как его ответ.
                drainInput( m_transport.get(), asSerial( m_transport.get() ) );
                m_assembler.reset();
                m_step = Step::Idle;
                return;
            }
            // Повтор запроса — только при ПОЛНОМ отсутствии байт и только
            // по истечении защитного интервала. Он только что истёк внутри
            // `receiveResponse()`. Повторяется именно `'D'`: продолжение
            // длинного ответа этот шаг не ждёт — его добирает
            // `collectBody()` в том же вызове, где начался ответ.
            if ( sendPacket( Oscill::makeDigitize( m_checksum ) ) )
                m_step = Step::Recovering;
            return;
        }

        // Байты были, а пакета не вышло: сумма не сошлась либо длина
        // разошлась с объявленной. Первоисточник знает на это ровно одно
        // средство — перезапрос последнего ответа, однократный.
        if ( m_step == Step::Recovering ) {
            setError( QStringLiteral( "ответ прибора не разобран и после перезапроса" ) );
            drainInput( m_transport.get(), asSerial( m_transport.get() ) );
            m_assembler.reset();
            m_step = Step::Idle;
            return;
        }
        drainInput( m_transport.get(), asSerial( m_transport.get() ) );
        ++m_stats.repeatsSent;
        if ( sendPacket( Oscill::makeRepeatLast( m_checksum ) ) )
            m_step = Step::Recovering;
        return;
    }

    // --- ответ разобран ---

    if ( r.code == uint8_t( Oscill::Rsp::InternalError ) ) {
        // Прибор увидел искажение ЗАПРОСА. Повторяется запрос, а не ответ,
        // и тоже ровно один раз.
        if ( m_step == Step::Recovering ) {
            setError( QStringLiteral( "прибор дважды не понял запрос оцифровки" ) );
            m_assembler.reset();
            m_step = Step::Idle;
            return;
        }
        if ( sendPacket( Oscill::makeDigitize( m_checksum ) ) )
            m_step = Step::Recovering;
        return;
    }
    if ( r.code == uint8_t( Oscill::Rsp::NotImplemented ) ) {
        ++m_stats.notImplemented;
        setError( QStringLiteral( "прибор не знает команды оцифровки 'D'" ) );
        m_step = Step::Idle;
        return;
    }
    if ( r.code == uint8_t( Oscill::Rsp::BadRequest ) ) {
        setError( QStringLiteral( "прибор не понял запрос оцифровки" ) );
        m_step = Step::Idle;
        return;
    }
    if ( r.code == uint8_t( Oscill::Rsp::NoContent ) ) {
        // «Данных не найдено» — ответ прибора, а не отказ линии. Кадра нет,
        // ошибки нет, `lastError` не трогается.
        m_assembler.reset();
        m_step = Step::Idle;
        return;
    }

    // Тело кадра. Дорогое ожидание — синхронизация, задержка развёртки и
    // сама оцифровка — уже позади: оно и было тем, что шаг ждал в
    // состоянии `Requested`. Остаток кадра прибор просто выдаёт из своей
    // памяти порциями, и растягивать их приём на обходы вредно: при
    // приёмном буфере в 4096 байт кадру в сто тысяч выборок понадобилось
    // бы столько же обходов, сколько порций, и объявленная `flow()`
    // частота кадров стала бы враньём. Поэтому порции добираются здесь же,
    // одним заходом, с пределом длины из паспорта прибора.
    if ( r.isContinue() )
        m_step = Step::Assembling;

    QByteArray body;
    if ( !collectBody( r, body, OscillTiming::CONTINUE_REPLY_MS ) ) {
        // Причина уже названа внутри.
        m_step = Step::Idle;
        return;
    }
    m_step = Step::Idle;

    Oscill::Frame frame;
    const bool ok = Oscill::parseFrame( body, m_layout, frame );

    if ( m_layout == Oscill::FrameLayout::Unknown && frame.layout != Oscill::FrameLayout::Unknown ) {
        // Раскладка определяется ОДИН раз, опытом, и печатается в журнал:
        // спор «есть поле размера или нет» бумагой не закрывается, и то,
        // чем он закрылся на этом приборе, обязано быть видно. [СБОРКА]
        m_layout = frame.layout;
        qDebug() << "Oscill: раскладка кадра определена опытом:"
                 << ( m_layout == Oscill::FrameLayout::WithSizeField ? "с полем размера массива"
                                                                     : "без поля размера массива" );
    }

    if ( !ok ) {
        // Кадр публикуется даже несогласованным: его атрибуты — сведения о
        // том, ЧЕЙ это был кадр (П1), и выбрасывать их вместе с телом
        // нельзя. Но `valid == false`, номер кадра не растёт, и причина
        // названа: несогласованное показывают как несогласованное.
        m_frame = frame;
        setError( frame.layout == Oscill::FrameLayout::Unknown
                      ? QStringLiteral( "раскладка кадра не определилась опытом: тело %1 байт" )
                            .arg( body.size() )
                      : QStringLiteral( "кадр не сошёлся сам с собой: тело %1 байт" ).arg( body.size() ) );
        return;
    }

    m_frame = frame;
    ++m_frameSerial;
    ++m_stats.framesReceived;

    // Кадр пришёл — значит нынешней беды нет. `lastError` описывает
    // СОСТОЯНИЕ, а не историю: оставить в нём прошлое молчание значило бы
    // показывать отказ на здоровой линии, пока приложение работает. То,
    // что беды БЫЛИ, помнят счётчики `LinkStats` — они для того и заведены.
    m_state.lastError.clear();

    // П2. Оба исхода — валидные результаты, и ни один из них НЕ ставит
    // `lastError`: «нет сигнала на входе» не есть «прибор отвалился».
    if ( m_frame.empty )
        ++m_stats.framesEmpty; // ждущий запуск не дождался: пустое тело 0x49
    else if ( !m_frame.synchronized() )
        ++m_stats.framesUnsynchronized; // запуск по таймауту: данные годны
}


Flow OscillBackend::flow() const {
    Flow f;

    // Темп считается из ТЕКУЩЕГО RS, а не берётся константой. В
    // бесконечной оцифровке прибор говорит сам, пакетами Continue до
    // Abort, — опрашивать его нельзя. Признак снимается и с настройки, и с
    // хода сессии: настройка объявляет, что прибор СПОСОБЕН заговорить
    // сам, ход — что он уже говорит.
    const bool tape = Oscill::decodeRs( m_settings.rs ).isRoll() || m_step == Step::Rolling;
    f.pace = tape ? Pace::Stream : Pace::OnRequest;

    // Байт на порцию: размер массива на число байт, которое занимает
    // выборка в ТЕКУЩЕМ формате. Формат не опознан — величины нет, и ноль
    // здесь означает именно это.
    Oscill::SampleFormat fmt = Oscill::SampleFormat::Normal;
    if ( Oscill::decodeSampleFormat( m_settings.m1, fmt ) )
        f.bytesPerPortion = std::size_t( m_settings.qs ) * std::size_t( Oscill::sampleBytes( fmt ) );

    if ( tape ) {
        // Лента идёт со скоростью выборки; порция размером QS набирается
        // за QS выборок.
        const double sps = Oscill::sampleRateSps( m_settings.ts, m_settings.mc );
        if ( sps > 0.0 && m_settings.qs > 0 )
            f.ratePerSecond = sps / double( m_settings.qs );
    } else {
        Oscill::CaptureTiming t;
        t.mc = m_settings.mc;
        t.ts = m_settings.ts;
        t.qs = m_settings.qs;
        t.rt = Oscill::decodeRt( m_settings.rt );
        t.ta = m_settings.ta;
        t.tw = m_settings.tw;
        t.td = m_settings.td;
        const double window = Oscill::captureWindowPs( t );
        // Бесконечность и ноль — не числа кадров в секунду: первая значит
        // «предела нет», второй — «интервал не вычислим». Оба дают 0, то
        // есть «порядок величины неизвестен», и это честнее выдуманной
        // частоты.
        if ( window > 0.0 && !std::isinf( window ) )
            f.ratePerSecond = 1.0e12 / window;
    }

    // Прореживание делает сам прибор: усреднение и пиковый режим идут
    // внутри него, до 256 проходов, и наружу выходит уже готовый массив
    // размером QS, а не поток выборок АЦП.
    f.decimatedAtSource = true;
    return f;
}


Type OscillBackend::type() const { return Type::Oscill; }

// ===========================================================================
// Instrument::WaveformSource
// ===========================================================================

unsigned OscillBackend::channelCount() const {
    // Ровно один: это свойство прибора, а не текущей настройки. Число
    // каналов в атрибутах кадра разбирается всё равно — формат рассчитан
    // на четыре, и молчаливое «там всегда один» уже стоило эталонной
    // реализации неверного разбора.
    return 1;
}


double OscillBackend::samplerate() const {
    // Sps, а не Hz: скорость выборки и частота сигнала — разные величины.
    // Ноль означает «период не установлен».
    return Oscill::sampleRateSps( m_settings.ts, m_settings.mc );
}

// ===========================================================================
// Паспорт, настройки, П3
// ===========================================================================

const OscillPassport &OscillBackend::passport() const { return m_passport; }

const OscillSettings &OscillBackend::settings() const { return m_settings; }

const std::vector< RegisterMismatch > &OscillBackend::mismatches() const { return m_mismatches; }


bool OscillBackend::readRegister( Oscill::Register reg, uint32_t &value ) {
    const QByteArray request = Oscill::makeReadRegister( reg, m_checksum );
    if ( request.isEmpty() ) {
        setError( QStringLiteral( "запрос чтения регистра %1 не собрался" )
                      .arg( QLatin1String( Oscill::registerName( reg ) ) ) );
        return false;
    }

    Oscill::Response r;
    if ( !transact( request, r, OscillTiming::SHORT_REPLY_MS ) )
        return false;

    if ( r.code == uint8_t( Oscill::Rsp::NotImplemented ) ) {
        // Такого регистра у прибора нет. Это СОСТОЯНИЕ прибора, а не отказ
        // линии: `lastError` не ставится, счётчик растёт.
        ++m_stats.notImplemented;
        return false;
    }
    return Oscill::valueFromResponse( r, reg, value );
}


bool OscillBackend::writeRegister( Oscill::Register reg, uint32_t wanted, uint32_t &actual ) {
    const QByteArray request = Oscill::makeWriteRegister( reg, wanted, m_writeStyle, m_checksum );
    if ( request.isEmpty() ) {
        setError( QStringLiteral( "запрос записи регистра %1 не собрался" )
                      .arg( QLatin1String( Oscill::registerName( reg ) ) ) );
        return false;
    }

    Oscill::Response r;
    if ( !transact( request, r, OscillTiming::SHORT_REPLY_MS ) )
        return false;

    if ( r.code == uint8_t( Oscill::Rsp::NotImplemented ) ) {
        ++m_stats.notImplemented;
        setError( QStringLiteral( "прибор не знает регистра %1" )
                      .arg( QLatin1String( Oscill::registerName( reg ) ) ) );
        return false;
    }
    if ( r.code == uint8_t( Oscill::Rsp::BadRequest ) ) {
        setError( QStringLiteral( "прибор не понял запись регистра %1" )
                      .arg( QLatin1String( Oscill::registerName( reg ) ) ) );
        return false;
    }

    // П3. Факт берётся из ответа совмещённой транзакции; если прибор
    // значения не вернул (или запись шла пакетом PUT), факт читается
    // отдельным запросом. Записать сюда `wanted` было бы прямым враньём:
    // прибор вправе подрезать запрошенное по своим границам, и вся суть
    // правила — увидеть, подрезал ли.
    bool haveActual = false;
    if ( m_writeStyle == Oscill::WriteStyle::CombinedGet )
        haveActual = Oscill::valueFromResponse( r, reg, actual );
    if ( !haveActual )
        haveActual = readRegister( reg, actual );
    if ( !haveActual ) {
        setError( QStringLiteral( "регистр %1 записан, но факт не прочитан" )
                      .arg( QLatin1String( Oscill::registerName( reg ) ) ) );
        return false;
    }

    storeActual( m_settings, reg, actual );
    noteMismatch( reg, wanted, actual );

    // Зависимые перечитываются ВСЕГДА, а не только при расхождении:
    // границы `QSh`, `TDl`/`TDh`, `P1h`/`P1l` двигаются от самой записи,
    // даже когда записанное принято дословно.
    refreshDependents( reg );
    return true;
}


bool OscillBackend::readProperty( Oscill::Property prop, uint32_t &value ) {
    const QByteArray request = Oscill::makeReadProperty( prop, m_checksum );
    if ( request.isEmpty() ) {
        setError( QStringLiteral( "запрос чтения свойства %1 не собрался" )
                      .arg( QLatin1String( Oscill::propertyName( prop ) ) ) );
        return false;
    }

    Oscill::Response r;
    if ( !transact( request, r, OscillTiming::SHORT_REPLY_MS ) )
        return false;

    if ( r.code == uint8_t( Oscill::Rsp::NotImplemented ) ) {
        // Прибор вправе не иметь свойства — тогда величины НЕТ. Именно на
        // этот ответ и опирается опрос двух непересекающихся списков
        // версий: какой из них поддержан, показывает прибор, а не бумага.
        ++m_stats.notImplemented;
        return false;
    }
    return Oscill::valueFromResponse( r, prop, value );
}


bool OscillBackend::readPropertyAscii( Oscill::Property prop, QString &text ) {
    const QByteArray request = Oscill::makeReadProperty( prop, m_checksum );
    if ( request.isEmpty() )
        return false;

    Oscill::Response r;
    if ( !transact( request, r, OscillTiming::SHORT_REPLY_MS ) )
        return false;

    if ( r.code == uint8_t( Oscill::Rsp::NotImplemented ) ) {
        ++m_stats.notImplemented;
        return false;
    }

    QByteArray raw;
    if ( !Oscill::asciiFromResponse( r, raw ) )
        return false;
    // Latin-1, а не локальная кодировка: значение объявлено как ASCII, и
    // перекодировщик здесь превратил бы четыре байта версии в вопросы.
    text = QString::fromLatin1( raw ).trimmed();
    return true;
}


bool OscillBackend::applySettings( const OscillSettings &wanted ) {
    if ( !m_state.linked ) {
        setError( QStringLiteral( "настройки не применены: связи нет" ) );
        return false;
    }

    m_mismatches.clear();
    bool lineOk = true;

    // Порядок взят из рабочего эталонного кода: QS/TS/TC записываются
    // ПОСЛЕДНИМИ. Записанные раньше, они были бы подрезаны последующими
    // записями — прибор пересчитывает свои пределы на каждой из них.
    for ( Oscill::Register reg : Oscill::initOrder() ) {
        uint32_t actual = 0;
        if ( !writeRegister( reg, wantedOf( wanted, reg ), actual ) ) {
            // Отказом считается только отказ ЛИНИИ или протокола.
            // Подрезанное прибором значение отказом не является: оно уже
            // легло в `mismatches()` и будет показано.
            lineOk = false;
        }
    }
    return lineOk;
}


bool OscillBackend::refreshDependents( Oscill::Register reg ) {
    bool ok = true;

    // Граф собран по описаниям первоисточника, а не по эталонному коду:
    // там связи RS → {TD, M1} и TS → {RS, M1, TD} не заведены вовсе, и
    // после записи RS или TS зависимые регистры не перечитываются.
    for ( Oscill::Register dep : Oscill::dependentRegisters( reg ) ) {
        uint32_t value = 0;
        if ( readRegister( dep, value ) )
            storeActual( m_settings, dep, value );
        else
            ok = false;
    }

    // Свойства-границы сдвигаются той же записью: `P1h`/`P1l` от
    // чувствительности, `QSh`/`TCh` от способа оцифровки и формата,
    // `TDl`/`TDh` от размера массива, `D1m` от уровня и режима
    // синхронизации.
    for ( Oscill::Property prop : Oscill::dependentProperties( reg ) ) {
        uint32_t value = 0;
        if ( !readProperty( prop, value ) ) {
            ok = false;
            continue;
        }
        switch ( prop ) {
        case Oscill::Property::QSh:
            m_passport.samplesMax.set( uint16_t( value ) );
            break;
        case Oscill::Property::TCh:
            m_passport.preSamplesMax.set( uint16_t( value ) );
            break;
        case Oscill::Property::TDl:
            m_passport.delayMin.set( value );
            break;
        case Oscill::Property::TDh:
            m_passport.delayMax.set( value );
            break;
        case Oscill::Property::P1h:
            m_passport.offsetMax.set( Oscill::signed16( value ) );
            break;
        case Oscill::Property::P1l:
            m_passport.offsetMin.set( Oscill::signed16( value ) );
            break;
        case Oscill::Property::D1m:
            m_passport.comparatorDelay.set( uint16_t( value ) );
            break;
        default:
            // Граф зависимостей объявляет только перечисленные свойства.
            // Молча проглотить чужое здесь нельзя: если список расширят, а
            // сюда не заглянут, величина обновится в приборе и не
            // обновится у нас.
            qWarning() << "Oscill: свойство" << Oscill::propertyName( prop )
                       << "объявлено зависимым, но в паспорт не кладётся";
            break;
        }
    }
    return ok;
}


bool OscillBackend::calibrate() {
    if ( !m_state.linked ) {
        setError( QStringLiteral( "калибровка не выполнена: связи нет" ) );
        return false;
    }

    const QByteArray request = Oscill::makeCalibrate( m_checksum );
    if ( request.isEmpty() ) {
        setError( QStringLiteral( "команда калибровки не собралась" ) );
        return false;
    }

    // Сколько длится калибровка, первоисточник не называет. Выдумывать
    // здесь секунды нельзя: если прибор не успел ответить за названный
    // короткий срок, это будет видно отказом с причиной, а не молчаливым
    // ожиданием неизвестной длины.
    Oscill::Response r;
    if ( !transact( request, r, OscillTiming::SHORT_REPLY_MS ) )
        return false;
    if ( r.code == uint8_t( Oscill::Rsp::NotImplemented ) ) {
        ++m_stats.notImplemented;
        setError( QStringLiteral( "прибор не знает команды калибровки 'C'" ) );
        return false;
    }
    if ( !r.isSuccess() ) {
        setError( QStringLiteral( "прибор отклонил калибровку: код ответа 0x%1" )
                      .arg( r.code, 2, 16, QLatin1Char( '0' ) ) );
        return false;
    }

    // Калибровка привязывает смещение канала и уровень синхронизации к
    // диапазону АЦП — то есть меняет P1 и S1 без нашего ведома. Не
    // перечитать их значило бы держать в настройках числа, которых в
    // приборе уже нет.
    bool ok = true;
    for ( Oscill::Register reg : { Oscill::Register::P1, Oscill::Register::S1 } ) {
        uint32_t value = 0;
        if ( readRegister( reg, value ) )
            storeActual( m_settings, reg, value );
        else
            ok = false;
    }
    if ( !ok )
        setError( QStringLiteral( "калибровка выполнена, но P1/S1 не перечитаны" ) );
    return ok;
}

// ===========================================================================
// Кадр
// ===========================================================================

const Oscill::Frame &OscillBackend::frame() const { return m_frame; }

unsigned long long OscillBackend::frameSerial() const { return m_frameSerial; }


Oscill::ChannelScale OscillBackend::channelScale() const {
    // Формат берётся у ПОСЛЕДНЕГО кадра: шаг кода зависит от него
    // (повышенное разрешение кладёт на тот же размах 65536 кодов вместо
    // 256). Если кадра ещё не было — из регистра M1, то есть из того, что
    // прибор подтвердил при чтении.
    Oscill::SampleFormat fmt = Oscill::SampleFormat::Normal;
    bool known = false;
    if ( m_frame.valid && !m_frame.channel.empty() && m_frame.channel.front().formatKnown ) {
        fmt = m_frame.channel.front().format;
        known = true;
    } else {
        known = Oscill::decodeSampleFormat( m_settings.m1, fmt );
    }
    if ( !known )
        // Формат неизвестен — шкалы нет. `known == false` и есть отказ:
        // величина без единицы и годности числом не показывается.
        return Oscill::ChannelScale();

    return Oscill::channelScale( m_settings.v1, m_settings.p1, fmt, m_codeSpan );
}


Oscill::CodeSpan OscillBackend::codeSpan() const { return m_codeSpan; }


void OscillBackend::setCodeSpan( Oscill::CodeSpan span ) {
    // Рамка устанавливается настройкой, а не догадкой: противоречие «8 мВ
    // против 8,53 мВ» разрешается ОДНИМ измерением на приборе, и до него
    // бэкенд отдаёт коды, а не вольты. [ПЛАН]
    m_codeSpan = span;
}


Oscill::FrameLayout OscillBackend::frameLayout() const { return m_layout; }


void OscillBackend::setFrameLayout( Oscill::FrameLayout layout ) {
    m_layout = layout;
    qDebug() << "Oscill: раскладка кадра задана снаружи:" << int( layout );
}

// ===========================================================================
// Бесконечная лента (roll)
// ===========================================================================

bool OscillBackend::startRoll() {
    if ( !m_state.linked ) {
        setError( QStringLiteral( "лента не запущена: связи нет" ) );
        return false;
    }
    if ( m_step == Step::Rolling )
        return true; // уже идёт; повторный запуск ничего не меняет

    // RS бесконечной оцифровки. Регистр — ТРИ НЕЗАВИСИМЫХ ПЕРЕКЛЮЧАТЕЛЯ, и
    // ставятся два из них, каждый по своему основанию (разбор —
    // `AcquisitionMode` в oscillprotocol.h):
    //
    //   бит 2  BufferType    = ROLL     — сама бесконечность;
    //   бит 1  DataOutputType = REALTIME — передача параллельно оцифровке.
    //          Ленте она нужна по существу: у бесконечного массива нет
    //          момента «оцифровка закончилась», после которого пошли бы
    //          данные.
    //
    // Бит 0 (ProcessingType = RIS) не трогаем: стробоскопическая выборка —
    // отдельный переключатель, служит периодике высокой частоты и к ленте
    // отношения не имеет.
    //
    // Прежняя редакция ставила все три и получала 0x07 — то есть включала
    // заодно стробоскоп, о котором её никто не просил.
    Oscill::AcquisitionMode mode;
    mode.ris = false;
    mode.parallel = true;
    mode.roll = true;

    uint32_t actual = 0;
    if ( !writeRegister( Oscill::Register::RS, Oscill::encode( mode ), actual ) )
        return false;
    if ( !Oscill::decodeRs( uint8_t( actual ) ).isRoll() ) {
        // Прибор бесконечную оцифровку не принял (вправе: она возможна не
        // при всяком периоде выборки). Отдать 'D' после этого значило бы
        // запросить обычный кадр и назвать его лентой.
        setError( QStringLiteral( "прибор не принял бесконечную оцифровку: RS = 0x%1" )
                      .arg( actual, 2, 16, QLatin1Char( '0' ) ) );
        return false;
    }

    m_assembler.reset();
    m_rollSamples.clear();

    Oscill::Response r;
    if ( !transact( Oscill::makeDigitize( m_checksum ), r, guardMs() ) )
        return false;
    if ( !r.isContinue() ) {
        // Лента объявляется кодом Continue: «данные не поместились в один
        // пакет, забирай следующую порцию». Success означает законченный
        // объект — то есть прибор отдал обычный кадр и ленту не начинал.
        // Объявить это лентой значило бы ждать порций, которых не будет.
        setError( QStringLiteral( "прибор ответил на 'D' кодом 0x%1, а не Continue: ленты нет" )
                      .arg( r.code, 2, 16, QLatin1Char( '0' ) ) );
        return false;
    }

    // Заголовок ленты приходит ОДИН раз, в её начале, и в пакетах Continue
    // не повторяется. Дальше идут только выборки.
    m_assembler.feed( r );
    const QByteArray head = m_assembler.body();
    m_assembler.reset();

    // Раскладка задаётся явно и с основанием: у бесконечного массива поля
    // размера быть не может по построению — размер не объявляют тому, что
    // не кончается. Это не выбор между двумя гипотезами, а следствие
    // режима.
    Oscill::Frame frame;
    if ( !Oscill::parseFrame( head, Oscill::FrameLayout::WithoutSizeField, frame ) ||
         frame.channel.empty() ) {
        setError( QStringLiteral( "заголовок ленты не разобран: %1 байт" ).arg( head.size() ) );
        return false;
    }
    if ( frame.channels != 1u ) {
        // Границ между каналами в этой раскладке нет. Резать наугад значило
        // бы выдать чужие байты за выборки канала.
        setError( QStringLiteral( "лента объявляет %1 канала: разделить их нечем" )
                      .arg( frame.channels ) );
        return false;
    }

    m_frame = frame;
    ++m_frameSerial;
    ++m_stats.framesReceived;
    // Коды из первой порции — сразу в ленту; незавершённый хвост остаётся
    // в сыром массиве канала.
    harvestRoll( m_frame, m_rollSamples );

    // Правило ленты, которое дальше держит `takeRollSamples()`: после
    // КАЖДОЙ принятой порции сразу уходит запрос следующей. Обмен OBEX
    // остаётся «запрос — ответ» и в бесконечной оцифровке: код Continue
    // прямо и означает «забери следующую порцию запросом Get». Без этого
    // первого запроса лента встала бы, не начавшись, а молчание прибора
    // выглядело бы отказом линии.
    if ( !sendPacket( Oscill::makeBare( m_continueFinal ? Oscill::Op::GetFinal : Oscill::Op::Get ) ) )
        return false;

    m_step = Step::Rolling;
    return true;
}


bool OscillBackend::stopRoll() {
    if ( m_step != Step::Rolling )
        return true; // ленты нет — останавливать нечего

    // Прибор прекращает поток ТОЛЬКО по требованию хоста. Перестать читать
    // недостаточно: он продолжит говорить, и порт останется забит
    // недосказанным ответом.
    const bool sent = sendPacket( Oscill::makeAbort() );
    QThread::msleep( OscillTiming::RESET_SETTLE_MS );
    drainInput( m_transport.get(), asSerial( m_transport.get() ) );

    m_assembler.reset();
    m_rollSamples.clear();
    m_step = Step::Idle;

    // RS здесь НЕ восстанавливается: вернуть прибор к другому способу
    // оцифровки — решение вызывающего, а не побочное действие остановки.
    // Пока RS остаётся бесконечным, `flow()` продолжает называть темп
    // потоковым — и правильно делает: команда 'D' запустила бы ленту снова.
    if ( !sent )
        setError( QStringLiteral( "пакет остановки ленты не отправлен" ) );
    return sent;
}


bool OscillBackend::rollRunning() const { return m_step == Step::Rolling; }


namespace {

/// \brief Сбой посреди ленты: назвать причину и решить, продолжается ли она.
///
/// Находка состязательной сверки 2026-09-06: из цикла ленты вело ПЯТЬ путей
/// выхода (молчание, обрывок, несошедшаяся сумма, неразобранный ответ, отказ
/// сервера), и каждый уходил ДО отправки запроса следующей порции. Правило,
/// объявленное в самом цикле — «пока лента идёт, у неё обязан быть
/// неотвеченный запрос», — на этих путях нарушалось, и лента вставала
/// НАВСЕГДА И МОЛЧА: `update()` при `Step::Rolling` возвращается сразу, а
/// запрашивать порцию больше некому.
///
/// Здесь пути сведены к одному. Приёмный буфер чистится (в нём мог остаться
/// хвост испорченного пакета, который иначе сдвинет разбор следующего),
/// причина называется, и делается ОДНА попытка перезапросить порцию — ровно
/// столько же, сколько разрешает обмен коротким запросом. Не вышло — лента
/// прекращается честно, с записанной причиной, а не замирает.
bool rollFault( int &repeats, const QString &reason, QString &errorOut ) {
    if ( repeats >= OscillTiming::REPEAT_LIMIT ) {
        errorOut = QStringLiteral( "лента прекращена: %1" ).arg( reason );
        return false;
    }
    ++repeats;
    return true;
}

} // namespace

std::vector< int > OscillBackend::takeRollSamples() {
    if ( m_step != Step::Rolling )
        return std::vector< int >();
    if ( m_frame.channel.empty() ) {
        setError( QStringLiteral( "лента идёт без разобранного заголовка: формат порции неизвестен" ) );
        return std::vector< int >();
    }

    Transport *t = m_transport.get();
    SerialTransport *serial = asSerial( t );

    // За один заход забирается не больше, чем мы САМИ объявили прибору
    // приёмным буфером в CONNECT. Предел нужен потому, что лента
    // бесконечна: без него вызов не вернулся бы никогда, пока прибор
    // говорит быстрее, чем мы разбираем.
    int taken = 0;
    int rollRepeats = 0; // перезапросов порции, сделанных в ЭТОМ вызове
    while ( taken < int( Oscill::CLIENT_RX_BUFFER ) ) {
        QByteArray packet;
        // Ждать нечего: лента идёт своим ходом, и «пока пусто» — законное
        // состояние, а не молчание прибора.
        const Intake in = takePacket( t, serial, 0, OscillTiming::CONTINUE_REPLY_MS, packet );

        // Молчание лентой не считается отказом: порция могла ещё не
        // набраться. Но и уходить с неотвеченным запросом нельзя — выход
        // отсюда обязан пройти через общий завершитель, как и все прочие.
        QString fault;
        if ( in == Intake::Silence ) {
            break; // запрос уже отправлен и остаётся неотвеченным — лента жива
        } else if ( in == Intake::Fragment ) {
            ++m_stats.lengthMismatch;
            fault = QStringLiteral( "пришёл обрывок пакета" );
        }

        Oscill::Response r;
        if ( fault.isEmpty() ) {
            const Oscill::ParseError err = Oscill::parseResponse( packet, r );
            if ( err == Oscill::ParseError::BadChecksum ) {
                ++m_stats.badChecksum;
                fault = QStringLiteral( "контрольная сумма не сошлась" );
            } else if ( err != Oscill::ParseError::None ) {
                ++m_stats.lengthMismatch;
                fault = QStringLiteral( "ответ не разобран: %1" )
                            .arg( QString::fromLatin1( Oscill::parseErrorName( err ) ) );
            } else if ( r.code == uint8_t( Oscill::Rsp::InternalError ) ) {
                ++m_stats.serverErrors;
                fault = QStringLiteral( "прибор ответил внутренней ошибкой" );
            }
        }

        if ( !fault.isEmpty() ) {
            drainInput( m_transport.get(), serial );
            QString stopReason;
            if ( !rollFault( rollRepeats, fault, stopReason ) ) {
                setError( stopReason );
                m_step = Step::Idle;
                break;
            }
            // Перезапрос порции: без него лента осталась бы без
            // неотвеченного запроса и встала бы молча.
            if ( !sendPacket( Oscill::makeBare( m_continueFinal ? Oscill::Op::GetFinal : Oscill::Op::Get ) ) ) {
                setError( QStringLiteral( "лента прекращена: %1, и перезапрос порции не ушёл" ).arg( fault ) );
                m_step = Step::Idle;
                break;
            }
            continue;
        }

        // Накопитель в ленте не участвует намеренно: он копит тело
        // ЦЕЛИКОМ, а лента не кончается. Байты порции идут прямо в сырой
        // массив канала, откуда `harvestRoll()` берёт целые коды и
        // оставляет незавершённый хвост.
        bool finished = false;
        int brought = 0; // байт, принесённых ЭТОЙ порцией
        for ( const Oscill::Header &h : r.headers ) {
            if ( h.id == uint8_t( Oscill::HeaderId::BodyPart ) ) {
                m_frame.channel.front().raw.append( h.body );
                brought += h.body.size();
                continue;
            }
            if ( h.id == uint8_t( Oscill::HeaderId::BodyEnd ) ) {
                m_frame.channel.front().raw.append( h.body );
                brought += h.body.size();
                finished = true; // объект закончен: лента прекратилась сама
                break;
            }
        }
        taken += brought;
        harvestRoll( m_frame, m_rollSamples );

        if ( finished ) {
            m_step = Step::Idle;
            break;
        }
        if ( !r.isContinue() )
            // Не Continue и не конец тела: прибор сказал что-то, чего в
            // ленте быть не должно. Продолжения не запрашиваем — иначе
            // запрос уйдёт в пустоту и съест защитный интервал.
            break;

        // Запрос следующей порции уходит ВСЕГДА, даже когда эта порция
        // пришла пустой: пока лента идёт, у неё обязан быть неотвеченный
        // запрос, иначе она встанет молча.
        if ( !sendPacket( Oscill::makeBare( m_continueFinal ? Oscill::Op::GetFinal : Oscill::Op::Get ) ) )
            break;

        // А вот забирать дальше в ЭТОМ вызове незачем: порция без единого
        // байта прогрессом не является, и предел `taken` её не остановит —
        // он считает байты, которых нет. Без этой проверки поток пустых
        // продолжений держал бы вызов вечно; такой прибор — не выдумка, а
        // тот же прибор в момент, когда выборки ещё не набрались.
        if ( brought == 0 )
            break;
    }

    std::vector< int > out;
    out.swap( m_rollSamples );
    return out;
}

// ===========================================================================
// Линия (П8)
// ===========================================================================

bool OscillBackend::negotiateSpeed( int baud ) {
    if ( baud <= 0 ) {
        setError( QStringLiteral( "скорость линии не задана" ) );
        return false;
    }

    // Величина прибора — КОЭФФИЦИЕНТ, скорость есть его следствие.
    // Скорости, для которой целого коэффициента нет, прибор принять не
    // может, и «ближайшая» здесь запрещена: хост открыл бы порт на одной
    // скорости, прибор перешёл бы на другую, и линия умерла бы молча.
    const uint8_t divisor = Oscill::speedDivisor( uint32_t( baud ) );
    if ( divisor == 0 ) {
        setError( QStringLiteral( "для скорости %1 бод у прибора нет коэффициента" ).arg( baud ) );
        return false;
    }

    SerialTransport *serial = asSerial( m_transport.get() );
    if ( !serial ) {
        setError( QStringLiteral( "сменить скорость можно только на последовательной оснастке" ) );
        return false;
    }

    const QByteArray request = Oscill::makeSetSpeed( divisor, m_checksum );
    if ( request.isEmpty() ) {
        setError( QStringLiteral( "пакет смены скорости не собрался" ) );
        return false;
    }

    // Порядок именно такой: сперва прибор, потом мы. Прибор переходит на
    // новую скорость ПОСЛЕ ответа на своей старой — значит ответ надо
    // дождаться, иначе мы сменим скорость раньше, чем он договорит, и
    // разберём его ответ как мусор.
    Oscill::Response r;
    if ( !transact( request, r, OscillTiming::SHORT_REPLY_MS ) ) {
        setError( QStringLiteral( "прибор не ответил на смену скорости: скорость линии не менялась" ) );
        return false;
    }

    if ( !serial->setBaudRate( baud ) ) {
        // Прибор уже перешёл, а мы — нет. Связи в этом состоянии нет, и
        // делать вид, что она есть, нельзя: обход начал бы опрашивать
        // мёртвую линию. Лечится повторным `link()` на новой скорости.
        m_state.linked = false;
        m_step = Step::Offline;
        setError( QStringLiteral( "прибор перешёл на %1 бод, а порт — нет: связь потеряна" ).arg( baud ) );
        return false;
    }

    // Байты, оказавшиеся в линии в момент переключения, приняты на разных
    // скоростях и достоверными не являются.
    drainInput( m_transport.get(), serial );
    return true;
}


bool OscillBackend::checksumEnabled() const { return m_checksum; }

void OscillBackend::setChecksumEnabled( bool on ) { m_checksum = on; }

Oscill::WriteStyle OscillBackend::registerWriteStyle() const { return m_writeStyle; }

void OscillBackend::setRegisterWriteStyle( Oscill::WriteStyle style ) { m_writeStyle = style; }

bool OscillBackend::continueWithFinalBit() const { return m_continueFinal; }

void OscillBackend::setContinueWithFinalBit( bool on ) { m_continueFinal = on; }

const OscillBackend::LinkStats &OscillBackend::linkStats() const { return m_stats; }

OscillBackend::Step OscillBackend::step() const { return m_step; }

Oscill::Known< uint16_t > OscillBackend::deviceRxBuffer() const { return m_deviceRx; }

// ===========================================================================
// Сессия OBEX: внутреннее
// ===========================================================================

bool OscillBackend::sendPacket( const QByteArray &packet ) {
    if ( packet.isEmpty() ) {
        setError( QStringLiteral( "пакет не собран: отправлять нечего" ) );
        return false;
    }
    if ( !m_transport || !m_transport->isOpen() ) {
        setError( QStringLiteral( "порт закрыт" ) );
        return false;
    }

    // Единственные ворота исходящих байт — и единственное место, где
    // проверяется предел размера. Первоисточник: «нет необходимости слать
    // Oscill-у пакеты, превышающие 32 байта», и это ограничение действует
    // независимо от того, что прибор объявил своим приёмным буфером.
    if ( packet.size() > Oscill::SAFE_TX_LIMIT ) {
        setError( QStringLiteral( "пакет %1 байт длиннее предела %2: прибору не отправлен" )
                      .arg( packet.size() )
                      .arg( Oscill::SAFE_TX_LIMIT ) );
        return false;
    }
    if ( m_deviceRx.known && packet.size() > int( m_deviceRx.value ) ) {
        setError( QStringLiteral( "пакет %1 байт больше приёмного буфера прибора (%2)" )
                      .arg( packet.size() )
                      .arg( m_deviceRx.value ) );
        return false;
    }

    if ( !m_transport->write( packet ) ) {
        const QString why = m_transport->lastError();
        setError( why.isEmpty() ? QStringLiteral( "пакет не записан в порт" )
                                : QStringLiteral( "пакет не записан в порт: %1" ).arg( why ) );
        return false;
    }
    return true;
}


bool OscillBackend::receiveResponse( Oscill::Response &out, int guard ) {
    out = Oscill::Response();
    if ( !m_transport || !m_transport->isOpen() ) {
        setError( QStringLiteral( "порт закрыт" ) );
        return false;
    }

    SerialTransport *serial = asSerial( m_transport.get() );
    QByteArray packet;
    const Intake in = takePacket( m_transport.get(), serial, guard, OscillTiming::CONTINUE_REPLY_MS, packet );

    if ( in == Intake::Silence ) {
        // Ноль байт есть ОТСУТСТВИЕ ответа. Это не ошибка формата, и
        // перезапрашивать ответ, которого не было, нельзя — повторяют
        // запрос, и решает это вызывающий.
        ++m_stats.silentIntervals;
        return false;
    }
    if ( in == Intake::Fragment ) {
        // Хотя бы один байт есть — значит ответ был, но пришёл битым.
        ++m_stats.lengthMismatch;
        return false;
    }

    switch ( Oscill::parseResponse( packet, out ) ) {
    case Oscill::ParseError::None:
        break;
    case Oscill::ParseError::BadChecksum:
        ++m_stats.badChecksum;
        return false;
    case Oscill::ParseError::MalformedHeader:
        // Заголовок, выходящий за пакет, есть расхождение длины, только
        // обнаруженное на шаг позже. Заводить под него отдельный счётчик
        // значило бы делить одну величину надвое.
        ++m_stats.lengthMismatch;
        return false;
    case Oscill::ParseError::LengthMismatch:
    case Oscill::ParseError::Incomplete:
    case Oscill::ParseError::NotConnect:
        ++m_stats.lengthMismatch;
        return false;
    }

    // Отсутствие суммы во ВХОДЯЩЕМ пакете ошибкой не является, даже когда
    // мы сами её ставим: первоисточник объявляет её необязательной, и ни
    // одна эталонная реализация её не считает.
    if ( out.code == uint8_t( Oscill::Rsp::InternalError ) )
        ++m_stats.serverErrors;
    return true;
}


bool OscillBackend::transact( const QByteArray &request, Oscill::Response &out, int guard ) {
    if ( !sendPacket( request ) )
        return false;

    // Оба предела — по одному повтору, как велит первоисточник. Счётчики
    // местные: транзакция синхронна, между её проходами состояние сессии
    // не переключается.
    int requestRepeats = 0; // повторы САМОГО запроса (молчание, 0xD0)
    int answerRepeats = 0;  // перезапросы ОТВЕТА (0x92: сумма или длина)

    for ( ;; ) {
        const LinkStats before = m_stats;

        if ( receiveResponse( out, guard ) ) {
            if ( out.code != uint8_t( Oscill::Rsp::InternalError ) )
                return true;
            // Прибор увидел искажение запроса. Повторяется запрос.
            if ( requestRepeats >= OscillTiming::REPEAT_LIMIT ) {
                setError( QStringLiteral( "прибор дважды не понял запрос" ) );
                return false;
            }
            ++requestRepeats;
            if ( !sendPacket( request ) )
                return false;
            continue;
        }

        // Различие «тишина или порча» — по тому, какой счётчик вырос:
        // именно оно и решает, что повторять. Смешать их значило бы
        // перезапрашивать ответ, которого никто не отправлял.
        const bool silent = m_stats.silentIntervals > before.silentIntervals;
        if ( silent ) {
            if ( guard == 0 )
                // Предела молчанию нет (бесконечно ждущий запуск) либо
                // ждать не просили. Повторять запрос не по чему.
                return false;
            if ( requestRepeats >= OscillTiming::REPEAT_LIMIT ) {
                setError( QStringLiteral( "прибор молчит дольше защитного интервала (%1 мс)" ).arg( guard ) );
                return false;
            }
            ++requestRepeats;
            if ( !sendPacket( request ) )
                return false;
            continue;
        }

        if ( answerRepeats >= OscillTiming::REPEAT_LIMIT ) {
            setError( QStringLiteral( "ответ прибора не разобран и после перезапроса" ) );
            return false;
        }
        // Хвост испорченного ответа выбрасывается до перезапроса: иначе он
        // встанет впереди повторённого и испортит уже его.
        drainInput( m_transport.get(), asSerial( m_transport.get() ) );
        ++answerRepeats;
        ++m_stats.repeatsSent;
        if ( !sendPacket( Oscill::makeRepeatLast( m_checksum ) ) )
            return false;
    }
}


bool OscillBackend::collectBody( Oscill::Response &first, QByteArray &body, int guard ) {
    m_assembler.reset();
    body.clear();

    // Предел длины — не выдуманное число, а то, что прибор СПОСОБЕН
    // вернуть: размер массива не больше `QSh` выборок по два байта плюс
    // атрибуты. Пока `QSh` не прочитан, берётся предел самого поля размера.
    // Без предела цикл держался бы на добросовестности прибора.
    const int maxSamples = m_passport.samplesMax.known ? int( m_passport.samplesMax.value ) : 0xFFFF;
    const int maxBody = maxSamples * 2 + 16;

    // `first` — рабочий ответ, а не только начальный: каждая следующая
    // порция ложится в него же. Потому он и принят изменяемой ссылкой:
    // после возврата вызывающий видит ПОСЛЕДНИЙ ответ прибора, а не
    // первый, — по нему видно, чем кончился обмен.
    int idleRounds = 0;

    for ( ;; ) {
        // Прогресс меряется ПРИНЕСЁННЫМИ БАЙТАМИ, а не фактом «заголовок
        // тела в ответе был». Возврат `feed()` истинен уже потому, что
        // заголовок 0x48 встретился, — даже если тело в нём пустое. Прибор,
        // отвечающий пустыми продолжениями, обнулял бы счётчик простоя
        // каждым таким ответом, и цикл не кончался бы никогда.
        const int sizeBefore = int( m_assembler.body().size() );
        m_assembler.feed( first );
        const bool brought = int( m_assembler.body().size() ) > sizeBefore;
        if ( m_assembler.complete() )
            break;

        if ( !first.isContinue() ) {
            // Продолжения не будет, а объект не закончен: тела больше
            // взяться неоткуда.
            setError( brought || sizeBefore
                          ? QStringLiteral( "длинный ответ оборван: конца тела 0x49 не пришло" )
                          : QStringLiteral( "ответ не несёт тела объекта" ) );
            m_assembler.reset();
            return false;
        }

        if ( int( m_assembler.body().size() ) > maxBody ) {
            setError( QStringLiteral( "ответ длиннее, чем прибор способен вернуть (%1 байт)" ).arg( maxBody ) );
            m_assembler.reset();
            return false;
        }

        // Continue без единого байта тела прогрессом не является. Один
        // такой проход простим (прибор вправе разделить служебное и
        // данные), два подряд означают, что объект не соберётся никогда.
        idleRounds = brought ? 0 : idleRounds + 1;
        if ( idleRounds > OscillTiming::REPEAT_LIMIT ) {
            setError( QStringLiteral( "прибор шлёт продолжения без данных" ) );
            m_assembler.reset();
            return false;
        }

        const QByteArray next = Oscill::makeBare( m_continueFinal ? Oscill::Op::GetFinal : Oscill::Op::Get );
        if ( !transact( next, first, guard ) ) {
            m_assembler.reset();
            return false;
        }
    }

    body = m_assembler.body();
    m_assembler.reset();
    return true;
}


bool OscillBackend::readPassport() {
    m_passport = OscillPassport();
    bool any = false;

    // Опознание. Оба списка версий опрашиваются целиком: какой из них
    // поддержан прибором, показывает его ответ `0xD1` на остальные, а не
    // выбор на бумаге. Пустое поле честнее подставленного.
    if ( readPropertyAscii( Oscill::Property::VNM, m_passport.deviceId ) )
        any = true;
    if ( readPropertyAscii( Oscill::Property::VSN, m_passport.serial ) )
        any = true;
    if ( readPropertyAscii( Oscill::Property::VHW, m_passport.hardware ) )
        any = true;
    if ( m_passport.hardware.isEmpty() ) {
        // Второй список спрашивается не только при отказе `0xD1`, но и при
        // ПУСТОМ ответе: четыре нуля версией не являются, а именно так
        // отвечает прибор (и поддельный прибор проекта), у которого имя
        // свойства не объявлено, но отказывать он не приучен. Считать
        // такой ответ версией значило бы записать в паспорт пустоту и
        // больше ни о чём не спросить.
        //
        // Версии узлов склеиваются с ИМЕНАМИ: «1.02, 1.00» не сказало бы,
        // что чему принадлежит.
        QStringList parts;
        for ( Oscill::Property p : { Oscill::Property::VHD, Oscill::Property::VHA } ) {
            QString text;
            if ( readPropertyAscii( p, text ) ) {
                any = true;
                if ( !text.isEmpty() )
                    parts << QStringLiteral( "%1 %2" )
                                 .arg( QString::fromLatin1( Oscill::propertyName( p ) ), text );
            }
        }
        m_passport.hardware = parts.join( QStringLiteral( ", " ) );
    }
    if ( readPropertyAscii( Oscill::Property::VSW, m_passport.software ) )
        any = true;
    if ( m_passport.software.isEmpty() ) {
        QStringList parts;
        for ( Oscill::Property p :
              { Oscill::Property::VSD, Oscill::Property::VSI, Oscill::Property::VSC, Oscill::Property::VSO } ) {
            QString text;
            if ( readPropertyAscii( p, text ) ) {
                any = true;
                if ( !text.isEmpty() )
                    parts << QStringLiteral( "%1 %2" )
                                 .arg( QString::fromLatin1( Oscill::propertyName( p ) ), text );
            }
        }
        m_passport.software = parts.join( QStringLiteral( ", " ) );
    }

    // Числовые свойства. Каждое кладётся в `Known<>`: непрочитанное
    // остаётся непрочитанным, а не нулём. На этих числах строятся деления
    // и границы, и ноль вместо «нет величины» дал бы либо деление на ноль,
    // либо предел в ноль выборок.
    struct NumericSlot {
        Oscill::Property prop;
        Oscill::Known< uint16_t > *u16;
        Oscill::Known< uint32_t > *u32;
        Oscill::Known< int16_t > *i16;
    };
    // Имя массива не `slots`: это ключевое слово Qt (макрос `slots`), и
    // переменная с таким именем не компилируется вовсе.
    const NumericSlot numericSlots[] = {
        { Oscill::Property::MCd, &m_passport.tickDefault, nullptr, nullptr },
        { Oscill::Property::MCl, &m_passport.tickMin, nullptr, nullptr },
        { Oscill::Property::TOl, &m_passport.tsRealtimeMin, nullptr, nullptr },
        { Oscill::Property::TOv, nullptr, &m_passport.fastVariants, nullptr },
        { Oscill::Property::TMl, &m_passport.tsRisMin, nullptr, nullptr },
        { Oscill::Property::TMh, &m_passport.tsRisMax, nullptr, nullptr },
        { Oscill::Property::TPl, nullptr, &m_passport.tsParallelMin, nullptr },
        { Oscill::Property::TCh, &m_passport.preSamplesMax, nullptr, nullptr },
        { Oscill::Property::QSh, &m_passport.samplesMax, nullptr, nullptr },
        { Oscill::Property::TDl, nullptr, &m_passport.delayMin, nullptr },
        { Oscill::Property::TDh, nullptr, &m_passport.delayMax, nullptr },
        { Oscill::Property::V1h, &m_passport.sensitivityCoarse, nullptr, nullptr },
        { Oscill::Property::V1l, &m_passport.sensitivityFine, nullptr, nullptr },
        { Oscill::Property::P1h, nullptr, nullptr, &m_passport.offsetMax },
        { Oscill::Property::P1l, nullptr, nullptr, &m_passport.offsetMin },
        { Oscill::Property::D1m, &m_passport.comparatorDelay, nullptr, nullptr },
    };

    for ( const NumericSlot &slot : numericSlots ) {
        uint32_t value = 0;
        if ( !readProperty( slot.prop, value ) )
            continue;
        any = true;
        if ( slot.u16 )
            slot.u16->set( uint16_t( value ) );
        else if ( slot.u32 )
            slot.u32->set( value );
        else if ( slot.i16 )
            // Знаковые свойства приходят сырыми шестнадцатью битами:
            // разворот делается там, где известно, что величина знаковая.
            slot.i16->set( Oscill::signed16( value ) );
    }

    // Паспорт годен, если прибор ответил хотя бы на одно свойство. Более
    // строгого условия здесь быть не может: какие именно свойства прибор
    // поддерживает, заранее не известно (два непересекающихся списка
    // версий тому и пример), а годность КАЖДОЙ величины несёт её
    // собственный признак `known`.
    m_passport.valid = any;
    if ( !any )
        qWarning() << "Oscill: прибор не ответил ни на одно свойство - паспорта нет";
    return any;
}


int OscillBackend::guardMs() const {
    Oscill::CaptureTiming t;
    t.mc = m_settings.mc;
    t.ts = m_settings.ts;
    t.qs = m_settings.qs;
    t.rt = Oscill::decodeRt( m_settings.rt );
    t.ta = m_settings.ta;
    t.tw = m_settings.tw;
    t.td = m_settings.td;

    const double ps = Oscill::captureWindowPs( t );

    if ( std::isinf( ps ) )
        // Бесконечно ждущий запуск: предела молчанию нет вовсе. Ноль
        // означает «в этом шаге не ждать» — заглянуть в буфер и выйти;
        // повтор запроса при этом не делается, и проверяет это вызывающий
        // отдельным условием, а не по нулю.
        return 0;
    if ( !( ps > 0.0 ) )
        // Такт не установлен — интервал не вычислим. Берётся НАЗВАННАЯ
        // величина, а не выдуманная секунда: второго места, порождающего
        // срок ожидания, здесь не заводится.
        return OscillTiming::SHORT_REPLY_MS;

    // Вычисленное окно говорит, сколько прибор ОЦИФРОВЫВАЕТ: ожидание
    // синхронизации, задержка развёртки и сам проход по выборкам. Оно НЕ
    // говорит, за сколько прибор соберётся ответить, — а это отдельное
    // слагаемое, и оно у прибора есть всегда, как и у ответа на короткий
    // запрос. Поэтому сроки СКЛАДЫВАЮТСЯ, и оба названы: своего числа
    // здесь не заводится.
    //
    // Слагаемое не украшение. Прогон против поддельного прибора
    // (`tools/oscill-mock`) при `TA = 500` и `QS = 256` дал окно 284 мкс,
    // то есть защитный интервал в 1 мс, — и клиент объявлял молчанием
    // ответ, который прибор ещё не начал слать: повтор запроса на каждом
    // кадре, ни одного принятого кадра. С «первым байтом ответа» в
    // слагаемых кадры пошли. [СБОРКА]
    const double ms = std::ceil( ps / 1.0e9 ) + double( OscillTiming::SHORT_REPLY_MS );
    if ( ms > double( std::numeric_limits< int >::max() ) )
        return std::numeric_limits< int >::max();
    return int( ms );
}


void OscillBackend::noteMismatch( Oscill::Register reg, uint32_t wanted, uint32_t actual ) {
    if ( wanted == actual )
        return;

    // Прибор подрезал запрошенное по своим границам. Это НЕ ошибка — но и
    // не мелочь: подрезанный предел, о котором не сказали, есть враньё об
    // измерении. Расхождение складывается, чтобы его показали.
    RegisterMismatch m;
    m.reg = reg;
    m.wanted = wanted;
    m.actual = actual;
    m_mismatches.push_back( m );
    qDebug() << "Oscill: регистр" << Oscill::registerName( reg ) << "запрошен" << wanted << "принят"
             << actual;
}


void OscillBackend::setError( const QString &text ) {
    m_state.lastError = text;
    qWarning() << "Oscill:" << text;
}

// ===========================================================================
// Оснастка слота и объявление модели
// ===========================================================================

TransportPtr makeOscillTransport( int slot ) {
    // [ПЛАН]. Порт и скорость слота обязаны приходить из реестра
    // параметров (`params.h`, группа `oscill`, ключи `port` и `baud`) —
    // фабрика реестра приборов по договору принимает ТОЛЬКО номер слота.
    // Группы этой в реестре параметров сегодня нет, и придумать порт
    // здесь нельзя: открытый наугад чужой COM-порт есть вмешательство в
    // прибор, которого никто не просил, а угаданная скорость — молчаливо
    // мёртвая линия.
    //
    // Пустой указатель виден сразу: `probe()` отвечает «оснастка слота не
    // задана», и причина попадает в `State::lastError`.
    qWarning() << "Oscill: слот" << slot
               << "не оснащён: группы параметров «oscill» (port, baud) в реестре нет";
    return TransportPtr();
}


bool declareOscill() {
    // Ровно одна строка, как принято в дереве: модель объявляет свой тип и
    // способ создания, фронтенд моделей не перечисляет.
    //
    // Вызывается ЯВНО из точки сборки приложения. Статическим
    // инициализатором нельзя: тест дерева приборов объявляет `Type::Oscill`
    // своей поддельной фабрикой и отдельно проверяет, что повторное
    // объявление отклоняется, — саморегистрация столкнулась бы с этой
    // проверкой, и падать начал бы тест, а не виноватый код.
    return Registry::instance().declare(
        Type::Oscill, QStringLiteral( "Oscill" ), []( int slot ) -> std::unique_ptr< Backend > {
            return std::make_unique< OscillBackend >( makeOscillTransport( slot ) );
        } );
}

} // namespace Instrument
