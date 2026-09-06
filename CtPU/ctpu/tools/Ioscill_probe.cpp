// SPDX-License-Identifier: GPL-3.0-or-later
// Last edited: 2026-09-06 UTC
//
// Замер на приборе Oscill: НАШИМ ЖЕ КОДОМ, а не отдельной реализацией.
//
// Распоряжение автора 2026-09-06: «на любые замеры ответ тестскриптом получен
// неоднократно. важно что бы ты написал его корректно коду. и получил
// искомое, против угаданного?»
//
// Отсюда устройство пробника. Он НЕ повторяет протокол своими средствами: он
// открывает `SerialTransport` и разговаривает через `OscillBackend` — те
// самые, что стоят в приложении. Поэтому его вывод отвечает не «что умеет
// прибор вообще», а «что получает НАШ КОД, когда разговаривает с прибором».
// Пробник, написанный мимо кода, доказал бы про прибор, а не про программу.
//
// ЧТО ИМЕННО СПРАШИВАЕТСЯ. Регистр `RS` — это ТРИ НЕЗАВИСИМЫХ ПЕРЕКЛЮЧАТЕЛЯ
// (бит 0 — реальное время либо стробоскоп, бит 1 — передача после оцифровки
// либо параллельно, бит 2 — буферная либо бесконечная), и выбираются они в
// приложении явно. Вопрос не в том, «что означают биты» — это установлено.
//
// Вопрос в том, КАКИЕ СОЧЕТАНИЯ ПРИБОР ДЕРЖИТ. Вендор говорит прямо: «регистр
// RS применяется только если при выбранном интервале выборок (регистр TS)
// возможны два (или более) способа оцифровки». То есть часть сочетаний прибор
// при текущем `TS` просто не примет — и вернёт не то, что записали.
//
// Ответ даёт сам прибор: после записи регистра он возвращает ФАКТИЧЕСКОЕ
// значение (принцип «прибор корректирует — клиент перечитывает»). Пробник
// пишет каждое сочетание и печатает, что вернулось, — при нескольких `TS`,
// потому что от `TS` ответ и зависит. Это и есть искомое против угаданного.
//
// ПРИБОР НЕ ПОРТИТСЯ. Значение `RS` снимается до опыта и возвращается после,
// что бы ни случилось в середине. Запуск программы не имеет права менять
// состояние железа — это правило проекта, и на пробник оно распространяется.
//
// Запуск:
//     Ioscill_probe.exe                 — перечислить порты и попробовать все
//     Ioscill_probe.exe COM3            — порт назван, скорость по умолчанию
//     Ioscill_probe.exe COM3 115200     — порт и скорость названы
//
// Вывод целиком уходит в стандартный поток: перенаправить в файл и положить
// в `inbox/`.

#include "instrument/oscill/oscillbackend.h"
#include "instrument/oscill/oscillprotocol.h"
#include "instrument/oscill/serialtransport.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QString>
#include <QStringList>
#include <QTextStream>

#include <memory>
#include <vector>

using namespace Instrument;

namespace {

QTextStream &out() {
    static QTextStream s( stdout );
    return s;
}

void line( const QString &s = QString() ) { out() << s << "\n" << Qt::flush; }

/// Скорости, которые стоит пробовать. 115200 — та, на которой открывают порт
/// обе эталонные реализации; 9600 — та, что названа в описании протокола как
/// начальная. Противоречие на бумаге не закрывается, поэтому пробуются обе.
const int kBauds[] = { 115200, 9600 };

/// Значения `RS`, о которых спор. Каждое сопровождено тем, откуда оно взято, —
/// чтобы в выводе было видно не только число, но и чьё это утверждение.
struct RsCandidate {
    uint8_t value;
    const char *why;
};
const RsCandidate kRsCandidates[] = {
    { 0x00, "реальное время, после оцифровки, буферная — простейшее" },
    { 0x01, "стробоскопическая (RIS)" },
    { 0x02, "реальное время, передача параллельно оцифровке" },
    { 0x03, "стробоскопическая + параллельная" },
    { 0x04, "буфер ROLL без параллельной передачи" },
    { 0x06, "ROLL + параллельная — ЭТО ПИШЕТ НАШ КОД для ленты" },
    { 0x07, "все три разом; прежняя редакция нашего кода писала это" },
};

QString hex2( uint32_t v ) { return QStringLiteral( "0x%1" ).arg( v, 2, 16, QLatin1Char( '0' ) ); }

/// Свойства прибора, которые стоит показать целиком: по ним автор сверит
/// прибор с руководством, а мы — наш разбор с прибором.
void printPassport( OscillBackend &b ) {
    line( "--- ПАСПОРТ ПРИБОРА (свойства, только чтение) ---" );
    struct Item {
        Oscill::Property p;
        const char *note;
    };
    const Item items[] = {
        { Oscill::Property::MCd, "машинный такт по умолчанию, ед. 10 пс" },
        { Oscill::Property::MCl, "наименьший такт (разгон), ед. 10 пс" },
        { Oscill::Property::TOl, "наименьший период одиночной выборки, такты*256" },
        { Oscill::Property::TOv, "маска быстрых одиночных периодов" },
        { Oscill::Property::TMl, "наименьший период стробоскопической" },
        { Oscill::Property::TMh, "наибольший период стробоскопической" },
        { Oscill::Property::TPl, "наименьший период параллельной/бесконечной" },
        { Oscill::Property::TCh, "предвыборок до синхронизации, максимум" },
        { Oscill::Property::QSh, "наибольший размер массива" },
        { Oscill::Property::V1h, "наибольшая чувствительность канала" },
        { Oscill::Property::V1l, "наименьшая чувствительность канала" },
        { Oscill::Property::P1h, "наибольшее смещение, 1/256 диапазона" },
        { Oscill::Property::P1l, "наименьшее смещение, 1/256 диапазона" },
        { Oscill::Property::D1m, "конструктивная задержка канала, ед. 10 пс" },
    };
    for ( const Item &it : items ) {
        uint32_t v = 0;
        if ( b.readProperty( it.p, v ) )
            line( QStringLiteral( "  %1 = %2 (%3)  %4" )
                      .arg( QString::fromLatin1( Oscill::propertyName( it.p ) ), -5 )
                      .arg( v, 10 )
                      .arg( hex2( v ), -8 )
                      .arg( QString::fromUtf8( it.note ) ) );
        else
            line( QStringLiteral( "  %1 = НЕ ОТВЕТИЛ  %2" )
                      .arg( QString::fromLatin1( Oscill::propertyName( it.p ) ), -5 )
                      .arg( QString::fromUtf8( it.note ) ) );
    }

    // Версии узлов — ASCII по четыре байта. По ним модель и прошивка.
    const Oscill::Property vers[] = { Oscill::Property::VHD, Oscill::Property::VHA, Oscill::Property::VSD,
                                      Oscill::Property::VSI, Oscill::Property::VSC, Oscill::Property::VSO };
    for ( Oscill::Property p : vers ) {
        QString text;
        if ( b.readPropertyAscii( p, text ) )
            line( QStringLiteral( "  %1 = «%2»" ).arg( QString::fromLatin1( Oscill::propertyName( p ) ), -5, QLatin1Char( ' ' ) ).arg( text ) );
    }
    line();
}

/// Тот самый замер, ради которого пробник и написан.
void probeRs( OscillBackend &b ) {
    line( "--- РЕГИСТР RS: КАКИЕ СОЧЕТАНИЯ ПРИБОР ДЕРЖИТ ---" );
    line( "RS — три независимых переключателя: бит 0 реальное время/стробоскоп," );
    line( "бит 1 передача после оцифровки/параллельно, бит 2 буферная/бесконечная." );
    line( "Вендор: «регистр RS применяется только если при выбранном интервале" );
    line( "выборок (TS) возможны два или более способа оцифровки» — значит часть" );
    line( "сочетаний прибор при текущем TS не примет и вернёт другое." );
    line( "Столбец «принял» — ответ ПРИБОРА, а не наше толкование." );
    line();

    uint32_t saved = 0;
    const bool haveSaved = b.readRegister( Oscill::Register::RS, saved );
    if ( haveSaved )
        line( QStringLiteral( "  исходное значение RS = %1 — будет возвращено в конце" ).arg( hex2( saved ) ) );
    else
        line( "  ВНИМАНИЕ: исходное RS прочитать не удалось; восстанавливать нечего" );
    line();

    // Период выборки снимается и возвращается так же, как RS: он влияет на
    // ответ, и менять его насовсем пробник права не имеет.
    uint32_t savedTs = 0;
    const bool haveTs = b.readRegister( Oscill::Register::TS, savedTs );

    // Три периода: тот, что стоит сейчас, и по краю в обе стороны. Вендор
    // прямо связывает набор доступных способов с `TS`, поэтому один период
    // ответа не даёт — он даёт ответ ДЛЯ ЭТОГО ПЕРИОДА.
    std::vector< uint32_t > tsList;
    if ( haveTs ) {
        tsList.push_back( savedTs );
        tsList.push_back( savedTs * 256u > savedTs ? savedTs * 256u : savedTs );
        if ( savedTs > 256u )
            tsList.push_back( savedTs / 256u );
    } else {
        tsList.push_back( 0 ); // TS не тронем вовсе
    }

    for ( uint32_t ts : tsList ) {
        if ( haveTs ) {
            uint32_t tsActual = 0;
            if ( !b.writeRegister( Oscill::Register::TS, ts, tsActual ) ) {
                line( QStringLiteral( "  TS = %1 — записать не удалось, сочетания при нём не проверяются" ).arg( ts ) );
                continue;
            }
            line( QStringLiteral( "  При TS: просили %1, прибор держит %2" ).arg( ts ).arg( tsActual ) );
        }
        line( QStringLiteral( "  %1 %2 %3 %4" )
                  .arg( QStringLiteral( "просили" ), -8 )
                  .arg( QStringLiteral( "принял" ), -8 )
                  .arg( QStringLiteral( "лента?" ), -8 )
                  .arg( QStringLiteral( "что это за сочетание" ) ) );

        for ( const RsCandidate &c : kRsCandidates ) {
            uint32_t actual = 0;
            if ( !b.writeRegister( Oscill::Register::RS, c.value, actual ) ) {
                line( QStringLiteral( "  %1 %2 %3 %4" )
                          .arg( hex2( c.value ), -8 )
                          .arg( QStringLiteral( "ОТКАЗ" ), -8 )
                          .arg( QStringLiteral( "—" ), -8 )
                          .arg( QString::fromUtf8( c.why ) ) );
                continue;
            }
            const Oscill::AcquisitionMode m = Oscill::decodeRs( uint8_t( actual ) );
            line( QStringLiteral( "  %1 %2 %3 %4%5" )
                      .arg( hex2( c.value ), -8 )
                      .arg( hex2( actual ), -8 )
                      .arg( m.isRoll() ? QStringLiteral( "да" ) : QStringLiteral( "нет" ), -8 )
                      .arg( QString::fromUtf8( c.why ) )
                      .arg( actual == c.value ? QString() : QStringLiteral( "   <-- ПРИБОР ПОПРАВИЛ" ) ) );
        }
        line();
    }

    if ( haveTs ) {
        uint32_t tsBack = 0;
        if ( b.writeRegister( Oscill::Register::TS, savedTs, tsBack ) )
            line( QStringLiteral( "  TS возвращено: просили %1, прибор держит %2" ).arg( savedTs ).arg( tsBack ) );
        else
            line( QStringLiteral( "  ВНИМАНИЕ: вернуть TS = %1 НЕ УДАЛОСЬ" ).arg( savedTs ) );
    }

    if ( haveSaved ) {
        uint32_t back = 0;
        if ( b.writeRegister( Oscill::Register::RS, saved, back ) )
            line( QStringLiteral( "  RS возвращено: просили %1, прибор держит %2" ).arg( hex2( saved ), hex2( back ) ) );
        else
            line( QStringLiteral( "  ВНИМАНИЕ: вернуть RS = %1 НЕ УДАЛОСЬ" ).arg( hex2( saved ) ) );
    }
    line();
}

bool probePort( const QString &port, int baud ) {
    SerialParams params;
    params.port = port;
    params.baudRate = baud;
    params.readTimeoutMs = 300;

    auto transport = std::unique_ptr< SerialTransport >( new SerialTransport( params ) );
    SerialTransport *raw = transport.get();
    OscillBackend backend( TransportPtr( transport.release() ) );

    if ( !backend.link() ) {
        line( QStringLiteral( "  %1 @ %2 — связи нет: %3" )
                  .arg( port )
                  .arg( baud )
                  .arg( backend.state().lastError.isEmpty() ? QStringLiteral( "прибор не ответил" )
                                                            : backend.state().lastError ) );
        return false;
    }

    line();
    line( QStringLiteral( "=== ПРИБОР ОТВЕТИЛ: %1 ===" ).arg( raw->description() ) );
    line( QStringLiteral( "модель по паспорту: «%1»" ).arg( backend.state().model ) );
    line();

    printPassport( backend );
    probeRs( backend );

    backend.unlink();
    return true;
}

} // namespace


int main( int argc, char *argv[] ) {
    QCoreApplication app( argc, argv );
    const QStringList args = QCoreApplication::arguments();

    line( "== ЗАМЕР НА ПРИБОРЕ OSCILL ==" );
    line( "Пробник разговаривает с прибором ТЕМ ЖЕ КОДОМ, что и приложение:" );
    line( "SerialTransport + OscillBackend. Поэтому его вывод говорит о нашей" );
    line( "программе, а не только о приборе." );
    line();

    std::vector< QString > ports;
    int fixedBaud = 0;
    if ( args.size() > 1 ) {
        ports.push_back( args.at( 1 ) );
        if ( args.size() > 2 )
            fixedBaud = args.at( 2 ).toInt();
    } else {
        ports = SerialTransport::enumerate();
        line( QStringLiteral( "Порты, найденные в системе: %1" )
                  .arg( ports.empty() ? QStringLiteral( "ни одного" ) : QStringLiteral( "%1" ).arg( int( ports.size() ) ) ) );
        for ( const QString &p : ports )
            line( QStringLiteral( "  %1" ).arg( p ) );
        line();
        line( "Прибор всегда доступен по COM-порту — и через преобразователь," );
        line( "и по Bluetooth SPP (со слов автора, 2026-09-06). Поэтому портов" );
        line( "может быть несколько, и пробуются все." );
        line();
    }

    if ( ports.empty() ) {
        line( "ИТОГ: портов нет — замерять нечего. Прибор не подключён?" );
        return 1;
    }

    bool answered = false;
    for ( const QString &p : ports ) {
        if ( fixedBaud ) {
            answered = probePort( p, fixedBaud ) || answered;
            continue;
        }
        for ( int b : kBauds )
            if ( probePort( p, b ) ) {
                answered = true;
                break; // на этом порту прибор нашёлся, вторую скорость не трогаем
            }
    }

    line();
    if ( answered ) {
        line( "ИТОГ: прибор ответил, замер снят. Строка «принял» в таблице RS —" );
        line( "и есть искомое: она говорит, какое значение прибор ДЕРЖИТ, а не" );
        line( "какое мы предполагали." );
        return 0;
    }
    line( "ИТОГ: ни один порт не ответил как Oscill." );
    line( "Это не обязательно отказ прибора: порт мог быть занят другой" );
    line( "программой. Многосвязность к разным приложениям не проверялась" );
    line( "никогда (со слов автора), и здесь она тоже не проверяется." );
    return 1;
}
