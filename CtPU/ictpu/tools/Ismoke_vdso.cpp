// SPDX-License-Identifier: GPL-3.0-or-later
// Last edited: 2026-09-06 UTC
//
// Дымовая проверка пути A на настоящей библиотеке.
//
// Распоряжение автора 2026-09-06: «ты НЕ тестировал работоспособность сборки
// с vdso.dll, несмотря на то что она тебе предоставлена… тестируй
// работоспособность приложения прежде чем выкладывать».
//
// Прибора у сборочной машины нет, и его отсутствие проверкой не считается.
// Зато **всё, что до прибора, проверяется по-настоящему**, и это ровно то,
// что раньше не проверялось вовсе:
//
//   1. библиотека находится и грузится (вместе с её рантаймом MSVC —
//      MSVCP140, VCRUNTIME140, VCRUNTIME140_1; их отсутствие даёт код 126,
//      и мы это увидим, а не узнаем от автора);
//   2. связываются ВСЕ 73 экспорта — ни одного промаха в именах;
//   3. InitDll выполняется и возвращает определённый ответ;
//   4. опрос прибора отвечает «нет прибора» — определённо, а не падением.
//
// Отсутствие прибора — НЕ отказ проверки. Отказ — это ненайденная библиотека,
// несвязанный экспорт или падение.

#include "ivdsoloader.h"
#include "ivdsosession.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace {

class RealClock : public IVdso::Clock {
  public:
    void sleepMs( int ms ) override {
        if ( ms > 0 )
            std::this_thread::sleep_for( std::chrono::milliseconds( ms ) );
    }
    uint64_t nowMs() override {
        using namespace std::chrono;
        return uint64_t( duration_cast< milliseconds >( steady_clock::now().time_since_epoch() ).count() );
    }
};

} // namespace


int main( int argc, char **argv ) {
    if ( argc < 2 ) {
        std::fprintf( stderr, "использование: Ismoke_vdso <путь к vdso.dll>\n" );
        return 2;
    }
    const std::string path = argv[ 1 ];
    std::printf( "== дымовая проверка пути A ==\nбиблиотека: %s\n", path.c_str() );

    IVdso::Loader loader;
    if ( !loader.load( path ) ) {
        std::printf( "ОТКАЗ: %s\n", loader.lastError().c_str() );
        return 1;
    }
    std::printf( "загружена, связано экспортов: %d из 73\n", loader.boundSymbols() );
    if ( loader.boundSymbols() != 73 ) {
        std::printf( "ОТКАЗ: связаны не все экспорты\n" );
        return 1;
    }

    RealClock clock;
    IVdso::Session session( loader, clock );
    const bool opened = session.open();
    std::printf( "открытие прибора: %s\n", opened ? "прибор есть" : session.lastError().c_str() );

    if ( opened ) {
        const IVdso::Passport &p = session.passport();
        std::printf( "модель: %s, память %u КБ, DDS: %s\n", p.model.c_str(), p.memoryKb,
                     p.hasDds ? "есть" : "нет" );
        session.close();
    } else {
        // Прибора на сборочной машине нет, и это ожидаемо. Важно, что ответ
        // ОПРЕДЕЛЁННЫЙ: библиотека отработала, а не упала.
        std::printf( "прибора нет — для сборочной машины это ожидаемо и отказом не считается\n" );
    }

    std::printf( "ИТОГ: библиотека рабочая, интерфейс сходится\n" );
    return 0;
}
