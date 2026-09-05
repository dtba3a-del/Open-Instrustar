// SPDX-License-Identifier: GPL-2.0-or-later
// Last edited: 2026-08-13 07:32:55 UTC
//
// BinTape — лента с фиксированным числом бинов: замена кольцевого буфера
// XY-рекордера.
//
// ЧТО БЫЛО НЕ ТАК СО СТАРОЙ СХЕМОЙ (диагностировано по коду и по 3peak.csv):
//   1. СТУПЕНИ. Каскад XYRecorder::feedStage() выдавал точку строго каждые
//      cascadeBase сэмплов и сбрасывал стадию. Недобор до полного блока
//      «повисал» в стадии до СЛЕДУЮЩЕГО кадра, то есть одна выходная точка
//      усредняла данные, разорванные во времени паузой USB-цикла. На
//      физически линейном участке это давало плато + скачок на стыке кадров.
//      Период ступеней = период кадра, а не сигнала.
//   2. ЧИСЛО ТОЧЕК НЕ СОБЛЮДАЛОСЬ. depth = ceil(log(decim)/log(base))
//      квантует прореживание до целой степени base: при base=8 достижимы
//      только 8, 64, 512, 4096, 32768. Заказ «2000 точек» давал по факту
//      977 / 1953 / 488 — промах до 8 раз.
//   3. SLEWRATE — ПРЕДСКАЗАНИЕ, А НЕ ИЗМЕРЕНИЕ. Длительность развёртки
//      угадывалась как fullScale/slewRate. Одним числом невозможно описать
//      разные скорости нарастания и спада (наблюдалось на реальном железе).
//   4. СТИРАНИЕ. При переполнении делался pop_front() — начало записи
//      молча терялось.
//
// ЧТО ДЕЛАЕТ ЭТА СХЕМА (модель самописца):
//   Бин — это участок ЛЕНТЫ (позиция в порядке записи), а не значение
//   сигнала. Лента протягивается монотонно, независимо от того, куда
//   мечется «перо». Отсюда сразу:
//     - ступеней нет: решение «выдать точку» не привязано ни к границе
//       кадра, ни к предсказанной скорости;
//     - точек РОВНО столько, сколько бинов — по определению, не по прогнозу;
//     - длительность записи знать заранее НЕ НУЖНО: когда бины кончились,
//       соседние пары сливаются (занято становится вдвое меньше, ёмкость
//       бина удваивается), запись продолжается в новом масштабе;
//     - НИЧЕГО НЕ СТИРАЕТСЯ: вся история сохраняется, лишь равномерно
//       грубеет;
//     - гистерезис не смешивается: точки разных проходов лежат в разных
//       бинах по построению.
//
// ПОЧЕМУ ЗДЕСЬ НЕТ КЛАССИФИКАЦИИ НА «ПРЯМУЮ/ОБРАТНУЮ» ВЕТВЬ:
//   Рассматривалось (и отвергнуто в чате) раздвоение накопителей на fwd/rev.
//   Такое деление НАВЯЗЫВАЕТ данным модель «простая развёртка туда-обратно».
//   Если внутри петли есть зубцы, вложенные минипетли или задержки, данные
//   попадут в два ведра по признаку, не имеющему отношения к их физике, а на
//   выходе получатся два опрятных столбца, выглядящих достоверно. Это худший
//   вид ошибки — не шум, а правдоподобная выдумка.
//   Поэтому здесь ИЗМЕРЯЮТ, а не классифицируют:
//     path      — пройденный путь внутри бина (факт),
//     net       — смещение конец-начало (факт),
//     path/net  — насколько бину можно доверять:
//                   ~1  -> бин пройден напрямую, mean осмыслен;
//                   >>1 -> внутри петли/зубцы, mean бессмыслен, И ЭТО ВИДНО.
//   min/max при этом гарантируют, что даже при глубоком слиянии петля
//   остаётся видна как ПОЛОСА РЕАЛЬНОГО ОХВАТА, а не схлопывается в линию.
//   Ни при каких данных этот набор ничего не выдумывает.
//   (Замерено на 3peak.csv: сырой path/net имеет медиану 7.5 — это ДРОЖАНИЕ,
//   не петли; после MedianFilter W=61 медиана падает до 1.02, а верхние 10%
//   остаются на 3.15 — то есть метрика начинает отличать реальную структуру
//   от шума. Отсюда рекомендация включать медианный предфильтр.)
//
// СЛИЯНИЕ ТОЧНОЕ. Все хранимые величины восстанавливаются из накопителей:
//   count/sum/sumSq -> mean, rms, stddev;  min/max;  first/last;  path/net.
//   Медианы здесь намеренно НЕТ: она не сливается точно (медиана медиан !=
//   медиана), поэтому живёт предфильтром (medianfilter.h), а не колонкой.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace BinTape {

/// \brief Накопители одного канала внутри одного бина.
/// Всё — O(1) по памяти и точно сливаемо с соседним бином.
struct ChannelAccum {
    std::uint64_t count = 0;
    double sum = 0.0;
    double sumSq = 0.0;
    double minV = 0.0;
    double maxV = 0.0;
    double first = 0.0;
    double last = 0.0;

    /// \name Энергетический баланс предфильтра (закон сохранения)
    ///
    /// Задача: при использовании данных самописца (RMS, мощность, дисперсия)
    /// не должно возникать нарушения закона сохранения энергии — что ушло в
    /// отвал, должно быть учтено ЭНЕРГЕТИЧЕСКИ, а не по числу отсчётов.
    /// Счёт отсчётов для этого бесполезен: он ничего не говорит об амплитуде.
    ///
    /// ВАЖНАЯ МАТЕМАТИЧЕСКАЯ ТОНКОСТЬ. Медианный фильтр НЕЛИНЕЕН, поэтому
    /// энергия НЕ распадается на «осталось + ушло». Точное тождество:
    ///
    ///     Sum(x^2)  =  Sum(y^2)  +  Sum(r^2)  +  2*Sum(y*r)
    ///     энергия      энергия      энергия      перекрёстный
    ///     входа        выхода       отвала       член
    ///
    /// где y — выход фильтра (именно он копится в sumSq выше, так как в
    /// накопитель подаются уже отфильтрованные значения), r = x - y — отвал.
    /// Перекрёстный член 2*Sum(y*r) в общем случае НЕ НУЛЕВОЙ: ортогонального
    /// разложения (как у Парсеваля для идеального ФНЧ) здесь нет. Если его
    /// не учитывать, баланс не сойдётся и это будет выглядеть как «пропавшая
    /// энергия», хотя пропажи нет. Поэтому хранятся все три члена, и
    /// тождество проверяемо в любой момент — см. energyIn().
    ///
    /// Все величины аддитивны (кроме residualPeak — она max), поэтому
    /// сливаются точно при объединении бинов на любой глубине.
    ///@{
    double residualSumSq = 0.0;  ///< Sum(r^2) — энергия того, что ушло в отвал
    double residualCross = 0.0;  ///< Sum(y*r) — перекрёстный член (умножить на 2 в тождестве)
    double residualPeak = 0.0;   ///< max|r| — наибольшая одиночная поправка (амплитуда выброса)
    double residualAbs = 0.0;    ///< Sum|r| — L1, вспомогательно (средний размер поправки)
    std::uint64_t medianModified = 0; ///< сколько значений фильтр изменил (диагностика, не баланс)
    ///@}

    void add( double v ) {
        if ( count == 0 ) {
            minV = maxV = first = v;
        } else {
            minV = std::min( minV, v );
            maxV = std::max( maxV, v );
        }
        last = v;
        sum += v;
        sumSq += v * v;
        ++count;
    }

    /// \brief Учесть отвал предфильтра для ЭТОГО отсчёта.
    /// \param y отфильтрованное значение (то, что реально пошло в данные)
    /// \param r остаток x - y (то, что ушло в отвал; знак сохраняется)
    /// Вызывается вместе с add(y) для того же отсчёта.
    void noteResidual( double y, double r ) {
        if ( r == 0.0 )
            return;
        residualSumSq += r * r;
        residualCross += y * r;
        residualAbs += std::fabs( r );
        residualPeak = std::max( residualPeak, std::fabs( r ) );
        ++medianModified;
    }

    /// \brief Слияние с ПОСЛЕДУЮЩИМ по времени бином (other идёт после this).
    /// Порядок важен только для first/last; остальное коммутативно.
    void mergeWithNext( const ChannelAccum &other ) {
        if ( other.count == 0 )
            return;
        if ( count == 0 ) {
            *this = other;
            return;
        }
        minV = std::min( minV, other.minV );
        maxV = std::max( maxV, other.maxV );
        last = other.last; // first остаётся нашим — мы раньше по времени
        sum += other.sum;
        sumSq += other.sumSq;
        count += other.count;
        residualSumSq += other.residualSumSq;
        residualCross += other.residualCross;
        residualAbs += other.residualAbs;
        residualPeak = std::max( residualPeak, other.residualPeak );
        medianModified += other.medianModified;
    }

    /// \name Энергетические характеристики (для проверки закона сохранения)
    ///@{
    /// \brief Энергия сигнала, попавшего в данные: Sum(y^2).
    double energyKept() const { return sumSq; }
    /// \brief Энергия, ушедшая в отвал: Sum(r^2).
    double energyResidual() const { return residualSumSq; }
    /// \brief Полная энергия ИСХОДНОГО (до фильтра) сигнала.
    /// Тождество точное: Sum(x^2) = Sum(y^2) + Sum(r^2) + 2*Sum(y*r).
    double energyIn() const { return sumSq + residualSumSq + 2.0 * residualCross; }
    /// \brief Доля энергии, ушедшей в отвал (0..1). Главный индикатор:
    /// если она заметна, среднеквадратичные величины бина занижены ровно
    /// на неё, и это видно, а не спрятано.
    double residualEnergyFraction() const {
        const double ein = energyIn();
        return ein > 0.0 ? residualSumSq / ein : 0.0;
    }
    /// \brief СКЗ отвала — «сколько вольт шума» было снято в среднем.
    double residualRms() const { return count ? std::sqrt( residualSumSq / double( count ) ) : 0.0; }
    ///@}

    double mean() const { return count ? sum / double( count ) : 0.0; }
    double rms() const { return count ? std::sqrt( sumSq / double( count ) ) : 0.0; }
    /// Población std-dev (совпадает с AC-rms относительно собственного среднего).
    double stddev() const {
        if ( !count )
            return 0.0;
        const double m = mean();
        return std::sqrt( std::max( 0.0, sumSq / double( count ) - m * m ) );
    }
};

/// \brief Один бин ленты: два канала + модельно-свободные метрики доверия.
struct Bin {
    ChannelAccum x;
    ChannelAccum y;

    /// Пройденный путь в плоскости XY внутри бина (сумма |шагов|). Факт.
    double path = 0.0;
    /// Число слияний, пережитых этим бином (0 = ни разу не сливался).
    std::uint32_t depth = 0;
    /// Метки времени (host wall-clock, мс): диапазон, а не список.
    /// Список принципиально непригоден: полчаса на 15 MS/s при 512 бинах
    /// дают ~5.3e7 сэмплов в бине — ячейка распухла бы до мусора.
    std::int64_t tFirstMs = 0;
    std::int64_t tLastMs = 0;
    /// Сколько кадров (вызовов addFrame) внесли вклад в этот бин.
    std::uint32_t frames = 0;

    bool empty() const { return x.count == 0; }

    /// \brief Смещение «конец минус начало» в плоскости XY. Факт.
    double net() const {
        const double dx = x.last - x.first;
        const double dy = y.last - y.first;
        return std::sqrt( dx * dx + dy * dy );
    }

    /// \brief Индикатор доверия к усреднённым величинам этого бина.
    /// ~1  -> бин пройден напрямую, mean/rms осмысленны.
    /// >>1 -> внутри бина петли/зубцы/дрожание: mean НЕ характеризует бин.
    /// Возвращает бесконечность, если путь пройден, а смещение нулевое
    /// (замкнутая петля внутри бина) — это максимально возможное недоверие,
    /// и это честный ответ, а не ошибка.
    double trustRatio() const {
        const double n = net();
        if ( n <= 1e-12 )
            return path > 1e-12 ? std::numeric_limits< double >::infinity() : 1.0;
        return path / n;
    }

    /// \brief Направление по мастер-оси за бин: знак net по X (или Y).
    /// Осмысленно ТОЛЬКО при trustRatio() ~ 1; при больших значениях честно
    /// бессмысленно, и это видно по соседнему полю. Намеренно НЕ участвует
    /// в разделении накопителей (см. большой комментарий вверху файла).
    int direction( bool masterIsX ) const {
        const double d = masterIsX ? ( x.last - x.first ) : ( y.last - y.first );
        return ( d > 0 ) - ( d < 0 );
    }

    void mergeWithNext( const Bin &other ) {
        if ( other.empty() )
            return;
        if ( empty() ) {
            const std::uint32_t keepDepth = depth;
            *this = other;
            depth = std::max( keepDepth, other.depth );
            return;
        }
        // Путь склеивается с учётом «перепрыгивания» между бинами: между
        // последней точкой этого бина и первой точкой следующего сигнал
        // тоже прошёл какое-то расстояние, и оно должно попасть в path,
        // иначе trustRatio окажется занижен (бин покажется прямее, чем он
        // есть). Это тот самый стык, на котором старый каскад давал ступень.
        const double jx = other.x.first - x.last;
        const double jy = other.y.first - y.last;
        path += std::sqrt( jx * jx + jy * jy ) + other.path;

        x.mergeWithNext( other.x );
        y.mergeWithNext( other.y );
        tLastMs = other.tLastMs;
        frames += other.frames;
        depth = std::max( depth, other.depth ) + 1;
    }
};

/// \brief Лента: фиксированное число бинов, слияние пар вместо стирания.
///
/// Число бинов выбирается пользователем из стандартного набора (решение
/// согласовано в чате: произвольный ввод не даётся, чтобы не плодить
/// неудобные для многократного деления пополам значения).
static inline const std::vector< std::size_t > &standardBinCounts() {
    static const std::vector< std::size_t > v = { 500, 1000, 2000, 5000, 10000, 20000, 50000 };
    return v;
}

class Tape {
  public:
    /// \param binCount желаемое число бинов (обычно из standardBinCounts()).
    /// \param samplesPerBin стартовая ёмкость бина; удваивается на каждом
    ///        слиянии. Значение 0 трактуется как 1.
    explicit Tape( std::size_t binCount = 2000, std::uint64_t samplesPerBin = 1 )
        : m_capacity( std::max< std::size_t >( 1, binCount ) ),
          m_samplesPerBin( std::max< std::uint64_t >( 1, samplesPerBin ) ) {
        m_bins.reserve( m_capacity );
    }

    /// \brief Добавить один отсчёт траектории.
    /// \param tMs host wall-clock метка кадра, из которого пришёл отсчёт.
    /// \param newFrame true для ПЕРВОГО отсчёта каждого кадра (для счётчика frames).
    /// \param residualX,residualY остаток предфильтра для ЭТОГО отсчёта по
    ///        каждому каналу: r = (исходное значение) - (отфильтрованное).
    ///        Знак важен — по нему считается перекрёстный член энергетического
    ///        тождества. 0 = фильтр не вмешивался или выключен.
    void addSample( double xv, double yv, std::int64_t tMs = 0, bool newFrame = false, double residualX = 0.0,
                    double residualY = 0.0 ) {
        if ( m_bins.empty() || m_bins.back().x.count >= m_samplesPerBin ) {
            if ( m_bins.size() >= m_capacity )
                mergePairs();
            m_bins.emplace_back();
            m_bins.back().tFirstMs = tMs;
        }
        Bin &b = m_bins.back();
        if ( m_havePrev ) {
            const double dx = xv - m_prevX;
            const double dy = yv - m_prevY;
            b.path += std::sqrt( dx * dx + dy * dy );
        }
        b.x.add( xv );
        b.y.add( yv );
        b.x.noteResidual( xv, residualX );
        b.y.noteResidual( yv, residualY );
        b.tLastMs = tMs;
        if ( newFrame )
            ++b.frames;
        m_prevX = xv;
        m_prevY = yv;
        m_havePrev = true;
    }

    /// \brief Слияние соседних пар: занятость падает вдвое, ёмкость бина
    /// удваивается. Ничего не теряется — запись лишь равномерно грубеет.
    void mergePairs() {
        if ( m_bins.size() < 2 )
            return;
        std::vector< Bin > merged;
        merged.reserve( ( m_bins.size() + 1 ) / 2 );
        for ( std::size_t i = 0; i + 1 < m_bins.size(); i += 2 ) {
            Bin b = m_bins[ i ];
            b.mergeWithNext( m_bins[ i + 1 ] );
            merged.push_back( b );
        }
        if ( m_bins.size() % 2 )
            merged.push_back( m_bins.back() ); // нечётный хвост переносим как есть
        m_bins.swap( merged );
        m_samplesPerBin *= 2;
    }

    const std::vector< Bin > &bins() const { return m_bins; }
    std::size_t capacity() const { return m_capacity; }
    std::uint64_t samplesPerBin() const { return m_samplesPerBin; }

    /// \brief Сколько сырых отсчётов уже поглощено лентой (сумма по бинам).
    std::uint64_t totalSamples() const {
        std::uint64_t n = 0;
        for ( const Bin &b : m_bins )
            n += b.x.count;
        return n;
    }

    void clear() {
        m_bins.clear();
        m_havePrev = false;
    }

  private:
    std::vector< Bin > m_bins;
    std::size_t m_capacity;
    std::uint64_t m_samplesPerBin;
    double m_prevX = 0.0;
    double m_prevY = 0.0;
    bool m_havePrev = false;
};

} // namespace BinTape
