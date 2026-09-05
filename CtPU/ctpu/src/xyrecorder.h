// SPDX-License-Identifier: GPL-2.0-or-later
// Last edited: 2026-08-19 06:48:51 UTC

#pragma once

#include <QFile>
#include <QString>
#include <QTextStream>
#include <cstddef>
#include <deque>
#include <vector>

#include "bintape.h"
#include "xyfield.h"
#include "medianfilter.h"
#include "scopesettings.h" // XYCurveConfig full definition (for m_curveConfig member)

class PPresult;

namespace Dso {
struct ControlSpecification;
}

/// \brief Continuous XY chart-recorder with anti-aliased cascade decimation.
///
/// Pen-plotter model: X is typically a sawtooth sweep (0.0001 V/s .. 10 V/us),
/// Y is the response, independent slew range. Points arrive as (CH1,CH2) pairs
/// and are pushed through a cascade of small averaging FIFO stages: each stage
/// low-pass-filters (box-car mean) its input *before* decimating to the next
/// stage. This avoids the aliasing/steps that plain stride-decimation produces,
/// while memory stays bounded to (cascade depth x stage size), independent of
/// recording length.
///
/// Two sizing modes ("лист" vs "лента"):
///   FINITE - operator knows the full-scale range of the master axis (comes
///            from DsoSettingsScope::gain() for that channel) and an expected
///            slew rate -> sweep duration is estimated, cascade is sized to
///            hit targetPoints for the whole sweep. Fits in RAM by design.
///   TAPE   - open-ended recording, cascade is sized to hit a fixed output
///            density (targetDensity points/second) instead. Since the total
///            length is unbounded, points are streamed to disk in chunks as
///            they age out of the in-RAM render window (tapeFilePath) -
///            nothing is discarded, RAM only ever holds the current window.
class XYRecorder {
  public:
    enum class SheetMode { FINITE, TAPE };        ///< Фиксированное N (лист) vs фиксированная плотность (лента)
    enum class MasterAxis { X, Y };                ///< Which axis' full-scale/slew rate sizes the cascade
    enum class ExtractMode { CASCADE, PEAK_ENVELOPE }; ///< Mean per block, or min+max envelope per block

    /// \brief Чем лента выводится наружу. Две РАЗНЫЕ функции, а не режим
    /// одной: усреднение по бинам и запись охвата отвечают на разные вопросы.
    enum class TapeProjection {
        ENVELOPE, ///< САМОПИСЕЦ: охват бина (first/min/max/last) — форма цела
        MEAN      ///< УСРЕДНИТЕЛЬ: одна точка на бин — тренд, медленная часть
    };

    struct Point {
        double x = 0.0;
        double y = 0.0;
        double sigma = 0.0; ///< std-dev of the final cascade block (0 if trackSigma == false)
        /// \brief Точка НАЧИНАЕТ новый отрезок: с предыдущей не соединять.
        ///
        /// Между концом одного пакета и началом следующего сигнала не было —
        /// прямая через весь экран рисовала бы связь, которой не существует.
        /// Разрешение — бин ленты: если в бине началось несколько кадров,
        /// разрыв ставится один, на первой его точке. Это та точность, какая
        /// у ленты есть; сочинять более мелкую нельзя.
        bool newSegment = false;
    };

    struct Config {
        SheetMode sheetMode = SheetMode::FINITE;
        MasterAxis masterAxis = MasterAxis::X;

        double slewRateX = 1.0; ///< V/s, expected range 1e-4 .. 1e7 (10 V/us)
        double slewRateY = 1.0; ///< V/s, same range

        std::size_t targetPoints = 2000; ///< FINITE: total points for the whole sweep
        double targetDensity = 2000.0;   ///< TAPE: points per second

        int cascadeBase = 8;             ///< samples averaged per stage, every stage
        ExtractMode extractMode = ExtractMode::CASCADE;
        bool trackSigma = false;         ///< only meaningful at the last cascade stage

        /// \name BinTape (замена кольцевого буфера)
        ///
        /// useBinTape=true переводит рекордер на модель самописца
        /// (bintape.h): бин — участок ЛЕНТЫ (позиция в порядке записи), а не
        /// значение сигнала. Это устраняет сразу все дефекты старого каскада:
        ///   - ступени (выдача точки больше не привязана к границе кадра),
        ///   - несоблюдение числа точек (было до 8x промаха из-за квантования
        ///     depth до целой степени cascadeBase),
        ///   - зависимость от slewRate как ПРЕДСКАЗАНИЯ длительности развёртки
        ///     (одно число не описывает разные скорости нарастания и спада),
        ///   - стирание начала записи через pop_front().
        /// При useBinTape=true поля slewRateX/slewRateY/cascadeBase/
        /// targetDensity в расчёте НЕ УЧАСТВУЮТ (остаются только для записи
        /// в заголовок и обратной совместимости чтения старых конфигов).
        ///@{
        bool useBinTape = true;          ///< false = старый каскад (для сравнения/отката)

        /// Проекция ленты наружу. По умолчанию САМОПИСЕЦ: прежде здесь стояло
        /// усреднение по бинам, и самописец от этого сам становился
        /// усреднителем — форма внутри бина пропадала тем сильнее, чем длиннее
        /// запись (`mergePairs` удваивает ёмкость бина).
        TapeProjection tapeProjection = TapeProjection::ENVELOPE;
        std::size_t binCount = 2000;     ///< из BinTape::standardBinCounts()
        /// Окно медианного предфильтра, 0/1 = выключен. Снимает коммутационные
        /// выбросы и звон ДО бинирования, за счёт чего min/max бина означают
        /// реальный охват сигнала, а не «где однажды стрельнуло».
        /// ВНИМАНИЕ: широкое окно срезает узкие вершины. На реальном захвате
        /// 3peak.csv при W>=31 пик отвала достигал полного размаха сигнала
        /// (0.189 В) — то есть медиана уничтожала вершины пиков, при том что
        /// по ДОЛЕ ЭНЕРГИИ отвала (0.0007%) этого не было видно совсем.
        /// Контролировать по residualPeak, а не по доле энергии.
        unsigned medianWindow = 0;

        /// \name Поле XY (настоящая XY-запись, xyfield.h)
        /// Лента BinTape усредняет по ВРЕМЕНИ и форму сигнала не сохраняет;
        /// поле бинирует ЗНАЧЕНИЯ обеих осей и потому восстановимо. Число
        /// ячеек задаётся по каждой оси отдельно: одно общее число дало бы
        /// только квадратную сетку.
        ///@{
        bool buildField = true;         ///< вести поле плотности X×Y
        std::size_t fieldBinsX = 512;   ///< ячеек по оси X (независимо от Y)
        std::size_t fieldBinsY = 512;   ///< ячеек по оси Y
        bool fieldAutoRange = true;     ///< границы по данным; false = полная шкала каналов
        double fieldXMin = 0.0, fieldXMax = 0.0;
        double fieldYMin = 0.0, fieldYMax = 0.0;
        ///@}
        ///@}

        /// TAPE only. Empty = no persistence, points are dropped from the
        /// front once the render window is exceeded (old ring-buffer
        /// behaviour, data IS lost - only useful for a quick live preview).
        /// Non-empty = every point that ages out of the render window is
        /// appended to this CSV instead of being dropped; full record is
        /// preserved on disk while RAM stays bounded to renderWindowPoints.
        QString tapeFilePath;

        /// Points kept in RAM for GPU rendering (both modes). For TAPE with
        /// a tapeFilePath set, this is also the trigger for flushing the
        /// oldest chunk to disk.
        std::size_t renderWindowPoints = 200000;

        /// TAPE + tapeFilePath only: how many of the oldest points to flush
        /// to disk at once. 0 = auto (renderWindowPoints / 2).
        std::size_t flushChunkPoints = 0;

        /// Safety net only, not the primary sizing mechanism. 0 = auto:
        /// 10x targetPoints (FINITE) or renderWindowPoints (TAPE without
        /// tapeFilePath).
        std::size_t safetyCapPoints = 0;
    };

    XYRecorder() = default;
    ~XYRecorder();

    /// Sizes the cascade from scope->horizontal.samplerate and
    /// scope->gain(masterChannel), clears any previous trajectory, and (TAPE
    /// + tapeFilePath) opens the output file and writes its header. Not safe
    /// mid-recording - call before starting acquisition. Any previous
    /// streaming file is finalized (flushed + closed) first. scope/spec are
    /// non-owning and must outlive the recorder (both owned by DsoWidget).
    void configure( DsoSettingsScope *scope, const Dso::ControlSpecification *spec, const Config &cfg );

    /// Feed one PPresult frame (many raw samples at hardware samplerate).
    void addFrame( const PPresult *data );

    /// Flushes any buffered points to the streaming file (if open) and
    /// closes it, then clears the in-RAM trajectory. Call when stopping
    /// acquisition - also called from the destructor as a safety net.
    void finalize();

    /// Clears the in-RAM trajectory only. Does NOT flush/close a streaming
    /// file - call finalize() first if one is open, or those points are lost.
    void clear();

    /// \brief Точки для отрисовки.
    /// При useBinTape=true это ПРОЕКЦИЯ бинов, пересобираемая лениво (один
    /// раз на кадр отрисовки, а не на каждый отсчёт), поэтому стоимость
    /// O(bins) на чтение, а не O(1) на сэмпл. Тип возврата и семантика
    /// сохранены прежними, чтобы glscope.cpp не менялся.
    const std::deque< Point > &trajectory() const {
        if ( config.useBinTape && m_renderDirty ) {
            traj.clear();
            if ( config.tapeProjection == TapeProjection::MEAN )
                projectBinMeans();
            else
                projectBinEnvelopes();
            m_renderDirty = false;
        }
        return traj;
    }

    /// \brief УСРЕДНИТЕЛЬ: одна точка на бин — его среднее.
    ///
    /// Именно это и превращало самописец в усреднитель: усреднение ВНУТРИ бина
    /// законно и нужно, но если наружу отдавать только средние, то по мере
    /// роста записи ёмкость бина удваивается (`mergePairs`), и вся запись
    /// сходится к горстке средних — формы внутри бина не остаётся.
    ///
    /// Функция сохранена намеренно и названа по существу: усреднение по бинам —
    /// **отдельная полезная функция** (тренд, дрейф, медленная составляющая),
    /// а не дефект. Дефектом было то, что она стояла вместо самописца.
    void projectBinMeans() const {
        for ( const auto &b : m_tape.bins() ) {
            if ( b.empty() )
                continue;
            // sigma переиспользована под СКЗ отвала предфильтра (ноль, если
            // фильтр выключен) — старый формат экспорта остаётся читаемым.
            traj.push_back( { b.x.mean(), b.y.mean(), b.y.residualRms(), b.frames > 0 } );
        }
    }

    /// \brief САМОПИСЕЦ: охват бина — то, что в нём действительно было.
    ///
    /// На каждый бин отдаётся его протяжённость: первое значение, крайние
    /// (min/max) и последнее. Средним бин не подменяется, поэтому размах
    /// сигнала внутри бина виден и при любом огрублении ленты.
    ///
    /// Порядок крайних выбирается по направлению движения внутри бина
    /// (last ≥ first → сперва минимум): иначе ломаная рисовала бы ложный
    /// зигзаг там, где сигнал шёл монотонно.
    void projectBinEnvelopes() const {
        for ( const auto &b : m_tape.bins() ) {
            if ( b.empty() )
                continue;
            const double sigma = b.y.residualRms();
            // Размах Y ставится в СЕРЕДИНЕ бина по X, а не в его крайних
            // точках. Причина: `x.minV` и `y.minV` — статистики РАЗНЫХ осей и
            // могли прийти с разных отсчётов; связать их в одну точку значило
            // бы сочинить отсчёт, которого не было (`docs/СЛЫШИМОСТЬ.md` §5).
            // Вертикальный штрих в середине утверждает ровно то, что известно:
            // «внутри этого бина Y побывал между min и max».
            //
            // Порядок first → середина → last сохраняет монотонность X, когда
            // X есть время: иначе ломаная шла бы назад по времени, и это
            // ловится тестом testTimeAxisMonotonicAcrossFrames.
            const double xMid = 0.5 * ( b.x.first + b.x.last );
            const bool rising = b.y.last >= b.y.first;
            traj.push_back( { b.x.first, b.y.first, sigma, b.frames > 0 } );
            if ( b.y.maxV != b.y.minV ) {
                if ( rising ) {
                    traj.push_back( { xMid, b.y.minV, sigma } );
                    traj.push_back( { xMid, b.y.maxV, sigma } );
                } else {
                    traj.push_back( { xMid, b.y.maxV, sigma } );
                    traj.push_back( { xMid, b.y.minV, sigma } );
                }
            }
            if ( b.x.last != b.x.first || b.y.last != b.y.first )
                traj.push_back( { b.x.last, b.y.last, sigma } );
        }
    }

    /// \brief Поле плотности X×Y — настоящая XY-запись (xyfield.h).
    /// В отличие от bins(), здесь бинируются ЗНАЧЕНИЯ осей, а не позиция на
    /// ленте, поэтому форма сигнала (меандр, ВАХ, петля) сохраняется.
    const XYField::Field &field() const { return m_field; }
    XYField::Field &field() { return m_field; }

    /// \brief Экспорт поля XY: длинный формат x_center,y_center,count.
    void exportFieldCSV( const QString &filename ) const;

    /// \brief Прямой доступ к бинам (все статистики, энергетический баланс).
    /// Пусто, если useBinTape=false.
    const std::vector< BinTape::Bin > &bins() const { return m_tape.bins(); }

    bool isStreamingToDisk() const { return tapeFile.isOpen(); }

    /// Snapshot of what's currently in RAM. In TAPE + streaming mode this is
    /// only the current render window (tail) - the full record is already at
    /// config.tapeFilePath. In FINITE mode this is the whole recording.
    void exportCSV( const QString &filename ) const;
    void exportCSVDetailed( const QString &filename ) const;

    /// \name Наполненность рекордера
    /// [FIX] Спрашиваем ПРОЕКЦИЮ, а не кэш. При useBinTape=true (значение по
    /// умолчанию) источник истины — m_tape, а `traj` лишь ленивый кэш,
    /// который наполняется ТОЛЬКО внутри trajectory(). Прежняя реализация
    /// читала `traj` напрямую, а все три вызывающих —
    /// GlScope::updateXY(), DsoWidget::showNew() и экспорт в MainWindow —
    /// проверяют empty()/size() ПЕРЕД обращением к trajectory(). Получался
    /// замкнутый круг: кэш нечем наполнить, не отрисовав, и нечего
    /// отрисовывать, не наполнив кэш. Ни одна XY-кривая не выводилась
    /// вообще, счётчик «XY pts» стоял на нуле, экспорт молча пропускал все
    /// кривые. Пересборка проекции стоит O(bins) и кэшируется по
    /// m_renderDirty, то есть происходит один раз на кадр, как и задумано.
    ///@{
    bool empty() const { return trajectory().empty(); }
    std::size_t size() const { return trajectory().size(); }
    ///@}

    const Config &currentConfig() const { return config; }
    std::size_t cascadeDepth() const { return cascade.size(); }

    /// \brief Bind this recorder to a specific XY curve (TZ §7.4 fix).
    /// Tells addFrame() which (xChannel, yChannel) pair to pull from the
    /// PPresult, and exportCSV()/writeHeader() which curve metadata to emit.
    /// Must be called before the first addFrame(); call again whenever the
    /// user changes the curve's channel assignment in the config dialog.
    /// If never called, defaults to (xChannel=0, yChannel=1) — the legacy
    /// CH1×CH2 behaviour.
    /// [FIX] Re-sizes the cascade whenever the curve binding changes and the
    /// recorder has already been configure()'d. rebuildCascade() derives its
    /// master-channel full-scale range from m_curveConfig (see cpp), so a
    /// stale binding at the time configure() first ran would otherwise size
    /// the cascade from the wrong channel's gain until the next configure()
    /// call. Safe to call before configure() too (scope is still nullptr,
    /// so rebuildCascade() is skipped; configure() will do the initial
    /// sizing using the by-then-current m_curveConfig).
    /// [FIX 2026-08-21, найдено живой проверкой У3] Ранее rebuildCascade()
    /// вызывался безусловно, а DsoWidget::ingestNew() зовёт setCurveConfig()
    /// КАЖДЫЙ кадр — rebuildCascade() делает traj.clear() и пересоздаёт
    /// ленту, поэтому запись никогда не накапливала больше одного кадра
    /// (счётчик «XY pts» стоял на месте; плотная фигура Лиссажу маскировала
    /// потерю). Пересборка нужна только когда сменилась ПАРА ОСЕЙ — тогда
    /// старые точки относятся к другим величинам и обязаны быть стёрты.
    void setCurveConfig( const XYCurveConfig &cfg ) {
        const bool axesChanged =
            ( cfg.xChannel != m_curveConfig.xChannel ) || ( cfg.yChannel != m_curveConfig.yChannel );
        m_curveConfig = cfg;
        if ( scope && axesChanged )
            rebuildCascade();
    }
    const XYCurveConfig &curveConfig() const { return m_curveConfig; }

  private:
    struct CascadeStage {
        double sumX = 0.0, sumY = 0.0;
        double sumX2 = 0.0, sumY2 = 0.0; ///< only accumulated at the last stage when trackSigma
        double minX = 0.0, maxX = 0.0, minY = 0.0, maxY = 0.0;
        int count = 0;
    };

    DsoSettingsScope *scope = nullptr;
    const Dso::ControlSpecification *spec = nullptr;
    Config config;
    /// \brief Per-curve (xChannel, yChannel, enabled) binding (TZ §7.4 fix).
    /// Defaults to CH1×CH2 so the legacy single-curve code path still works.
    XYCurveConfig m_curveConfig;

    /// У3 — TimeChannel. База отсчёта оси времени: captureTimestampMs первого
    /// кадра записи (мс, wall-clock). Отрицательное = ещё не установлена.
    /// Ось времени кривой = epoch кадра (оценка, USB-джиттер ~мс, см.
    /// timechannel.h/Stamp) + i/samplerate (intra, измеренная часть).
    double m_timeBaseMs = -1.0;
    /// Фолбэк для данных без captureTimestampMs (синтетика/тесты):
    /// накопленная длительность обработанных кадров, с.
    double m_timeAccumS = 0.0;
    std::vector< CascadeStage > cascade;
    /// Кэш проекции бинов для отрисовки (см. trajectory()). mutable, потому
    /// что trajectory() — const-метод: пересборка кэша логически не меняет
    /// состояние рекордера, а лишь материализует представление бинов.
    /// При useBinTape=false это по-прежнему основное хранилище точек
    /// (старый путь каскада пишет сюда напрямую через emitPoint()).
    mutable std::deque< Point > traj;
    mutable bool m_renderDirty = true;
    /// Источник истины при useBinTape=true.
    BinTape::Tape m_tape;
    /// Поле плотности X×Y — настоящая XY-запись (xyfield.h).
    XYField::Field m_field;
    std::size_t safetyCap = 0;

    QFile tapeFile;
    QTextStream tapeStream;

    void rebuildCascade();
    void feedStage( std::size_t stageIndex, double x, double y, double sigma );
    void emitPoint( double x, double y, double sigma );
    void flushChunkToDisk( std::size_t count );
    void writeHeader( QTextStream &out ) const;
};
