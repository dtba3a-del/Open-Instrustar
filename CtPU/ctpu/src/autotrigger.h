// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file autotrigger.h
/// \brief Автоматический выбор уровня триггера по гистограмме отсчётов.
///
/// Задание 9 очереди прототипа (`docs/PROTOTYPE-QUEUE.md`): «автоматика на
/// основе гистограмм: где статистически чаще бывает, там и триггер».
///
/// ## На чём это стоит
///
/// Гистограмма отсчётов установившегося сигнала показывает, где он проводит
/// время. У сигнала с двумя состояниями (меандр, импульсы, логика) она имеет
/// **две моды** — низкий уровень и высокий; между ними провал, потому что
/// фронты быстрые и отсчётов там мало. Уровень, который сигнал заведомо
/// пересекает, лежит **между модами**, а не в них: в самой моде сигнал стоит,
/// и пересечения там может не быть вовсе.
///
/// Поэтому «где чаще» служит не ответом, а способом найти ответ: моды
/// указывают, ГДЕ находятся состояния, а порог ставится посередине.
///
/// У сигнала с одной модой (синус, шум, постоянная составляющая с дрожанием)
/// двух состояний нет, и осмысленного «между» тоже. Тогда берётся **медиана**:
/// уровень, который сигнал пересекает по построению — половину времени он
/// выше, половину ниже.
///
/// Это не заимствование приёма, а следствие того, что такое пересечение
/// уровня; порог и число корзин — выбор реализации и объявлены ниже.

#include <algorithm>
#include <cstddef>
#include <vector>

namespace AutoTrigger {

/// Число корзин гистограммы. Компромисс: мало корзин — моды сливаются, много —
/// каждая мода дробится шумом на несколько соседних.
inline constexpr std::size_t defaultBins = 64;

/// Во сколько раз вторая мода должна быть выше провала между модами, чтобы
/// считаться модой, а не колебанием шума. Ниже порога сигнал считается
/// одномодовым.
inline constexpr double modeContrast = 2.0;

/// \brief Результат разбора: уровень и то, чем он получен.
struct Level {
    double value = 0.0;      ///< уровень триггера в единицах отсчётов
    bool bimodal = false;    ///< true: найдены два состояния, порог между ними
    bool valid = false;      ///< false: данных недостаточно, уровень не назначен
};

/// \brief Уровень триггера по гистограмме отсчётов.
///
/// Возвращает `valid = false`, если отсчётов меньше двух или сигнал постоянен:
/// у постоянного сигнала пересечения не существует, и назначать уровень —
/// значит выдумывать событие, которого не будет.
inline Level level( const std::vector< double > &samples, std::size_t bins = defaultBins ) {
    Level out;
    if ( samples.size() < 2 || bins < 4 )
        return out;

    const auto mm = std::minmax_element( samples.begin(), samples.end() );
    const double lo = *mm.first;
    const double hi = *mm.second;
    if ( !( hi > lo ) )
        return out; // постоянный сигнал: пересечения нет

    std::vector< std::size_t > hist( bins, 0 );
    const double scale = double( bins ) / ( hi - lo );
    for ( double v : samples ) {
        std::size_t b = std::size_t( ( v - lo ) * scale );
        if ( b >= bins )
            b = bins - 1;
        ++hist[ b ];
    }

    // Первая мода — самая населённая корзина.
    const std::size_t m1 = std::size_t( std::max_element( hist.begin(), hist.end() ) - hist.begin() );

    // Вторая мода ищется вне окрестности первой: соседние корзины принадлежат
    // той же моде, размазанной шумом, и второй модой не являются.
    const std::size_t guard = std::max< std::size_t >( 1, bins / 16 );
    std::size_t m2 = m1;
    std::size_t best = 0;
    for ( std::size_t i = 0; i < bins; ++i ) {
        const std::size_t dist = i > m1 ? i - m1 : m1 - i;
        if ( dist <= guard )
            continue;
        if ( hist[ i ] > best ) {
            best = hist[ i ];
            m2 = i;
        }
    }

    const auto binCentre = [ & ]( std::size_t b ) { return lo + ( double( b ) + 0.5 ) / scale; };

    if ( m2 != m1 && best > 0 ) {
        // Провал между модами: если его нет, две «моды» — одна размазанная.
        const std::size_t from = std::min( m1, m2 ) + 1;
        const std::size_t to = std::max( m1, m2 );
        std::size_t valley = hist[ from ];
        for ( std::size_t i = from; i < to; ++i )
            valley = std::min( valley, hist[ i ] );
        if ( double( best ) >= modeContrast * double( valley ) ) {
            out.value = 0.5 * ( binCentre( m1 ) + binCentre( m2 ) );
            out.bimodal = true;
            out.valid = true;
            return out;
        }
    }

    // Одномодовый случай: медиана. Пересечение по построению.
    std::vector< double > sorted( samples );
    const std::size_t mid = sorted.size() / 2;
    std::nth_element( sorted.begin(), sorted.begin() + long( mid ), sorted.end() );
    out.value = sorted[ mid ];
    out.bimodal = false;
    out.valid = true;
    return out;
}

} // namespace AutoTrigger
