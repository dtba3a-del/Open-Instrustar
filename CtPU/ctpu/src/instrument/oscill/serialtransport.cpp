// SPDX-License-Identifier: GPL-3.0-or-later

/// \file serialtransport.cpp
/// \brief Последовательный порт: Win32 и POSIX за одним разделяемым швом.
///
/// ГДЕ ПРОХОДИТ ШОВ И ПОЧЕМУ ИМЕННО ТАМ
///
/// Платформенного кода здесь ровно столько, сколько его есть на самом
/// деле: дескриптор, настройка линии, ожидание, чтение, запись, опрос
/// очереди, перечисление портов. Всё остальное — умолчания, проверки
/// доводов, сроки, склейка `readExactly()` — написано ОДИН раз в общей
/// части. Разделение сделано так, что обе реализации `Impl` предъявляют
/// наверх одну и ту же шестёрку действий; если бы шов прошёл выше, по
/// публичным методам, каждая правка политики ожидания требовала бы двух
/// одинаковых правок в двух ветках — и рано или поздно они разошлись бы.
/// Это тот же запрет на второе место, порождающее величину, что и в
/// `docs/ДВОЕВЛАСТИЕ.md`, только внутри одного файла.
///
/// ЕДИНАЯ СЕМАНТИКА ОЖИДАНИЯ, СВЕДЁННАЯ К ОДНОЙ ФУНКЦИИ
///
/// `Impl::readSome( dst, want, totalMs )` на обеих платформах значит одно
/// и то же: ждать ПЕРВОГО байта не дольше `totalMs`, а после того как
/// очередь пошла — добирать её, пока паузы короче `interByteTimeoutMs`,
/// но не больше `want` байт. На Win32 это ровно одна комбинация
/// `COMMTIMEOUTS` и один `ReadFile`; на POSIX — цикл `poll()`.
/// Совпадение здесь не косметическое: на этой функции стоит правило
/// первоисточника «ноль полученных байт есть отсутствие ответа, хотя бы
/// один байт есть ошибочный ответ», и если бы платформы понимали ожидание
/// по-разному, перезапрос `0x92` срабатывал бы на одной машине и не
/// срабатывал на другой.
///
/// `totalMs == 0` — не вырожденный случай, а рабочий: он значит «забрать
/// то, что уже лежит, и вернуться». На нём стоит неблокирующий шаг
/// `update()` бэкенда. Поэтому в этом случае добирание НЕ делается вовсе:
/// у нас просили не ждать, а межбайтовая пауза есть ожидание.
///
/// ПОЧЕМУ VMIN/VTIME НЕ СЛУЖАТ ТАЙМЕРОМ
///
/// Линия настраивается через `termios`, и `VMIN`/`VTIME` выставлены в
/// `0`/`0` — то есть «чтение возвращает то, что есть, и не ждёт». Сроки
/// меряет `poll()`, а не `VTIME`, по одной причине: единица `VTIME` —
/// одна десятая секунды. Межбайтовый срок по умолчанию равен 20 мс и в
/// этой единице не представим вовсе, а округление его до 100 мс завело бы
/// у величины второе значение — объявленное в `SerialParams` и
/// действующее на линии. Расходятся такие пары не сразу и молча.
///
/// `lastError()` ОПИСЫВАЕТ ПОСЛЕДНЮЮ ОПЕРАЦИЮ, А НЕ ВСЮ СЕССИЮ
///
/// Каждый публичный метод, способный отказать, чистит запись об ошибке на
/// входе и заполняет её при отказе. Иначе `read()` становится непригоден:
/// возврат у него один и тот же — пустой `QByteArray` — и для законного
/// молчания прибора, и для отказа линии. Различить их можно только по
/// записи, а запись, оставшаяся от прошлой операции, превратила бы
/// молчание в ложную поломку.
///
/// ОПОЗНАНИЕ ПРИБОРА ПО VID:PID ЗДЕСЬ НЕ ДЕЛАЕТСЯ
///
/// Мост Oscill — CP210x, `10c4:840E`. Отбор по этой паре сюда не входит и
/// входить не может: `enumerate()` по контракту отдаёт имена портов,
/// которые показывает система, а `exists()` принимает уже готовое имя —
/// места для признака устройства в подписях нет. Отбор принадлежит слою
/// выше (реестр параметров слота и `probe()` бэкенда), а прочитать пару
/// можно, не трогая этот файл: на Linux — из
/// `/sys/class/tty/<имя>/device/../{idVendor,idProduct}`, на Windows — из
/// свойства `SPDRP_HARDWAREID` (`USB\\VID_10C4&PID_840E`) через SetupAPI.
/// SetupAPI не берётся ещё и потому, что потребовал бы новой библиотеки в
/// линковке, объявленной и приложению, и тестам, — ровно того двоевластия
/// в сборке, ради ухода от которого здесь не взят и `Qt5SerialPort`.
/// Ветка реестра `HARDWARE\\DEVICEMAP\\SERIALCOMM` даёт список портов и не
/// требует ни одной дополнительной библиотеки.
///
/// СОСТОЯНИЕ УТВЕРЖДЕНИЙ. Прибора Oscill в работе нет, поэтому ни одно
/// утверждение здесь не проверено линией. Сверх того ветки проверены
/// РАЗНОЙ мерой, и это различие важнее самих меток:
///
///   * **POSIX — [СБОРКА].** Собрана с `-Wall -Wextra -pedantic` без
///     замечаний и прогнана против псевдотерминала: открытие, отказ на
///     незаданной скорости, отказ на не-линии, приём и передача с
///     байтами `0x00`, `0x11`, `0x13`, `0x0D` без искажения, короткий
///     результат `readExactly()` по сроку, полный набор, `purge()`,
///     смена скорости и отказ на скорости вне таблицы.
///   * **`_WIN32` — ниже [СБОРКА].** Кросс-компилятора MinGW на машине
///     нет; ветка собрана лишь против подделки, повторяющей имена и
///     подписи Win32, то есть проверена на опечатки и на согласованность
///     самого кода, но НЕ против настоящих заголовков и тем более не в
///     работе. Первая сборка на целевой машине (MSYS2 MinGW64) есть
///     обязательная часть приёмки, а не формальность.
///
/// Разница мер названа здесь потому, что по внешнему виду файла она не
/// видна: обе ветки написаны одинаково уверенно, а подтверждены
/// по-разному.

#include "serialtransport.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace Instrument {

namespace {

/// Часы для сроков — монотонные. Настенные брать нельзя: перевод
/// системного времени назад посреди ожидания кадра растянул бы срок на
/// величину перевода, а вперёд — оборвал бы приём исправного ответа.
using Clock = std::chrono::steady_clock;

/// Сколько миллисекунд осталось до срока. Ноль значит «срок вышел» — и
/// это законное значение для `readSome()`: оно превращает ожидание в
/// однократный съём того, что уже лежит.
int msLeft( const Clock::time_point &deadline ) {
    const auto left = std::chrono::duration_cast< std::chrono::milliseconds >( deadline - Clock::now() ).count();
    if ( left <= 0 ) {
        return 0;
    }
    // Верхняя отсечка нужна не от долгих сроков, а от переполнения при
    // приведении к int: срок задаёт вызывающий, и он не обязан быть
    // разумным.
    constexpr long long LIMIT = 0x7FFFFFFF;
    return int( left > LIMIT ? LIMIT : left );
}

/// Имя порта, разложенное на буквенную часть и хвостовой номер.
void splitPortName( const QString &name, QString &head, long long &number ) {
    int i = name.size();
    while ( i > 0 && name.at( i - 1 ).isDigit() ) {
        --i;
    }
    head = name.left( i );
    number = ( i < name.size() ) ? name.mid( i ).toLongLong() : -1;
}

/// Порядок портов «как читает человек»: `COM9` раньше `COM10`, а не
/// наоборот. Список идёт пользователю в выбор порта, и посимвольный
/// порядок там читается как ошибка программы, потому что ею и является.
bool portLess( const QString &a, const QString &b ) {
    QString ha;
    QString hb;
    long long na = 0;
    long long nb = 0;
    splitPortName( a, ha, na );
    splitPortName( b, hb, nb );
    if ( ha != hb ) {
        return ha < hb;
    }
    return na < nb;
}

/// Похоже ли имя на конец Bluetooth SPP. Признак есть только на POSIX:
/// там канал SPP получает собственное имя `/dev/rfcommN`. На Windows SPP
/// выдаётся обычным `COMn`, неотличимым от порта преобразователя, и
/// подписи там не будет — подпись, которая врёт через раз, хуже, чем её
/// отсутствие.
bool looksLikeSpp( const QString &port ) {
    return port.contains( QStringLiteral( "rfcomm" ), Qt::CaseInsensitive );
}

} // namespace

// ===========================================================================
// Платформенная часть: Windows
// ===========================================================================

#ifdef _WIN32

struct SerialTransport::Impl {
    SerialParams params;
    HANDLE handle = INVALID_HANDLE_VALUE;
    QString lastError;

    bool isOpen() const { return handle != INVALID_HANDLE_VALUE; }

    /// Текст системной ошибки. Код без текста в журнале бесполезен: «не
    /// удалось открыть COM7» и «не удалось открыть COM7: доступ запрещён»
    /// приводят к разным действиям оператора.
    static QString systemError( DWORD code ) {
        wchar_t *text = nullptr;
        const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
        const DWORD chars = FormatMessageW( flags, nullptr, code, 0, reinterpret_cast< wchar_t * >( &text ), 0, nullptr );
        QString out;
        if ( chars != 0 && text != nullptr ) {
            out = QString::fromWCharArray( text, int( chars ) ).trimmed();
        } else {
            out = QStringLiteral( "системный код %1" ).arg( ulong( code ) );
        }
        if ( text != nullptr ) {
            LocalFree( text );
        }
        return out;
    }

    /// Записать отказ вместе с системной причиной. Зовётся немедленно
    /// после отказавшего вызова: `GetLastError()` живёт до следующего
    /// вызова любой функции, которая его переустановит.
    void fail( const QString &what ) { lastError = what + QStringLiteral( ": " ) + systemError( GetLastError() ); }

    /// Имя устройства в форме `\\.\COMn`. Разветвления по номеру порта
    /// нет намеренно: без префикса достижимы только `COM1`…`COM9`
    /// (`COM10` разбирается как имя файла в текущем каталоге), а с
    /// префиксом достижимы все. Одна форма на все случаи — это на одно
    /// место меньше, где можно ошибиться.
    static std::wstring devicePath( const QString &port ) {
        QString full = port;
        if ( !full.startsWith( QStringLiteral( "\\\\.\\" ) ) ) {
            full = QStringLiteral( "\\\\.\\" ) + full;
        }
        return std::wstring( reinterpret_cast< const wchar_t * >( full.utf16() ), size_t( full.size() ) );
    }

    /// Настроить линию: 8N1, без всякого управления потоком.
    ///
    /// Управление потоком выключено не «для простоты»: OBEX прибора
    /// двоичный, и программное управление (`XON`/`XOFF`) съело бы из
    /// потока байты `0x11` и `0x13` — а `0x11` есть половина значения
    /// любого регистра. Аппаратное выключено потому, что линии `CTS`/`DSR`
    /// у моста CP210x при выключенном приборе висят в состоянии, при
    /// котором передача не начнётся никогда, и отказ выглядел бы как
    /// молчание прибора.
    bool applyLine( int baud ) {
        DCB dcb;
        ZeroMemory( &dcb, sizeof dcb );
        dcb.DCBlength = sizeof dcb;
        if ( GetCommState( handle, &dcb ) == 0 ) {
            // Сюда же приходит открытый не-COM объект: `\\.\C:` открылся
            // бы успешно, а состояния линии у него нет.
            fail( QStringLiteral( "порт открыт, но это не последовательная линия" ) );
            return false;
        }
        dcb.BaudRate = DWORD( baud );
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fBinary = TRUE;
        dcb.fParity = FALSE;
        dcb.fOutxCtsFlow = FALSE;
        dcb.fOutxDsrFlow = FALSE;
        dcb.fDsrSensitivity = FALSE;
        dcb.fOutX = FALSE;
        dcb.fInX = FALSE;
        dcb.fTXContinueOnXoff = TRUE;
        dcb.fErrorChar = FALSE;
        // `fNull = TRUE` заставляет драйвер ВЫБРАСЫВАТЬ принятые нулевые
        // байты. В OBEX нулей полно: пустой пакет `FF 00 03` несёт их два,
        // а старший байт длины у коротких ответов всегда ноль. С этим
        // флагом линия отдавала бы разобранный мусор вместо пакетов.
        dcb.fNull = FALSE;
        // Обе линии подняты: мост держит их опущенными после открытия, а
        // приборы, у которых передатчик заперт по DTR, при опущенной
        // линии молчат — и молчание это неотличимо от отсутствия прибора.
        // Первоисточник состояния этих линий не задаёт; выбор явный. [СБОРКА]
        dcb.fDtrControl = DTR_CONTROL_ENABLE;
        dcb.fRtsControl = RTS_CONTROL_ENABLE;
        // `fAbortOnError = TRUE` переводит порт после ЛЮБОЙ ошибки приёма
        // в состояние, где все операции отказывают до `ClearCommError()`.
        // Одна помеха на линии выглядела бы как обрыв связи навсегда.
        dcb.fAbortOnError = FALSE;
        if ( SetCommState( handle, &dcb ) == 0 ) {
            fail( QStringLiteral( "не удалось задать параметры линии" ) );
            return false;
        }
        // Перечитать и сверить. Драйвер вправе подставить ближайшую
        // достижимую скорость и отчитаться об успехе; расхождение частот
        // рвёт обмен не сразу, а на длинных пакетах, и выглядит как
        // испорченный ответ, а не как неверная настройка.
        DCB back;
        ZeroMemory( &back, sizeof back );
        back.DCBlength = sizeof back;
        if ( GetCommState( handle, &back ) == 0 ) {
            fail( QStringLiteral( "не удалось перечитать параметры линии" ) );
            return false;
        }
        if ( back.BaudRate != DWORD( baud ) ) {
            lastError = QStringLiteral( "скорость не принята: просили %1, установлено %2" )
                            .arg( baud )
                            .arg( ulong( back.BaudRate ) );
            return false;
        }
        if ( back.ByteSize != 8 || back.Parity != NOPARITY || back.StopBits != ONESTOPBIT ) {
            lastError = QStringLiteral( "линия настроена не как 8N1" );
            return false;
        }
        params.baudRate = baud;
        return true;
    }

    /// Сроки чтения и записи. `totalMs <= 0` собирается в единственную
    /// комбинацию Win32, которая возвращает уже принятое НЕМЕДЛЕННО:
    /// межбайтовый срок `MAXDWORD` при обоих нулевых полных сроках.
    bool applyTimeouts( int totalMs ) {
        COMMTIMEOUTS to;
        ZeroMemory( &to, sizeof to );
        if ( totalMs <= 0 ) {
            to.ReadIntervalTimeout = MAXDWORD;
            to.ReadTotalTimeoutMultiplier = 0;
            to.ReadTotalTimeoutConstant = 0;
        } else {
            // Межбайтовый счётчик Win32 запускается ПОСЛЕ первого
            // принятого байта — ровно та семантика, которая нужна:
            // первого байта ждём по полному сроку, а паузу внутри
            // начавшейся очереди меряем межбайтовым.
            to.ReadIntervalTimeout = DWORD( params.interByteTimeoutMs > 0 ? params.interByteTimeoutMs : 0 );
            to.ReadTotalTimeoutMultiplier = 0;
            to.ReadTotalTimeoutConstant = DWORD( totalMs );
        }
        to.WriteTotalTimeoutMultiplier = 0;
        to.WriteTotalTimeoutConstant = DWORD( params.writeTimeoutMs > 0 ? params.writeTimeoutMs : 0 );
        if ( SetCommTimeouts( handle, &to ) == 0 ) {
            fail( QStringLiteral( "не удалось задать сроки порта" ) );
            return false;
        }
        return true;
    }

    bool open() {
        close();
        const std::wstring path = devicePath( params.port );
        handle = CreateFileW( path.c_str(), GENERIC_READ | GENERIC_WRITE,
                              // Без разделения: второй хозяин той же линии
                              // посреди сессии по данным не обнаруживается —
                              // чужие байты неотличимы от искажённых своих.
                              0, nullptr, OPEN_EXISTING,
                              // Синхронный обмен: ожиданием управляют
                              // `COMMTIMEOUTS`. Перекрытый ввод-вывод здесь
                              // добавил бы событий и состояний, не добавив
                              // ни одной новой возможности.
                              0, nullptr );
        if ( handle == INVALID_HANDLE_VALUE ) {
            fail( QStringLiteral( "не удалось открыть %1" ).arg( params.port ) );
            return false;
        }
        // Драйверные буферы под объявленный хостом приёмник CONNECT
        // (4096 байт). Просьба, а не приказ: отказ означает лишь, что
        // драйвер оставил свои размеры, и сессию из-за него не рвём.
        SetupComm( handle, 4096, 4096 );
        if ( !applyLine( params.baudRate ) || !applyTimeouts( params.readTimeoutMs ) ) {
            const QString kept = lastError;
            close();
            lastError = kept;
            return false;
        }
        purge();
        return true;
    }

    void close() {
        if ( handle != INVALID_HANDLE_VALUE ) {
            CloseHandle( handle );
            handle = INVALID_HANDLE_VALUE;
        }
    }

    bool write( const char *data, int size ) {
        int done = 0;
        while ( done < size ) {
            DWORD put = 0;
            if ( WriteFile( handle, data + done, DWORD( size - done ), &put, nullptr ) == 0 ) {
                fail( QStringLiteral( "запись в порт" ) );
                return false;
            }
            if ( put == 0 ) {
                // Полный срок записи истёк, а очередь не сдвинулась.
                // Дописывать бессмысленно: пакет OBEX, ушедший наполовину,
                // прибор разберёт как другой пакет.
                lastError = QStringLiteral( "запись не двигается: истёк writeTimeoutMs (%1 мс)" ).arg( params.writeTimeoutMs );
                return false;
            }
            done += int( put );
        }
        return true;
    }

    int readSome( char *dst, int want, int totalMs ) {
        if ( !applyTimeouts( totalMs ) ) {
            return -1;
        }
        DWORD got = 0;
        if ( ReadFile( handle, dst, DWORD( want ), &got, nullptr ) == 0 ) {
            fail( QStringLiteral( "чтение из порта" ) );
            // Снять флаг ошибки приёма, иначе он остаётся у драйвера и
            // мешает следующим операциям на портах, где отказ был разовой
            // помехой.
            DWORD errors = 0;
            COMSTAT comStat;
            ClearCommError( handle, &errors, &comStat );
            return -1;
        }
        return int( got );
    }

    int available() const {
        DWORD errors = 0;
        COMSTAT comStat;
        if ( ClearCommError( handle, &errors, &comStat ) == 0 ) {
            return -1;
        }
        return int( comStat.cbInQue );
    }

    void purge() { PurgeComm( handle, PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT ); }

    bool setBaud( int baud ) {
        // Дождаться ухода того, что уже отдано драйверу. Смена скорости
        // при непустой очереди отправила бы хвост прошлого пакета на
        // новой скорости — прибор принял бы его как искажённый. К моменту
        // вызова очередь обычно пуста: команду смены скорости прибор уже
        // подтвердил, а подтверждать ему было нечего, пока байты не ушли.
        FlushFileBuffers( handle );
        const int previous = params.baudRate;
        if ( applyLine( baud ) ) {
            return true;
        }
        // Отказ мог наступить ПОСЛЕ того, как драйвер принял новую
        // скорость: сверка перечитанным состоянием отвергает и тот
        // случай, когда установка удалась не так, как просили. Тогда
        // линия стоит на неизвестной скорости, а `params` помнит
        // прежнюю — расхождение, которое обнаруживается только обрывом
        // связи. Поэтому прежняя скорость возвращается явно.
        const QString kept = lastError;
        if ( applyLine( previous ) ) {
            lastError = kept;
        } else {
            lastError = kept + QStringLiteral( "; вернуть %1 бод тоже не удалось: " ).arg( previous ) + lastError;
        }
        return false;
    }
};

bool SerialTransport::exists( const QString &port ) {
    if ( port.isEmpty() ) {
        return false;
    }
    // `QueryDosDeviceW` спрашивает таблицу имён устройств и НЕ открывает
    // порт. Это существенно: `probe()` не вправе ничего менять, а
    // открытие чужого занятого COM-порта есть вмешательство — на нём
    // может идти чужая сессия, которую наше открытие оборвёт.
    QString name = port;
    if ( name.startsWith( QStringLiteral( "\\\\.\\" ) ) ) {
        name = name.mid( 4 );
    }
    const std::wstring wide( reinterpret_cast< const wchar_t * >( name.utf16() ), size_t( name.size() ) );
    wchar_t target[ 512 ];
    if ( QueryDosDeviceW( wide.c_str(), target, DWORD( sizeof target / sizeof target[ 0 ] ) ) != 0 ) {
        return true;
    }
    // Имя есть, но его цель не поместилась в буфер: устройство
    // существует, и «не поместилось» — не «не найдено».
    return GetLastError() == ERROR_INSUFFICIENT_BUFFER;
}

std::vector< QString > SerialTransport::enumerate() {
    std::vector< QString > found;
    HKEY key = nullptr;
    // Ветка отсутствует, когда в системе нет ни одного последовательного
    // порта. Это пустой список, а не отказ перечисления: контракт требует
    // различать их, и здесь различие соблюдается тем, что оба случая
    // ведут к пустому списку честно — иных исходов у перечисления нет.
    if ( RegOpenKeyExW( HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &key ) != ERROR_SUCCESS ) {
        return found;
    }
    for ( DWORD index = 0;; ++index ) {
        wchar_t name[ 256 ];
        wchar_t data[ 256 ];
        DWORD nameChars = DWORD( sizeof name / sizeof name[ 0 ] );
        DWORD dataBytes = DWORD( sizeof data );
        DWORD type = 0;
        const LONG rc =
            RegEnumValueW( key, index, name, &nameChars, nullptr, &type, reinterpret_cast< BYTE * >( data ), &dataBytes );
        if ( rc != ERROR_SUCCESS ) {
            // И конец перечисления, и порча ветки прекращают обход. Домысливать
            // пропущенные значения нельзя: имя порта, угаданное наполовину,
            // приведёт к открытию не того устройства.
            break;
        }
        if ( type != REG_SZ ) {
            continue;
        }
        // Длина отдана в БАЙТАХ и может не включать завершающий ноль —
        // поэтому строка строится по длине, а потом обрезается по первому
        // нулю, если он всё-таки внутри.
        QString port = QString::fromWCharArray( data, int( dataBytes / sizeof( wchar_t ) ) );
        const int zero = port.indexOf( QChar( QChar::Null ) );
        if ( zero >= 0 ) {
            port.truncate( zero );
        }
        if ( !port.isEmpty() ) {
            found.push_back( port );
        }
    }
    RegCloseKey( key );
    std::sort( found.begin(), found.end(), portLess );
    return found;
}

#else

// ===========================================================================
// Платформенная часть: POSIX
// ===========================================================================

namespace {

/// «Сейчас нечего» против «отказ». На Linux `EWOULDBLOCK` и `EAGAIN` —
/// одно значение, но стандарт этого не обещает, а сравнивать их через
/// `||` при совпадении значений — писать заведомо мёртвую ветвь.
inline bool wouldBlock( int code ) {
    if ( code == EAGAIN ) {
        return true;
    }
#if defined( EWOULDBLOCK ) && ( EWOULDBLOCK != EAGAIN )
    if ( code == EWOULDBLOCK ) {
        return true;
    }
#endif
    return false;
}

/// Соответствие «скорость в бодах ↔ код `termios`». Таблица, а не
/// вычисление: `speed_t` у POSIX есть НОМЕР скорости, а не её значение, и
/// набор номеров задан системой. Скорости, которой в таблице нет, здесь
/// не подбирается ближайшая — по той же причине, по которой её не
/// подбирает кодек при выборе коэффициента прибора: хост открыл бы линию
/// на одной скорости, прибор перешёл бы на другую, и связь умерла бы
/// молча, а молчание линии неотличимо от молчания прибора.
struct BaudEntry {
    int value;
    speed_t code;
};

constexpr BaudEntry BAUD_TABLE[] = {
    { 1200, B1200 },
    { 2400, B2400 },
    { 4800, B4800 },
    { 9600, B9600 },
    { 19200, B19200 },
    { 38400, B38400 },
// Всё, что выше 38400, POSIX не обещает: это расширения Linux и BSD.
// Отсутствующая на системе скорость обязана выпасть из таблицы на
// сборке, а не превратиться в отказ на линии.
#ifdef B57600
    { 57600, B57600 },
#endif
#ifdef B115200
    { 115200, B115200 },
#endif
#ifdef B230400
    { 230400, B230400 },
#endif
#ifdef B460800
    { 460800, B460800 },
#endif
#ifdef B500000
    { 500000, B500000 },
#endif
#ifdef B576000
    { 576000, B576000 },
#endif
#ifdef B921600
    { 921600, B921600 },
#endif
};

} // namespace

struct SerialTransport::Impl {
    SerialParams params;
    int fd = -1;
    QString lastError;

    bool isOpen() const { return fd >= 0; }

    static QString systemError() { return QString::fromLocal8Bit( std::strerror( errno ) ); }

    /// Зовётся немедленно после отказавшего вызова: `errno` переживает
    /// только удачные вызовы, а не любые.
    void fail( const QString &what ) { lastError = what + QStringLiteral( ": " ) + systemError(); }

    static bool baudCode( int baud, speed_t &code ) {
        for ( const BaudEntry &e : BAUD_TABLE ) {
            if ( e.value == baud ) {
                code = e.code;
                return true;
            }
        }
        return false;
    }

    /// Настроить линию: сырой режим, 8N1, без управления потоком.
    ///
    /// Режим собирается снятием флагов поимённо, а не `cfmakeraw()`:
    /// последняя не входит в POSIX, а главное — прячет ровно те флаги,
    /// из-за которых двоичный поток портится молча (`ICRNL` подменяет
    /// `0x0D` на `0x0A`, `ISTRIP` срезает восьмой бит, `IXON` съедает
    /// `0x11` и `0x13` — а `0x11` есть половина значения любого регистра).
    bool applyLine( int baud ) {
        speed_t code = B0;
        if ( !baudCode( baud, code ) ) {
            lastError = QStringLiteral( "скорость %1 бод системе неизвестна; ближайшая не подставляется" ).arg( baud );
            return false;
        }
        termios tio;
        std::memset( &tio, 0, sizeof tio );
        if ( tcgetattr( fd, &tio ) != 0 ) {
            fail( QStringLiteral( "не удалось прочитать параметры линии" ) );
            return false;
        }
        tio.c_iflag &= ~( IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON | IXOFF | IXANY );
        tio.c_oflag &= ~OPOST;
        tio.c_lflag &= ~( ECHO | ECHOE | ECHONL | ICANON | ISIG | IEXTEN );
        tio.c_cflag &= ~( CSIZE | PARENB | CSTOPB );
#ifdef CRTSCTS
        // Аппаратное управление потоком выключено: при выключенном
        // приборе `CTS` у моста висит в состоянии, при котором передача
        // не начнётся никогда, и отказ выглядел бы как молчание прибора.
        tio.c_cflag &= ~CRTSCTS;
#endif
        // `CLOCAL` — не ждать сигнала носителя (`DCD`); у виртуального
        // порта его может не быть вовсе, и без этого флага открытие или
        // чтение повисло бы. `CREAD` — приёмник включён.
        tio.c_cflag |= ( CS8 | CREAD | CLOCAL );
        // Ожидание меряет `poll()`, а не `VTIME`: единица `VTIME` — 0,1 с,
        // и межбайтовый срок в 20 мс в ней не представим. Пара 0/0 значит
        // «вернуть то, что есть, и не ждать».
        tio.c_cc[ VMIN ] = 0;
        tio.c_cc[ VTIME ] = 0;
        if ( cfsetispeed( &tio, code ) != 0 || cfsetospeed( &tio, code ) != 0 ) {
            fail( QStringLiteral( "не удалось задать скорость линии" ) );
            return false;
        }
        if ( tcsetattr( fd, TCSANOW, &tio ) != 0 ) {
            fail( QStringLiteral( "не удалось задать параметры линии" ) );
            return false;
        }
        // Перечитать и сверить обязательно: `tcsetattr()` отчитывается об
        // успехе, если удалась ХОТЯ БЫ ЧАСТЬ изменений. Драйвер, не
        // умеющий запрошенной скорости, оставит прежнюю и вернёт ноль —
        // расхождение частот проявится потом, на длинных пакетах, и будет
        // выглядеть как порча ответа, а не как неверная настройка.
        termios back;
        std::memset( &back, 0, sizeof back );
        if ( tcgetattr( fd, &back ) != 0 ) {
            fail( QStringLiteral( "не удалось перечитать параметры линии" ) );
            return false;
        }
        if ( cfgetispeed( &back ) != code || cfgetospeed( &back ) != code ) {
            lastError = QStringLiteral( "скорость %1 бод линией не принята" ).arg( baud );
            return false;
        }
        if ( ( back.c_cflag & CSIZE ) != CS8 || ( back.c_cflag & PARENB ) != 0 || ( back.c_cflag & CSTOPB ) != 0 ) {
            lastError = QStringLiteral( "линия настроена не как 8N1" );
            return false;
        }
        if ( back.c_cc[ VMIN ] != 0 || back.c_cc[ VTIME ] != 0 ) {
            lastError = QStringLiteral( "линия не приняла неблокирующий режим чтения (VMIN/VTIME)" );
            return false;
        }
        params.baudRate = baud;
        return true;
    }

    bool open() {
        close();
        const QByteArray path = params.port.toLocal8Bit();
        int handle = -1;
        for ( ;; ) {
            // `O_NOCTTY` — не делать порт управляющим терминалом
            // процесса: иначе сигнал с линии (разрыв, `^C` в потоке
            // данных) достался бы приложению как сигнал терминала.
            // `O_NONBLOCK` — не ждать носителя при открытии и держать
            // чтение неблокирующим; сроками управляет `poll()`.
            handle = ::open( path.constData(), O_RDWR | O_NOCTTY | O_NONBLOCK );
            if ( handle >= 0 || errno != EINTR ) {
                break;
            }
        }
        if ( handle < 0 ) {
            fail( QStringLiteral( "не удалось открыть %1" ).arg( params.port ) );
            return false;
        }
        fd = handle;
#ifdef TIOCEXCL
        // Просьба об исключительном владении. Второй хозяин той же линии
        // посреди сессии по данным не обнаруживается: чужие байты
        // неотличимы от искажённых своих. Отказ просьбы сессию не рвёт —
        // на системах без этого запроса его просто нет.
        ioctl( fd, TIOCEXCL );
#endif
        if ( isatty( fd ) == 0 ) {
            // Обычный файл с тем же именем открылся бы молча и молча
            // принял бы всё, что в него написано, а ответа не дал бы
            // никогда: отказ линии выглядел бы как молчание прибора.
            lastError = QStringLiteral( "%1 не является последовательной линией" ).arg( params.port );
            close();
            return false;
        }
        if ( !applyLine( params.baudRate ) ) {
            const QString kept = lastError;
            close();
            lastError = kept;
            return false;
        }
        purge();
        return true;
    }

    void close() {
        if ( fd >= 0 ) {
            ::close( fd );
            fd = -1;
        }
    }

    /// Ожидание готовности. Возврат: 1 — готово, 0 — срок вышел,
    /// −1 — отказ.
    int wait( short events, int ms ) const {
        // Отрицательный срок для `poll()` значит «ждать без конца». Ни
        // один вызывающий такого не передаёт, но цена ошибки здесь —
        // навсегда замерший опрос, а цена проверки — одна строка.
        if ( ms < 0 ) {
            ms = 0;
        }
        const auto deadline = Clock::now() + std::chrono::milliseconds( ms );
        for ( ;; ) {
            pollfd pfd;
            pfd.fd = fd;
            pfd.events = events;
            pfd.revents = 0;
            const int rc = ::poll( &pfd, 1, ms );
            if ( rc < 0 ) {
                if ( errno == EINTR ) {
                    // Сигнал — не тишина. Ждём остаток срока, а не срок
                    // заново: иначе поток сигналов растягивал бы ожидание
                    // без предела.
                    ms = msLeft( deadline );
                    continue;
                }
                return -1;
            }
            if ( rc == 0 ) {
                return 0;
            }
            // `POLLERR`/`POLLNVAL` на последовательной линии значат, что
            // порт исчез — выдернут преобразователь. Это отказ, а не
            // молчание, и он обязан быть виден: молчание прибора законно,
            // исчезновение порта — нет.
            if ( ( pfd.revents & ( POLLERR | POLLNVAL ) ) != 0 ) {
                return -1;
            }
            return 1;
        }
    }

    /// Один заход в ядро. Возврат: сколько прочитано (0 — сейчас нечего),
    /// −1 — отказ.
    int readRaw( char *dst, int want ) {
        for ( ;; ) {
            const ssize_t n = ::read( fd, dst, size_t( want ) );
            if ( n >= 0 ) {
                return int( n );
            }
            if ( errno == EINTR ) {
                continue;
            }
            if ( wouldBlock( errno ) ) {
                return 0;
            }
            fail( QStringLiteral( "чтение из порта" ) );
            return -1;
        }
    }

    int readSome( char *dst, int want, int totalMs ) {
        const int ready = wait( POLLIN, totalMs > 0 ? totalMs : 0 );
        if ( ready < 0 ) {
            fail( QStringLiteral( "ожидание данных на порту" ) );
            return -1;
        }
        if ( ready == 0 ) {
            return 0;
        }
        int got = readRaw( dst, want );
        if ( got < 0 ) {
            return -1;
        }
        // Догребание начавшейся очереди — только если ждать вообще
        // разрешено. При `totalMs == 0` обещано «то, что уже лежит»;
        // добирать значило бы ждать там, где просили не ждать, и
        // неблокирующий шаг опроса перестал бы быть неблокирующим.
        while ( totalMs > 0 && got < want ) {
            const int more = wait( POLLIN, params.interByteTimeoutMs > 0 ? params.interByteTimeoutMs : 0 );
            if ( more <= 0 ) {
                break;
            }
            const int n = readRaw( dst + got, want - got );
            if ( n <= 0 ) {
                break;
            }
            got += n;
        }
        return got;
    }

    bool write( const char *data, int size ) {
        const auto deadline = Clock::now() + std::chrono::milliseconds( params.writeTimeoutMs > 0 ? params.writeTimeoutMs : 0 );
        int done = 0;
        while ( done < size ) {
            const ssize_t n = ::write( fd, data + done, size_t( size - done ) );
            if ( n > 0 ) {
                done += int( n );
                continue;
            }
            if ( n < 0 && errno == EINTR ) {
                continue;
            }
            if ( n < 0 && !wouldBlock( errno ) ) {
                fail( QStringLiteral( "запись в порт" ) );
                return false;
            }
            // Очередь передатчика полна. Ждём готовности, но не дольше
            // объявленного срока: пакет OBEX, ушедший наполовину, прибор
            // разберёт как другой пакет, поэтому «сколько удалось» наверх
            // не выдаётся — выдаётся отказ.
            const int left = msLeft( deadline );
            if ( left == 0 || wait( POLLOUT, left ) <= 0 ) {
                lastError = QStringLiteral( "запись не уложилась в writeTimeoutMs (%1 мс): отдано %2 из %3 байт" )
                                .arg( params.writeTimeoutMs )
                                .arg( done )
                                .arg( size );
                return false;
            }
        }
        return true;
    }

    int available() const {
#ifdef FIONREAD
        int count = 0;
        if ( ioctl( fd, FIONREAD, &count ) != 0 ) {
            return -1;
        }
        return count;
#else
        // Спросить нечем. Ноль здесь был бы враньём: «нечего читать» и
        // «не смог спросить» — разные исходы, и контракт требует их
        // различать.
        return -1;
#endif
    }

    void purge() { tcflush( fd, TCIOFLUSH ); }

    bool setBaud( int baud ) {
        // Дождаться ухода того, что уже отдано драйверу. Смена скорости
        // при непустой очереди отправила бы хвост прошлого пакета на
        // новой скорости, и прибор принял бы его как искажённый. К моменту
        // вызова очередь обычно пуста: команду смены скорости прибор уже
        // подтвердил, а подтверждать ему было нечего, пока байты не ушли.
        while ( tcdrain( fd ) != 0 && errno == EINTR ) {
        }
        const int previous = params.baudRate;
        if ( applyLine( baud ) ) {
            return true;
        }
        // Отказ мог наступить ПОСЛЕ того, как драйвер принял новую
        // скорость: `tcsetattr()` отчитывается об успехе, если удалась
        // хотя бы часть изменений, и сверка перечитанным состоянием
        // отвергает в том числе такой исход. Тогда линия стоит на
        // неизвестной скорости, а `params` помнит прежнюю — расхождение,
        // которое обнаруживается только обрывом связи. Поэтому прежняя
        // скорость возвращается явно.
        const QString kept = lastError;
        if ( applyLine( previous ) ) {
            lastError = kept;
        } else {
            lastError = kept + QStringLiteral( "; вернуть %1 бод тоже не удалось: " ).arg( previous ) + lastError;
        }
        return false;
    }
};

bool SerialTransport::exists( const QString &port ) {
    if ( port.isEmpty() ) {
        return false;
    }
    // `stat()` спрашивает каталог устройств и НЕ открывает порт. Это
    // существенно: `probe()` не вправе ничего менять, а открытие чужого
    // занятого порта есть вмешательство — на нём может идти чужая сессия.
    struct stat info;
    if ( ::stat( port.toLocal8Bit().constData(), &info ) != 0 ) {
        return false;
    }
    // Именно символьное устройство: обычный файл с тем же именем есть, но
    // прибором не является.
    return S_ISCHR( info.st_mode );
}

std::vector< QString > SerialTransport::enumerate() {
    std::vector< QString > found;
    DIR *dir = ::opendir( "/dev" );
    if ( dir == nullptr ) {
        return found;
    }
    while ( const dirent *entry = ::readdir( dir ) ) {
        const QString name = QString::fromLocal8Bit( entry->d_name );
        // `ttyUSB` — мосты на драйверах usbserial (сюда попадает CP210x
        // прибора), `ttyACM` — устройства класса CDC, `rfcomm` — канал
        // Bluetooth SPP, названный в контракте наравне с COM/VCP.
        //
        // `ttyS*` намеренно нет: узлы `ttyS0`…`ttyS31` заведены в системе
        // независимо от того, есть ли за ними хоть какое-то железо, и
        // список из трёх десятков несуществующих портов хуже пустого —
        // в нём тонет единственный настоящий.
        const bool interesting = name.startsWith( QStringLiteral( "ttyUSB" ) ) || name.startsWith( QStringLiteral( "ttyACM" ) )
                                 || name.startsWith( QStringLiteral( "rfcomm" ) );
        if ( !interesting ) {
            continue;
        }
        const QString path = QStringLiteral( "/dev/" ) + name;
        if ( exists( path ) ) {
            found.push_back( path );
        }
    }
    ::closedir( dir );
    std::sort( found.begin(), found.end(), portLess );
    return found;
}

#endif

// ===========================================================================
// Общая часть: одна на обе платформы
// ===========================================================================

SerialTransport::SerialTransport( SerialParams params ) : m_impl( new Impl ) {
    m_impl->params = std::move( params );
}

SerialTransport::~SerialTransport() {
    // Закрывать порт обязан деструктор, а не только `close()`: брошенный
    // открытым порт держится за процессом до его конца, и следующая
    // попытка связи упрётся в собственный прошлый сеанс — отказ при этом
    // выглядит как «порт занят кем-то другим».
    //
    // Ждать ухода недописанного здесь нечего: последний пакет сессии
    // (`Disconnect`) подтверждается ответом прибора, то есть к этому
    // моменту он давно на линии. Ожидание же может не кончиться никогда,
    // а зависший выход хуже потерянного хвоста.
    m_impl->close();
}

Bus SerialTransport::bus() const {
    return Bus::Serial;
}

QString SerialTransport::description() const {
    const SerialParams &p = m_impl->params;
    if ( p.port.isEmpty() ) {
        return QStringLiteral( "порт не назначен" );
    }
    QString out = p.port;
    if ( p.baudRate > 0 ) {
        out += QLatin1Char( ' ' ) + QString::number( p.baudRate );
    } else {
        // Ноль скоростью не является. Напечатать «COM3 0» значило бы
        // показать число, которого нет: величина без единицы и годности
        // числом не показывается (`docs/СЛЫШИМОСТЬ.md`).
        out += QStringLiteral( " (скорость не задана)" );
    }
    if ( looksLikeSpp( p.port ) ) {
        out += QStringLiteral( " (SPP)" );
    }
    // Признака «открыт» здесь нет намеренно: связь описывает `State`
    // бэкенда (`present`, `linked`), и второе место, отвечающее на тот же
    // вопрос, рано или поздно разойдётся с первым. Описание отвечает
    // только на вопрос «что за оснастка», а не «работает ли она».
    return out;
}

bool SerialTransport::open() {
    m_lastError.clear();
    if ( m_impl->isOpen() ) {
        // Повторное открытие уже открытого порта отказом не считается:
        // `link()` бэкенда вправе быть вызван дважды, и закрывать ради
        // этого исправную линию — терять сессию на ровном месте.
        return true;
    }
    if ( m_impl->params.port.isEmpty() ) {
        m_lastError = QStringLiteral( "порт не назначен: открывать нечего" );
        return false;
    }
    if ( m_impl->params.baudRate <= 0 ) {
        // Умолчания у скорости нет намеренно (см. заголовок): противоречие
        // «сессия начинается на 9600» против «обе эталонные реализации
        // открывают сразу 115200» на бумаге не закрывается. Открыть порт
        // «как есть» значило бы принять ту скорость, которую оставил на
        // линии прошлый хозяин, — то есть величину без источника. Обмен
        // на ней сорвётся не сразу и молча.
        m_lastError = QStringLiteral( "скорость линии не задана: у неё нет умолчания, значение обязан назвать вызывающий" );
        return false;
    }
    if ( !m_impl->open() ) {
        m_lastError = m_impl->lastError;
        return false;
    }
    return true;
}

void SerialTransport::close() {
    m_impl->close();
}

bool SerialTransport::isOpen() const {
    return m_impl->isOpen();
}

bool SerialTransport::write( const QByteArray &data ) {
    m_lastError.clear();
    if ( !m_impl->isOpen() ) {
        m_lastError = QStringLiteral( "порт закрыт: писать некуда" );
        return false;
    }
    if ( data.isEmpty() ) {
        // Писать нечего — и это не отказ: пустой пакет собрать нельзя, а
        // пустую запись вызывающий делает только по недосмотру, и падать
        // на его недосмотре линии незачем.
        return true;
    }
    if ( !m_impl->write( data.constData(), int( data.size() ) ) ) {
        m_lastError = m_impl->lastError;
        return false;
    }
    return true;
}

QByteArray SerialTransport::read( int maxBytes ) {
    m_lastError.clear();
    if ( !m_impl->isOpen() ) {
        m_lastError = QStringLiteral( "порт закрыт: читать неоткуда" );
        return {};
    }
    if ( maxBytes <= 0 ) {
        return {};
    }
    QByteArray buffer( maxBytes, '\0' );
    const int got = m_impl->readSome( buffer.data(), maxBytes, m_impl->params.readTimeoutMs );
    if ( got < 0 ) {
        // Отказ линии и молчание прибора возвращают ОДНО И ТО ЖЕ — пустой
        // массив. Различить их можно только по записи об ошибке, и ровно
        // поэтому она чистится на входе в каждый метод: запись, оставшаяся
        // от прошлой операции, превратила бы законное молчание в поломку.
        m_lastError = m_impl->lastError;
        return {};
    }
    buffer.resize( got );
    return buffer;
}

int SerialTransport::bytesAvailable() const {
    if ( !m_impl->isOpen() ) {
        return -1;
    }
    return m_impl->available();
}

QByteArray SerialTransport::readExactly( int count, int timeoutMs ) {
    m_lastError.clear();
    if ( !m_impl->isOpen() ) {
        m_lastError = QStringLiteral( "порт закрыт: читать неоткуда" );
        return {};
    }
    if ( count <= 0 ) {
        return {};
    }
    QByteArray buffer( count, '\0' );
    int got = 0;
    const auto deadline = Clock::now() + std::chrono::milliseconds( timeoutMs > 0 ? timeoutMs : 0 );
    for ( ;; ) {
        const int left = msLeft( deadline );
        const int n = m_impl->readSome( buffer.data() + got, count - got, left );
        if ( n < 0 ) {
            m_lastError = m_impl->lastError;
            break;
        }
        got += n;
        if ( got >= count ) {
            break;
        }
        if ( left == 0 ) {
            // Срок вышел. Один заход без ожидания уже сделан выше — он
            // забрал то, что успело прийти. Короткий результат означает
            // «пакет не дочитан», и решает это вызывающий: для него
            // недочитанный пакет есть повод к перезапросу `0x92`, а не
            // повод считать линию неисправной.
            break;
        }
        // Межбайтовая пауза внутри `readSome()` обрывает ОДИН заход, а не
        // всё чтение: срок целиком принадлежит объявленному `timeoutMs`,
        // и других условий выхода контракт не называет. Прибор, у
        // которого между частями ответа есть пауза длиннее межбайтовой,
        // будет дочитан, а не обрезан.
    }
    buffer.resize( got );
    return buffer;
}

void SerialTransport::purge() {
    if ( !m_impl->isOpen() ) {
        return;
    }
    m_impl->purge();
}

bool SerialTransport::setBaudRate( int baud ) {
    m_lastError.clear();
    if ( !m_impl->isOpen() ) {
        // Менять скорость закрытого порта нечему: параметр запомнился бы,
        // а линия его не увидела бы — и `description()` печатал бы одно,
        // а на проводе стояло бы другое. Скорость до открытия задаётся
        // через `SerialParams`, и это её единственное место.
        m_lastError = QStringLiteral( "порт закрыт: скорость менять нечему" );
        return false;
    }
    if ( baud <= 0 ) {
        m_lastError = QStringLiteral( "скорость %1 бод недопустима" ).arg( baud );
        return false;
    }
    // Ряд `SerialSpeeds` здесь НЕ проверяется: он перечисляет скорости, на
    // которых имеет смысл сессия с Oscill, а порт про Oscill не знает и
    // знать не должен. Ограничение принадлежит бэкенду, который считает
    // коэффициент прибора; порт отвечает лишь за то, принята ли скорость
    // системой.
    if ( !m_impl->setBaud( baud ) ) {
        // Скорость линии при отказе НЕ меняется: `params.baudRate`
        // обновляется только после успешной сверки перечитанным
        // состоянием. Половинчатая смена скорости оборвала бы связь молча.
        m_lastError = m_impl->lastError;
        return false;
    }
    return true;
}

const SerialParams &SerialTransport::params() const {
    return m_impl->params;
}

} // namespace Instrument
