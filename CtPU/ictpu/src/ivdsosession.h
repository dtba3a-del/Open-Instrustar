// SPDX-License-Identifier: GPL-3.0-or-later
// Last edited: 2026-09-03 UTC
//
// Путь A: порядок вызовов vdso, оформленный автоматом состояний.
//
// Порядок здесь не украшение и не стиль — три его нарушения на ISDS205
// дают не сообщение об ошибке, а access violation или молча потерянный
// кадр (`docs/INSTRUSTAR-CONNECTOR.md`, §«Запреты»):
//   * читать до IsDataReady == 1 — запрещено;
//   * менять скорость выборки между готовностью и чтением — запрещено;
//   * длину кадра брать из запроса, а не из возврата — запрещено.
// Автомат состояний делает эти три запрета проверяемыми на поддельном
// приборе, без железа.
//
// Ни Qt, ни Windows: работа идёт через IVdso::Api и IVdso::Clock.

#pragma once

#include "ivdsoapi.h"
#include "ivdsounits.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace IVdso {

/// Время вынесено за интерфейс: тест не спит, он двигает часы сам.
class Clock {
  public:
    virtual ~Clock() = default;
    virtual void sleepMs( int ms ) = 0;
    virtual uint64_t nowMs() = 0;
};

/// Паспорт прибора: всё, что спрашивается один раз при подключении.
/// Спрашивается один раз потому, что каждый вызов Is*Support в горячем
/// цикле — это трафик по USB ради значения, которое не меняется.
struct Passport {
    bool valid = false;
    uint64_t id = 0;
    std::string model;
    std::vector< unsigned int > sampleRates;
    unsigned int memoryKb = 0;
    int adcBits = 8;
    bool hasHardTrigger = false;
    bool hasTriggerForce = false;
    bool hasRollMode = false;
    bool hasDds = false;
    bool hasAcDc = false; ///< уже с учётом обратной полярности вендора
    std::array< double, 2 > resolution = { 0.0, 0.0 }; ///< шаг на код, по каналам

    // --- Возможности, которых на пути B нет вовсе ---
    // Генератор и цифровой ввод-вывод у sigrok и его потомков не поддержаны
    // в принципе. Спрашиваются один раз при подключении: это свойства
    // прибора, а не состояние.
    bool hasIo = false;
    int ioChannels = 0;
    int ddsBoxingMask = 0;      ///< маска поддержанных форм сигнала
    bool ddsSoftZoomBias = false; ///< управляемы ли размах и смещение
    int ddsZoomMin = 0, ddsZoomMax = 0; ///< границы «сопротивления» размаха
    int ddsBiasMin = 0, ddsBiasMax = 0; ///< границы «сопротивления» смещения

    // --- Триггер: что прибор умеет сверх программного поиска ---
    bool hasTriggerSense = false;
    bool hasPreTriggerPercent = false;
    int pulseWidthNsMin = 0, pulseWidthNsMax = 0;
};

/// Настройки генератора. Имена вендора обманывают, и это разобрано по
/// пиньиню его внутренних символов: **`Zoom` — размах Vpp** (`FengFengzhi`,
/// 峰峰值), **`Bias` — постоянное смещение** (`Pianzhi`, 偏置); слово
/// «сопротивление» называет способ (цифровой потенциометр), а не величину.
struct DdsSettings {
    unsigned int boxingStyle = 0; ///< 0 синус, 1 меандр, 2 треугольник, 3/4 пилы
    unsigned int frequencyHz = 0; ///< ЦЕЛЫЕ герцы: дробных вендор не принимает
    int dutyPercent = 50;
    bool output = false;
    int zoomResistance = -1; ///< <0 — не трогать
    int biasResistance = -1; ///< <0 — не трогать
};

/// Настройки, которые коннектор держит сам: геттера предела в API нет,
/// прочитать их у прибора неоткуда, и после каждого восстановления их
/// надо записать заново.
struct Settings {
    unsigned int samplerate = 0;
    std::array< RangeMv, 2 > range = { RangeMv{ 0, 0 }, RangeMv{ 0, 0 } };
    std::array< bool, 2 > acCoupling = { false, false };
};

/// Кадр в единицах конвейера CtPU: вольты.
struct Frame {
    bool valid = false;
    unsigned int samplerate = 0;
    std::array< std::vector< double >, 2 > channel;
    std::array< bool, 2 > clipped = { false, false };
};

class Session {
  public:
    enum class State {
        Closed,    ///< библиотека не поднята
        Idle,      ///< прибор есть, захват не запущен
        Capturing, ///< Capture отдан, готовности нет
        Ready      ///< кадр готов и ещё не прочитан
    };

    Session( Api &api, Clock &clock );

    /// InitDll → пауза на перечисление USB → IsDevAvailable → паспорт.
    /// Пауза нужна не библиотеке (InitDll возвращается за ~100 мс), а
    /// перечислению USB: без неё IsDevAvailable отвечает 0 на исправном
    /// приборе.
    bool open();
    void close();

    State state() const { return m_state; }
    const Passport &passport() const { return m_passport; }
    const Settings &settings() const { return m_settings; }
    const std::string &lastError() const { return m_lastError; }

    /// Пишет в прибор ТОЛЬКО изменившееся. Отказывает, пока кадр не
    /// прочитан: смена скорости в окне между готовностью и чтением
    /// теряет данные на 205-й серии.
    bool applySettings( const Settings &wanted );

    /// Запускает захват. force = 1 всегда: без него библиотека режет
    /// захват примерно секундой данных. Длина в КБ; фактическая длина —
    /// возврат Capture, она и запоминается.
    bool startCapture( unsigned int wantedKb );

    unsigned int capturedKb() const { return m_capturedKb; }

    /// Опрос готовности с шагом 1 мс. Расчётного сна pts/sr здесь нет
    /// намеренно: он измерен неверным (теория 262 мс против факта 1.5 мс
    /// на 1 MSps) — IsDataReady означает готовность передачи, а не
    /// заполнение памяти прибора.
    bool waitReady( int timeoutMs );

    /// Читает оба канала. Длина берётся по факту возврата, каналы
    /// режутся по общей длине, флаг насыщения спрашивается на канал.
    Frame readFrame();

    /// Трёхступенчатое восстановление. После каждой ступени настройки
    /// записываются заново — восстановление их не сохраняет, а прочитать
    /// их у прибора нечем.
    bool recover();

    /// Сколько раз подряд восстановление не помогло.
    int failedRecoveries() const { return m_failedRecoveries; }

    /// Применяет настройки генератора. Отказывает, если прибор DDS не
    /// имеет, если форма не объявлена в маске поддержки или если размах
    /// и смещение запрошены на приборе, который ими управлять не умеет.
    /// Каждая запись, у которой есть парный геттер, читается обратно:
    /// возврата у сеттеров вендора нет, и это единственная проверка.
    bool applyDds( const DdsSettings &wanted );

    const DdsSettings &dds() const { return m_dds; }

    /// Множитель приведения к вольтам прошёл проверку правдоподобия.
    /// Ложь означает, что единица данных не установлена: кадры в
    /// конвейер не идут.
    bool unitsChecked() const { return m_unitsChecked; }

  private:
    bool readPassport();
    bool writeSettings( const Settings &wanted, bool force );
    void setError( std::string text );

    Api &m_api;
    Clock &m_clock;
    State m_state = State::Closed;
    Passport m_passport;
    Settings m_settings;
    Settings m_pending; ///< последнее, что просили записать
    DdsSettings m_dds;
    bool m_settingsWritten = false;
    bool m_unitsChecked = false;
    unsigned int m_capturedKb = 0;
    int m_failedRecoveries = 0;
    std::string m_lastError;
};

/// Задержки и таймауты пути A. Взяты из демо вендора и из прототипа,
/// работающего на живом приборе; здесь они собраны в одно место, чтобы
/// у величины не оказалось второго источника.
namespace Timing {
constexpr int USB_ENUMERATION_MS = 1000;  ///< после InitDll
constexpr int READY_POLL_STEP_MS = 1;     ///< шаг опроса IsDataReady
constexpr int READY_TIMEOUT_MS = 2000;    ///< боевой цикл прототипа
constexpr int RECOVER_FORCE_MS = 300;     ///< ступень 1: TriggerForce
constexpr int RECOVER_RESET_MS = 1500;    ///< ступень 2: ResetDevice
constexpr int RECOVER_FINISH_MS = 1000;   ///< ступень 3: FinishDll → InitDll
constexpr int MAX_FAILED_RECOVERIES = 3;  ///< дальше — сдаться и сказать
} // namespace Timing

} // namespace IVdso
