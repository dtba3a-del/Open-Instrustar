// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

/// \file xyfield.h
/// \brief Настоящая XY-запись: поле плотности посещений плоскости X×Y.
///
/// ПОЧЕМУ ОТДЕЛЬНО ОТ BinTape (замечание пользователя 2026-08-21, дефект
/// найден на приборе). BinTape бинирует **ленту** — позицию в порядке
/// записи, — и выдаёт по бину одно среднее. Для медленно меняющегося
/// сигнала это полезное усреднение за большое время, но это **не XY-запись**:
/// у меандра оба уровня попадают в один и тот же отрезок ленты, среднее
/// садится между ними, и по такому экспорту фигуру не восстановить никакой
/// математикой — информация уничтожена усреднением.
///
/// XY-запись обязана бинировать **значения обеих осей**, а не время:
/// плоскость делится на nX × nY ячеек, и копится, сколько раз траектория
/// побывала в каждой. Это ровно то, что рисует любой XY-самописец и что
/// хранит цифровой фосфор (*digital phosphor*, DPO — накопительный растр).
/// Меандр даёт две плотные области, ВАХ — свою кривую, петля гистерезиса —
/// свою петлю: форма сохраняется, потому что ничего не усредняется.
///
/// ДВА ПАРАМЕТРА, А НЕ ОДИН. Число ячеек задаётся по каждой оси отдельно
/// (nX и nY независимы). Единый «targetPoints» на всю плоскость — наивен:
/// из одного числа можно получить только квадратную сетку (√N × √N), то
/// есть либо квадратные числа, либо округление, о котором пользователь не
/// просил. Оси физически разные (разные пределы, разная динамика) — их
/// разрешение и задаётся раздельно.
namespace XYField {

/// \brief Одна ячейка поля.
struct Cell {
    std::uint64_t count = 0;   ///< сколько отсчётов попало в ячейку (плотность)
    std::int64_t tFirstMs = 0; ///< когда впервые посещена (host wall-clock)
    std::int64_t tLastMs = 0;  ///< когда посещена последний раз
};

/// \brief Поле плотности X×Y с независимым числом ячеек по осям.
///
/// Диапазон осей: либо задан явно (полная шкала канала — тогда сетка
/// стабильна и записи сравнимы между собой), либо отслеживается по данным
/// (autoRange) — тогда первый проход задаёт границы, а вылеты за них
/// зажимаются в крайние ячейки и считаются отдельно (clippedLow/High),
/// чтобы «за пределом» не притворялось «на границе» (REPRESENTATION-3D:
/// три области — в поле, на границе, за пределом).
class Field {
  public:
    void configure( std::size_t nx, std::size_t ny, double xMin, double xMax, double yMin, double yMax,
                    bool autoRange ) {
        m_nx = std::max< std::size_t >( 1, nx );
        m_ny = std::max< std::size_t >( 1, ny );
        m_autoRange = autoRange;
        m_xMin = xMin;
        m_xMax = xMax;
        m_yMin = yMin;
        m_yMax = yMax;
        m_rangeKnown = !autoRange && ( xMax > xMin ) && ( yMax > yMin );
        clear();
    }

    void clear() {
        m_cells.assign( m_nx * m_ny, Cell() );
        m_total = 0;
        m_clippedX = 0;
        m_clippedY = 0;
        if ( m_autoRange )
            m_rangeKnown = false;
    }

    /// \brief Внести один отсчёт траектории.
    void addSample( double x, double y, std::int64_t tMs ) {
        if ( !std::isfinite( x ) || !std::isfinite( y ) )
            return;
        if ( m_autoRange && !m_rangeKnown ) {
            // Первый отсчёт задаёт вырожденный диапазон; он расширяется
            // следующими, пока сетка ещё пуста.
            if ( m_total == 0 ) {
                m_xMin = m_xMax = x;
                m_yMin = m_yMax = y;
            } else {
                m_xMin = std::min( m_xMin, x );
                m_xMax = std::max( m_xMax, x );
                m_yMin = std::min( m_yMin, y );
                m_yMax = std::max( m_yMax, y );
            }
            ++m_total;
            m_pending.push_back( { x, y, tMs } );
            // Диапазон считаем установившимся, когда набрана первая порция:
            // дальше сетка фиксируется, иначе ячейки «плыли» бы и запись
            // теряла бы сравнимость.
            if ( m_pending.size() >= kRangeProbe ) {
                m_rangeKnown = true;
                padDegenerate();
                auto pending = std::move( m_pending );
                m_pending.clear();
                m_total = 0;
                for ( const auto &p : pending )
                    place( p.x, p.y, p.t );
            }
            return;
        }
        if ( !m_rangeKnown ) { // явный диапазон нулевой ширины
            padDegenerate();
            m_rangeKnown = true;
        }
        place( x, y, tMs );
    }

    /// \brief Досыпать отложенные отсчёты, если запись коротка (< kRangeProbe).
    void flushPending() {
        if ( m_pending.empty() )
            return;
        m_rangeKnown = true;
        padDegenerate();
        auto pending = std::move( m_pending );
        m_pending.clear();
        m_total = 0;
        for ( const auto &p : pending )
            place( p.x, p.y, p.t );
    }

    std::size_t binsX() const { return m_nx; }
    std::size_t binsY() const { return m_ny; }
    double xMin() const { return m_xMin; }
    double xMax() const { return m_xMax; }
    double yMin() const { return m_yMin; }
    double yMax() const { return m_yMax; }
    std::uint64_t totalSamples() const { return m_total; }
    std::uint64_t clippedX() const { return m_clippedX; }
    std::uint64_t clippedY() const { return m_clippedY; }
    const std::vector< Cell > &cells() const { return m_cells; }

    const Cell &at( std::size_t ix, std::size_t iy ) const { return m_cells[ iy * m_nx + ix ]; }

    /// \brief Центр ячейки по X (значение сигнала, не индекс).
    double xCenter( std::size_t ix ) const { return m_xMin + ( double( ix ) + 0.5 ) * stepX(); }
    double yCenter( std::size_t iy ) const { return m_yMin + ( double( iy ) + 0.5 ) * stepY(); }
    double stepX() const { return ( m_xMax - m_xMin ) / double( m_nx ); }
    double stepY() const { return ( m_yMax - m_yMin ) / double( m_ny ); }

    /// \brief Сколько ячеек реально посещено (непустых).
    std::size_t occupiedCells() const {
        std::size_t n = 0;
        for ( const auto &c : m_cells )
            if ( c.count )
                ++n;
        return n;
    }

    std::uint64_t maxCount() const {
        std::uint64_t m = 0;
        for ( const auto &c : m_cells )
            m = std::max( m, c.count );
        return m;
    }

  private:
    struct Pending {
        double x, y;
        std::int64_t t;
    };
    static constexpr std::size_t kRangeProbe = 4096;

    void padDegenerate() {
        if ( !( m_xMax > m_xMin ) ) {
            const double pad = std::max( 1e-9, std::fabs( m_xMin ) * 1e-6 );
            m_xMin -= pad;
            m_xMax += pad;
        }
        if ( !( m_yMax > m_yMin ) ) {
            const double pad = std::max( 1e-9, std::fabs( m_yMin ) * 1e-6 );
            m_yMin -= pad;
            m_yMax += pad;
        }
    }

    void place( double x, double y, std::int64_t tMs ) {
        const double fx = ( x - m_xMin ) / ( m_xMax - m_xMin );
        const double fy = ( y - m_yMin ) / ( m_yMax - m_yMin );
        if ( fx < 0.0 || fx >= 1.0 )
            ++m_clippedX;
        if ( fy < 0.0 || fy >= 1.0 )
            ++m_clippedY;
        const std::size_t ix =
            std::min( m_nx - 1, std::size_t( std::max( 0.0, std::min( 0.999999999, fx ) ) * double( m_nx ) ) );
        const std::size_t iy =
            std::min( m_ny - 1, std::size_t( std::max( 0.0, std::min( 0.999999999, fy ) ) * double( m_ny ) ) );
        Cell &c = m_cells[ iy * m_nx + ix ];
        if ( c.count == 0 )
            c.tFirstMs = tMs;
        c.tLastMs = tMs;
        ++c.count;
        ++m_total;
    }

    std::size_t m_nx = 256;
    std::size_t m_ny = 256;
    bool m_autoRange = true;
    bool m_rangeKnown = false;
    double m_xMin = 0.0, m_xMax = 0.0, m_yMin = 0.0, m_yMax = 0.0;
    std::uint64_t m_total = 0;
    std::uint64_t m_clippedX = 0;
    std::uint64_t m_clippedY = 0;
    std::vector< Cell > m_cells;
    std::vector< Pending > m_pending;
};

} // namespace XYField
