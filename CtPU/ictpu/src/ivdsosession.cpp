// SPDX-License-Identifier: GPL-3.0-or-later
// Last edited: 2026-09-03 UTC

#include "ivdsosession.h"

#include <algorithm>
#include <string>

namespace IVdso {

Session::Session( Api &api, Clock &clock ) : m_api( api ), m_clock( clock ) {}

void Session::setError( std::string text ) { m_lastError = std::move( text ); }

bool Session::open() {
    if ( m_state != State::Closed ) {
        setError( "open(): библиотека уже поднята" );
        return false;
    }
    if ( m_api.InitDll() != 1 ) {
        setError( "InitDll вернул отказ" );
        return false;
    }
    m_clock.sleepMs( Timing::USB_ENUMERATION_MS );
    if ( m_api.IsDevAvailable() != 1 ) {
        setError( "прибор не отвечает после перечисления USB" );
        m_api.FinishDll();
        return false;
    }
    if ( !readPassport() ) {
        m_api.FinishDll();
        return false;
    }
    m_state = State::Idle;
    m_settingsWritten = false;
    m_failedRecoveries = 0;
    return true;
}

void Session::close() {
    if ( m_state == State::Closed )
        return;
    // FinishDll зовётся из потока захвата, а не из потока панели: на
    // Linux-сборке той же библиотеки он однажды не вернулся вовсе
    // (деструктор ждёт поток, не разбудив его). На Windows такого не
    // наблюдалось, но место известно.
    m_api.FinishDll();
    m_state = State::Closed;
    m_settingsWritten = false;
    m_passport = Passport{};
}

bool Session::readPassport() {
    Passport p;
    p.id = deviceId( m_api.GetOnlyId0(), m_api.GetOnlyId1() );

    const int n = m_api.GetOscSupportSampleNum();
    if ( n > 0 ) {
        std::vector< unsigned int > buf( size_t( n ), 0u );
        // Возврат — сколько ЗАПИСАЛ, а не сколько поддерживается.
        const int written = m_api.GetOscSupportSamples( buf.data(), n );
        if ( written > 0 ) {
            buf.resize( size_t( std::min( written, n ) ) );
            p.sampleRates = buf;
        }
    }
    if ( p.sampleRates.empty() ) {
        setError( "ряд скоростей выборки пуст: прибор не опознан" );
        return false;
    }

    p.memoryKb = m_api.GetMemoryLength();
    p.hasHardTrigger = m_api.IsSupportHardTrigger() == 1;
    p.hasTriggerForce = m_api.IsSupportTriggerForce() == 1;
    p.hasRollMode = m_api.IsSupportRollMode() == 1;
    p.hasDds = m_api.IsSupportDDSDevice() == 1;
    p.hasAcDc = acDcSupported( m_api.IsSupportAcDc() );

    const unsigned int srMax = *std::max_element( p.sampleRates.begin(), p.sampleRates.end() );
    p.model = modelName( srMax, p.hasDds );
    p.adcBits = adcBits( p.model );
    p.resolution[ 0 ] = m_api.GetVoltageResolution( char( 0 ) );
    p.resolution[ 1 ] = m_api.GetVoltageResolution( char( 1 ) );

    // Триггер сверх программного поиска.
    p.hasTriggerSense = m_api.IsSupportTriggerSense() == 1;
    p.hasPreTriggerPercent = m_api.IsSupportPreTriggerPercent();
    if ( p.hasHardTrigger ) {
        p.pulseWidthNsMin = m_api.GetTriggerPulseWidthNsMin();
        p.pulseWidthNsMax = m_api.GetTriggerPulseWidthNsMax();
    }

    // Генератор. Формы спрашиваются ДВУМЯ вызовами: с нулевым указателем
    // вендор возвращает количество, с ненулевым — заполняет и возвращает
    // то же количество.
    if ( p.hasDds ) {
        const int styles = m_api.GetDDSSupportBoxingStyle( nullptr );
        if ( styles > 0 ) {
            int mask = 0;
            m_api.GetDDSSupportBoxingStyle( &mask );
            p.ddsBoxingMask = mask;
        }
        p.ddsSoftZoomBias = m_api.IsDDSSupportSoftwareControlZoomBias() == 1;
        if ( p.ddsSoftZoomBias ) {
            p.ddsZoomMin = m_api.GetDDSZoomResistanceRangeMin();
            p.ddsZoomMax = m_api.GetDDSZoomResistanceRangeMax();
            p.ddsBiasMin = m_api.GetDDSBiasResistanceRangeMin();
            p.ddsBiasMax = m_api.GetDDSBiasResistanceRangeMax();
        }
    }

    // Цифровой ввод-вывод.
    p.hasIo = m_api.IsSupportIODevice() == 1;
    if ( p.hasIo )
        p.ioChannels = m_api.GetSupportIoNumber();

    p.valid = true;
    m_passport = p;
    return true;
}

bool Session::writeSettings( const Settings &wanted, bool force ) {
    // Бессмысленные настройки отсекаются ДО прибора. Иначе нулевая
    // скорость совпала бы с нулём начального состояния, «изменений
    // нет» пропустило бы её мимо записи, и захват пошёл бы на
    // неустановленной скорости — без единого сообщения.
    if ( wanted.samplerate == 0 ) {
        setError( "настройки: скорость выборки 0" );
        return false;
    }
    for ( int ch = 0; ch < 2; ++ch ) {
        if ( wanted.range[ ch ].maxMv <= wanted.range[ ch ].minMv ) {
            setError( "настройки: пустой предел канала " + std::to_string( ch ) );
            return false;
        }
    }

    // Первая запись идёт целиком: начальное состояние — это не то, что
    // стоит в приборе, а то, чего мы про него не знаем.
    force = force || !m_settingsWritten;

    // Скорость: возврат — фактически установленная, 0 = отказ.
    if ( force || wanted.samplerate != m_settings.samplerate ) {
        const unsigned int got = m_api.SetOscSample( wanted.samplerate );
        if ( got == 0 ) {
            setError( "SetOscSample вернул отказ" );
            return false;
        }
        m_settings.samplerate = got; // истина — то, что прибор принял
    }
    for ( int ch = 0; ch < 2; ++ch ) {
        if ( force || !( wanted.range[ ch ] == m_settings.range[ ch ] ) ) {
            if ( m_api.SetOscChannelRange( ch, wanted.range[ ch ].minMv,
                                           wanted.range[ ch ].maxMv ) != 1 ) {
                setError( "SetOscChannelRange вернул отказ" );
                return false;
            }
            m_settings.range[ ch ] = wanted.range[ ch ];
        }
        if ( m_passport.hasAcDc && ( force || wanted.acCoupling[ ch ] != m_settings.acCoupling[ ch ] ) ) {
            // Возврата у сеттера нет; единственная проверка — чтение назад.
            m_api.SetAcDc( (unsigned int) ch, wanted.acCoupling[ ch ] ? 1 : 0 );
            const bool got = m_api.GetAcDc( (unsigned int) ch ) == 1;
            if ( got != wanted.acCoupling[ ch ] ) {
                setError( "SetAcDc не дошёл: чтение назад дало другое значение" );
                return false;
            }
            m_settings.acCoupling[ ch ] = got;
        }
    }
    m_pending = wanted;
    m_pending.samplerate = m_settings.samplerate;
    m_settingsWritten = true;

    // Единица данных проверяется на записанном пределе, а не на
    // запрошенном: правдоподобие считается от того, что принял прибор.
    m_unitsChecked = resolutionPlausible( m_passport.resolution[ 0 ], m_settings.range[ 0 ],
                                          m_passport.adcBits, SAMPLE_TO_VOLT );
    return true;
}

bool Session::applySettings( const Settings &wanted ) {
    if ( m_state == State::Closed ) {
        setError( "applySettings(): библиотека не поднята" );
        return false;
    }
    if ( m_state != State::Idle ) {
        // Смена скорости между готовностью и чтением теряет данные на
        // 205-й серии; на 210B безопасна. Правило берётся по худшему.
        setError( "applySettings(): захват не завершён, кадр не прочитан" );
        return false;
    }
    return writeSettings( wanted, false );
}

bool Session::startCapture( unsigned int wantedKb ) {
    if ( m_state != State::Idle ) {
        setError( "startCapture(): состояние не Idle" );
        return false;
    }
    if ( !m_settingsWritten ) {
        setError( "startCapture(): настройки в прибор не записаны" );
        return false;
    }
    if ( wantedKb == 0 ) {
        setError( "startCapture(): длина 0 КБ" );
        return false;
    }
    // force = 1 всегда: иначе библиотека ограничивает захват примерно
    // секундой данных, и на низких скоростях длина молча урезается.
    const int realKb = m_api.Capture( int( wantedKb ), char( 1 ) );
    if ( realKb <= 0 ) {
        setError( "Capture вернул отказ" );
        return false;
    }
    m_capturedKb = (unsigned int) realKb; // истина — возврат, не запрос
    m_state = State::Capturing;
    return true;
}

bool Session::waitReady( int timeoutMs ) {
    if ( m_state == State::Ready )
        return true;
    if ( m_state != State::Capturing ) {
        setError( "waitReady(): захват не запущен" );
        return false;
    }
    const uint64_t t0 = m_clock.nowMs();
    for ( ;; ) {
        if ( m_api.IsDataReady() == 1 ) {
            m_state = State::Ready;
            return true;
        }
        if ( int( m_clock.nowMs() - t0 ) >= timeoutMs ) {
            setError( "готовности нет: таймаут" );
            return false;
        }
        m_clock.sleepMs( Timing::READY_POLL_STEP_MS );
    }
}

Frame Session::readFrame() {
    Frame f;
    if ( m_state != State::Ready ) {
        // Чтение до готовности на 205A/B — access violation, а не
        // неполный кадр. Запрет стоит здесь, а не в вызывающем коде.
        setError( "readFrame(): кадр не готов" );
        return f;
    }
    if ( !m_unitsChecked ) {
        setError( "readFrame(): единица данных не установлена, кадр не отдаётся" );
        m_state = State::Idle;
        return f;
    }
    const unsigned int points = pointsFromKb( m_capturedKb );
    std::vector< double > buf0( points, 0.0 ), buf1( points, 0.0 );
    const unsigned int n0 = m_api.ReadVoltageDatas( char( 0 ), buf0.data(), points );
    const unsigned int n1 = m_api.ReadVoltageDatas( char( 1 ), buf1.data(), points );
    // Каналы возвращают разное число отсчётов, и оба — меньше
    // запрошенного: на 205A/B при пределах ≥2000 мВ на запрос 4096
    // приходит 2044. По длине окна считает FFT, поэтому берётся факт.
    const unsigned int n = commonLength( n0, n1 );
    if ( n == 0 ) {
        setError( "ReadVoltageDatas вернул 0 отсчётов" );
        m_state = State::Idle;
        return f;
    }
    buf0.resize( n );
    buf1.resize( n );
    if ( SAMPLE_TO_VOLT != 1.0 ) {
        for ( auto &v : buf0 )
            v *= SAMPLE_TO_VOLT;
        for ( auto &v : buf1 )
            v *= SAMPLE_TO_VOLT;
    }
    f.channel[ 0 ] = std::move( buf0 );
    f.channel[ 1 ] = std::move( buf1 );
    f.clipped[ 0 ] = clipped( m_api.IsVoltageDatasOutRange( char( 0 ) ) );
    f.clipped[ 1 ] = clipped( m_api.IsVoltageDatasOutRange( char( 1 ) ) );
    f.samplerate = m_settings.samplerate;
    f.valid = true;
    m_state = State::Idle;
    return f;
}

bool Session::applyDds( const DdsSettings &wanted ) {
    if ( m_state == State::Closed ) {
        setError( "applyDds(): библиотека не поднята" );
        return false;
    }
    if ( !m_passport.hasDds ) {
        setError( "applyDds(): прибор генератора не имеет" );
        return false;
    }
    // Форма проверяется по маске прибора, а не по списку имён: вендор
    // объявил пять имён, а маска приходит семибитная — формы 5 и 6 не
    // документированы, и запрещать их по списку имён было бы враньём.
    if ( m_passport.ddsBoxingMask && !( m_passport.ddsBoxingMask & ( 1 << wanted.boxingStyle ) ) ) {
        setError( "applyDds(): прибор такой формы сигнала не объявляет" );
        return false;
    }
    if ( wanted.dutyPercent < 1 || wanted.dutyPercent > 99 ) {
        setError( "applyDds(): скважность вне 1…99 %" );
        return false;
    }
    const bool wantsZoomBias = wanted.zoomResistance >= 0 || wanted.biasResistance >= 0;
    if ( wantsZoomBias && !m_passport.ddsSoftZoomBias ) {
        setError( "applyDds(): размахом и смещением этот прибор программно не управляет" );
        return false;
    }

    m_api.SetDDSBoxingStyle( wanted.boxingStyle );
    // Частота — ЦЕЛЫЕ герцы. Парного геттера у неё нет вовсе, проверить
    // запись нечем; это записано, а не замолчано.
    m_api.SetDDSPinlv( wanted.frequencyHz );
    m_api.SetDDSDutyCycle( wanted.dutyPercent );

    if ( wanted.zoomResistance >= 0 ) {
        m_api.SetDDSZoomResistance( wanted.zoomResistance );
        if ( m_api.GetDDSZoomResistance() != wanted.zoomResistance ) {
            setError( "applyDds(): размах не дошёл — чтение назад дало другое значение" );
            return false;
        }
    }
    if ( wanted.biasResistance >= 0 ) {
        m_api.SetDDSBiasResistance( wanted.biasResistance );
        if ( m_api.GetDDSBiasResistance() != wanted.biasResistance ) {
            setError( "applyDds(): смещение не дошло — чтение назад дало другое значение" );
            return false;
        }
    }

    m_api.DDSOutputEnable( wanted.output ? 1 : 0 );
    if ( ( m_api.IsDDSOutputEnable() == 1 ) != wanted.output ) {
        setError( "applyDds(): выход не переключился" );
        return false;
    }

    m_dds = wanted;
    return true;
}


bool Session::recover() {
    if ( m_state == State::Closed ) {
        setError( "recover(): библиотека не поднята" );
        return false;
    }
    const Settings wanted = m_pending;

    auto settled = [ & ]() -> bool {
        if ( m_api.IsDevAvailable() != 1 )
            return false;
        m_state = State::Idle;
        m_settings = Settings{}; // прибор настроек не сохранил
        if ( !writeSettings( wanted, true ) )
            return false;
        m_failedRecoveries = 0;
        return true;
    };

    // Ступень 1: расклинить принудительным запуском. Только если прибор
    // это умеет — возврата у TriggerForce нет, на неумеющем это вызов в
    // пустоту, который нечем отследить.
    if ( m_passport.hasTriggerForce ) {
        m_api.TriggerForce();
        m_clock.sleepMs( Timing::RECOVER_FORCE_MS );
        if ( settled() )
            return true;
    }

    // Ступень 2: сброс прибора. Зовётся только на потерянном приборе,
    // а не каждый цикл.
    m_api.ResetDevice();
    m_clock.sleepMs( Timing::RECOVER_RESET_MS );
    if ( settled() )
        return true;

    // Ступень 3: перезапуск библиотеки. Лечит накопленную
    // рассинхронизацию, которую сброс не снимает: симптом — Capture
    // принят, а готовность не наступает никогда. В горячий цикл эту
    // ступень ставить нельзя, она стоит секунду.
    m_api.FinishDll();
    m_clock.sleepMs( Timing::RECOVER_FINISH_MS );
    if ( m_api.InitDll() != 1 ) {
        setError( "восстановление: InitDll вернул отказ" );
        m_state = State::Closed;
        ++m_failedRecoveries;
        return false;
    }
    m_clock.sleepMs( Timing::USB_ENUMERATION_MS );
    if ( settled() )
        return true;

    ++m_failedRecoveries;
    setError( "восстановление не помогло" );
    return false;
}

} // namespace IVdso
