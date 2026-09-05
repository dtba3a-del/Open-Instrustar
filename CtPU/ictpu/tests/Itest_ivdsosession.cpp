// SPDX-License-Identifier: GPL-3.0-or-later
// Last edited: 2026-09-03 UTC
//
// Путь A: порядок вызовов vdso на ПОДДЕЛЬНОМ приборе.
//
// Три запрета пути A на ISDS205 не дают ни отказа сборки, ни сообщения
// об ошибке: чтение до готовности — access violation, смена скорости
// между готовностью и чтением — молча потерянный кадр, длина из запроса
// вместо возврата — частота, посчитанная по несуществующей длине окна.
// Поддельный прибор делает все три проверяемыми здесь, а не на железе.

#include "ivdsosession.h"

#include <QtTest>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

using namespace IVdso;

namespace {

/// Часы, которые не спят: тест двигает время сам и записывает, сколько
/// у него просили.
class FakeClock : public Clock {
  public:
    void sleepMs( int ms ) override {
        slept.push_back( ms );
        now += uint64_t( ms > 0 ? ms : 0 );
        log.push_back( "sleep:" + std::to_string( ms ) );
    }
    uint64_t nowMs() override { return now; }

    uint64_t now = 0;
    std::vector< int > slept;
    std::vector< std::string > log;
};

/// Поддельный ISDS205B: отвечает так, как отвечал измеренный прибор,
/// и записывает каждый вызов.
class FakeDevice : public Api {
  public:
    explicit FakeDevice( FakeClock &clock ) : m_log( clock.log ) {}

    // Настраиваемое поведение
    int initResult = 1;
    int availResult = 1;
    int captureReturnsKb = -1;    ///< <0 — вернуть запрошенное
    int readyAfterPolls = 0;      ///< сколько раз ответить «не готов»
    unsigned int nCh0 = 0, nCh1 = 0; ///< <>0 — отдать столько отсчётов
    bool readReturnsZero = false;    ///< прибор отдал пустой кадр
    int outRange0 = 0, outRange1 = 0;
    double resolution = 0.003355; ///< измерено на ISDS205B
    unsigned int setSampleResult = 0; ///< 0 — вернуть запрошенное
    int rangeResult = 1;
    bool supportsAcDc = false;    ///< на 205B измерено: не поддерживается
    bool supportsTriggerForce = false;
    bool hasDdsDevice = true;
    int availableAfterStage = -1; ///< на какой ступени восстановления ожить
    int ddsBoxingMask = 0x1F;     ///< пять объявленных форм
    bool ddsSoftZoomBias = true;
    bool ioSupported = false;
    int ioNumber = 0;
    bool ddsZoomSticks = true;    ///< доходит ли запись размаха
    bool ddsOutputSticks = true;  ///< доходит ли переключение выхода

    std::vector< std::string > &log() { return m_log; }

    bool called( const std::string &name ) const {
        for ( const auto &s : m_log )
            if ( s == name || s.compare( 0, name.size() + 1, name + ":" ) == 0 )
                return true;
        return false;
    }
    int countOf( const std::string &name ) const {
        int n = 0;
        for ( const auto &s : m_log )
            if ( s == name || s.compare( 0, name.size() + 1, name + ":" ) == 0 )
                ++n;
        return n;
    }
    int indexOf( const std::string &name ) const {
        for ( size_t i = 0; i < m_log.size(); ++i )
            if ( m_log[ i ] == name || m_log[ i ].compare( 0, name.size() + 1, name + ":" ) == 0 )
                return int( i );
        return -1;
    }

    int InitDll() override {
        m_log.push_back( "InitDll" );
        ++m_stage;
        return initResult;
    }
    int FinishDll() override {
        m_log.push_back( "FinishDll" );
        return 1;
    }
    int IsDevAvailable() override {
        m_log.push_back( "IsDevAvailable" );
        if ( availableAfterStage >= 0 )
            return m_stage >= availableAfterStage ? 1 : 0;
        return availResult;
    }
    unsigned int GetOnlyId0() override {
        m_log.push_back( "GetOnlyId0" );
        return 3136672271u;
    }
    unsigned int GetOnlyId1() override {
        m_log.push_back( "GetOnlyId1" );
        return 122u;
    }
    int ResetDevice() override {
        m_log.push_back( "ResetDevice" );
        ++m_stage;
        return 1;
    }
    int SetOscChannelRange( int channel, int minmv, int maxmv ) override {
        m_log.push_back( "SetOscChannelRange:" + std::to_string( channel ) + ":" +
                         std::to_string( minmv ) + ":" + std::to_string( maxmv ) );
        return rangeResult;
    }
    int IsSupportAcDc() override {
        m_log.push_back( "IsSupportAcDc" );
        return supportsAcDc ? 0 : 1; // полярность обратная
    }
    void SetAcDc( unsigned int chn, int ac ) override {
        m_log.push_back( "SetAcDc:" + std::to_string( chn ) + ":" + std::to_string( ac ) );
        m_ac[ chn ] = ac;
    }
    int GetAcDc( unsigned int chn ) override {
        m_log.push_back( "GetAcDc:" + std::to_string( chn ) );
        return m_ac.count( chn ) ? m_ac[ chn ] : 0;
    }
    int GetOscSupportSampleNum() override {
        m_log.push_back( "GetOscSupportSampleNum" );
        return int( m_rates.size() );
    }
    int GetOscSupportSamples( unsigned int *sample, int maxnum ) override {
        m_log.push_back( "GetOscSupportSamples" );
        const int n = std::min( maxnum, int( m_rates.size() ) );
        for ( int i = 0; i < n; ++i )
            sample[ i ] = m_rates[ size_t( i ) ];
        return n;
    }
    unsigned int GetOscSample() override {
        m_log.push_back( "GetOscSample" );
        return m_sample;
    }
    unsigned int SetOscSample( unsigned int sample ) override {
        m_log.push_back( "SetOscSample:" + std::to_string( sample ) );
        m_sample = setSampleResult ? setSampleResult : sample;
        return m_sample;
    }
    unsigned int GetMemoryLength() override {
        m_log.push_back( "GetMemoryLength" );
        return 1024u;
    }
    int Capture( int lengthKb, char force ) override {
        m_log.push_back( "Capture:" + std::to_string( lengthKb ) + ":" +
                         std::to_string( int( force ) ) );
        m_polls = 0;
        return captureReturnsKb < 0 ? lengthKb : captureReturnsKb;
    }
    int IsDataReady() override {
        m_log.push_back( "IsDataReady" );
        return m_polls++ >= readyAfterPolls ? 1 : 0;
    }
    unsigned int ReadVoltageDatas( char channel, double *buffer, unsigned int length ) override {
        m_log.push_back( "ReadVoltageDatas:" + std::to_string( int( channel ) ) + ":" +
                         std::to_string( length ) );
        if ( readReturnsZero )
            return 0;
        unsigned int n = channel == 0 ? nCh0 : nCh1;
        if ( n == 0 )
            n = length;
        n = std::min( n, length );
        for ( unsigned int i = 0; i < n; ++i )
            buffer[ i ] = double( channel + 1 ) * double( i );
        return n;
    }
    int IsVoltageDatasOutRange( char channel ) override {
        m_log.push_back( "IsVoltageDatasOutRange:" + std::to_string( int( channel ) ) );
        return channel == 0 ? outRange0 : outRange1;
    }
    double GetVoltageResolution( char channel ) override {
        m_log.push_back( "GetVoltageResolution:" + std::to_string( int( channel ) ) );
        return resolution;
    }
    int IsSupportHardTrigger() override {
        m_log.push_back( "IsSupportHardTrigger" );
        return 0; // на ISDS205 измерено: аппаратного триггера нет
    }
    int IsSupportTriggerForce() override {
        m_log.push_back( "IsSupportTriggerForce" );
        return supportsTriggerForce ? 1 : 0;
    }
    void TriggerForce() override {
        m_log.push_back( "TriggerForce" );
        ++m_stage;
    }
    int IsSupportRollMode() override {
        m_log.push_back( "IsSupportRollMode" );
        return 0;
    }
    int IsSupportDDSDevice() override {
        m_log.push_back( "IsSupportDDSDevice" );
        return hasDdsDevice ? 1 : 0;
    }


    // --- Остальные экспорты полного набора: подделка их только
    //     записывает. Реализованы ВСЕ 73, иначе класс абстрактен, и это
    //     же есть машинная проверка полноты интерфейса. ---
    void SetDevNoticeCallBack( void* ppara, AddCallBack addcallback, RemoveCallBack rmvcallback ) override {
        m_log.push_back( "SetDevNoticeCallBack" );
        (void) rmvcallback;
        (void) addcallback;
        (void) ppara;
    }
    void SetDevNoticeEvent( void * addevent, void * rmvevent ) override {
        m_log.push_back( "SetDevNoticeEvent" );
        (void) rmvevent;
        (void) addevent;
    }
    unsigned int GetTriggerMode() override {
        m_log.push_back( "GetTriggerMode" );
        return 0;
    }
    void SetTriggerMode( unsigned int mode ) override {
        m_log.push_back( "SetTriggerMode" );
        (void) mode;
    }
    unsigned int GetTriggerStyle() override {
        m_log.push_back( "GetTriggerStyle" );
        return 0;
    }
    void SetTriggerStyle( unsigned int style ) override {
        m_log.push_back( "SetTriggerStyle" );
        (void) style;
    }
    int GetTriggerPulseWidthNsMin() override {
        m_log.push_back( "GetTriggerPulseWidthNsMin" );
        return 0;
    }
    int GetTriggerPulseWidthNsMax() override {
        m_log.push_back( "GetTriggerPulseWidthNsMax" );
        return 0;
    }
    int GetTriggerPulseWidthDownNs() override {
        m_log.push_back( "GetTriggerPulseWidthDownNs" );
        return 0;
    }
    int GetTriggerPulseWidthUpNs() override {
        m_log.push_back( "GetTriggerPulseWidthUpNs" );
        return 0;
    }
    void SetTriggerPulseWidthNs( int down_ns, int up_ns ) override {
        m_log.push_back( "SetTriggerPulseWidthNs" );
        (void) up_ns;
        (void) down_ns;
    }
    unsigned int GetTriggerSource() override {
        m_log.push_back( "GetTriggerSource" );
        return 0;
    }
    void SetTriggerSource( unsigned int source ) override {
        m_log.push_back( "SetTriggerSource" );
        (void) source;
    }
    int GetTriggerLevel() override {
        m_log.push_back( "GetTriggerLevel" );
        return 0;
    }
    void SetTriggerLevel( int level ) override {
        m_log.push_back( "SetTriggerLevel" );
        (void) level;
    }
    int IsSupportTriggerSense() override {
        m_log.push_back( "IsSupportTriggerSense" );
        return 0;
    }
    double GetTriggerSenseDiv() override {
        m_log.push_back( "GetTriggerSenseDiv" );
        return 0.0;
    }
    void SetTriggerSenseDiv( double sense ) override {
        m_log.push_back( "SetTriggerSenseDiv" );
        (void) sense;
    }
    bool IsSupportPreTriggerPercent() override {
        m_log.push_back( "IsSupportPreTriggerPercent" );
        return 0;
    }
    int GetPreTriggerPercent() override {
        m_log.push_back( "GetPreTriggerPercent" );
        return 0;
    }
    void SetPreTriggerPercent( int front ) override {
        m_log.push_back( "SetPreTriggerPercent" );
        (void) front;
    }
    int SetRollMode( unsigned int en ) override {
        m_log.push_back( "SetRollMode" );
        (void) en;
        return 0;
    }
    void SetDataReadyCallBack( void* ppara, DataReadyCallBack datacallback ) override {
        m_log.push_back( "SetDataReadyCallBack" );
        (void) datacallback;
        (void) ppara;
    }
    void SetDataReadyEvent( void * dataevent ) override {
        m_log.push_back( "SetDataReadyEvent" );
        (void) dataevent;
    }
    int GetDDSSupportBoxingStyle( int* style ) override {
        m_log.push_back( style ? "GetDDSSupportBoxingStyle:fill" : "GetDDSSupportBoxingStyle:count" );
        if ( style )
            *style = ddsBoxingMask;
        return 5; // количество объявленных имён форм
    }
    void SetDDSBoxingStyle( unsigned int boxing ) override {
        m_log.push_back( "SetDDSBoxingStyle" );
        (void) boxing;
    }
    void SetDDSPinlv( unsigned int pinlv ) override {
        m_log.push_back( "SetDDSPinlv" );
        (void) pinlv;
    }
    void SetDDSDutyCycle( int cycle ) override {
        m_log.push_back( "SetDDSDutyCycle" );
        (void) cycle;
    }
    void DDSOutputEnable( int enable ) override {
        m_log.push_back( "DDSOutputEnable:" + std::to_string( enable ) );
        if ( ddsOutputSticks )
            m_ddsOut = enable != 0;
    }
    int IsDDSOutputEnable() override {
        m_log.push_back( "IsDDSOutputEnable" );
        return m_ddsOut ? 1 : 0;
    }
    int IsDDSSupportSoftwareControlZoomBias() override {
        m_log.push_back( "IsDDSSupportSoftwareControlZoomBias" );
        return ddsSoftZoomBias ? 1 : 0;
    }
    int GetDDSBiasResistanceRangeMin() override {
        m_log.push_back( "GetDDSBiasResistanceRangeMin" );
        return 0;
    }
    int GetDDSBiasResistanceRangeMax() override {
        m_log.push_back( "GetDDSBiasResistanceRangeMax" );
        return 0;
    }
    void SetDDSBiasResistance( int Resistance ) override {
        m_log.push_back( "SetDDSBiasResistance:" + std::to_string( Resistance ) );
        m_bias = Resistance;
    }
    int GetDDSBiasResistance() override {
        m_log.push_back( "GetDDSBiasResistance" );
        return m_bias;
    }
    int GetDDSZoomResistanceRangeMin() override {
        m_log.push_back( "GetDDSZoomResistanceRangeMin" );
        return 0;
    }
    int GetDDSZoomResistanceRangeMax() override {
        m_log.push_back( "GetDDSZoomResistanceRangeMax" );
        return 0;
    }
    void SetDDSZoomResistance( int Resistance ) override {
        m_log.push_back( "SetDDSZoomResistance:" + std::to_string( Resistance ) );
        if ( ddsZoomSticks )
            m_zoom = Resistance;
    }
    int GetDDSZoomResistance() override {
        m_log.push_back( "GetDDSZoomResistance" );
        return m_zoom;
    }
    int IsSupportIODevice() override {
        m_log.push_back( "IsSupportIODevice" );
        return ioSupported ? 1 : 0;
    }
    int GetSupportIoNumber() override {
        m_log.push_back( "GetSupportIoNumber" );
        return ioNumber;
    }
    void SetIOReadStateCallBack( void* ppara, IOReadStateCallBack callback ) override {
        m_log.push_back( "SetIOReadStateCallBack" );
        (void) callback;
        (void) ppara;
    }
    void SetIOReadStateReadyEvent( void * dataevent ) override {
        m_log.push_back( "SetIOReadStateReadyEvent" );
        (void) dataevent;
    }
    int IsIOReadStateReady() override {
        m_log.push_back( "IsIOReadStateReady" );
        return 0;
    }
    void SetIOInOut( unsigned char channel, unsigned char inout ) override {
        m_log.push_back( "SetIOInOut" );
        (void) inout;
        (void) channel;
    }
    void SetIOState( unsigned char channel, unsigned char state ) override {
        m_log.push_back( "SetIOState" );
        (void) state;
        (void) channel;
    }
    void ReadIOState( unsigned char channel ) override {
        m_log.push_back( "ReadIOState" );
        (void) channel;
    }
    char GetIOState( unsigned char channel ) override {
        m_log.push_back( "GetIOState:" + std::to_string( int( channel ) ) );
        // Вендор объявил отказ значением −1 при типе char: приём в
        // unsigned char превратил бы его в 255.
        return char( -1 );
    }

  private:
    std::vector< std::string > &m_log;
    std::vector< unsigned int > m_rates = { 1000000, 4000000, 8000000, 16000000, 48000000 };
    unsigned int m_sample = 1000000;
    int m_polls = 0;
    int m_stage = 0;
    std::map< unsigned int, int > m_ac;
    int m_zoom = -1;
    int m_bias = -1;
    bool m_ddsOut = false;
};

Settings goodSettings() {
    Settings s;
    s.samplerate = 4000000;
    s.range[ 0 ] = RangeMv{ -1000, 1000 };
    s.range[ 1 ] = RangeMv{ -1000, 1000 };
    return s;
}

} // namespace

class TestIVdsoSession : public QObject {
    Q_OBJECT
  private slots:

    // --- подъём библиотеки ---

    void open_failsWhenInitDllRefuses() {
        FakeClock clk;
        FakeDevice dev( clk );
        dev.initResult = 0;
        Session s( dev, clk );
        QVERIFY( !s.open() );
        QCOMPARE( s.state(), Session::State::Closed );
        // Ни одного вызова после отказа InitDll быть не должно.
        QVERIFY( !dev.called( "IsDevAvailable" ) );
        QVERIFY( !s.lastError().empty() );
    }

    void open_waitsForUsbEnumerationBeforeAsking() {
        // Пауза нужна не библиотеке (InitDll возвращается за ~100 мс), а
        // перечислению USB: без неё IsDevAvailable отвечает 0 на
        // исправном приборе. Проверяется порядком, а не длительностью.
        FakeClock clk;
        FakeDevice dev( clk );
        Session s( dev, clk );
        QVERIFY( s.open() );
        const int iInit = dev.indexOf( "InitDll" );
        const int iSleep = dev.indexOf( "sleep:" + std::to_string( Timing::USB_ENUMERATION_MS ) );
        const int iAvail = dev.indexOf( "IsDevAvailable" );
        QVERIFY( iInit >= 0 && iSleep > iInit && iAvail > iSleep );
    }

    void open_failsWhenDeviceSilent() {
        FakeClock clk;
        FakeDevice dev( clk );
        dev.availResult = 0;
        Session s( dev, clk );
        QVERIFY( !s.open() );
        // Библиотеку надо опустить обратно, иначе следующий open()
        // придёт на уже поднятую.
        QVERIFY( dev.called( "FinishDll" ) );
    }

    void open_readsPassportOnce() {
        FakeClock clk;
        FakeDevice dev( clk );
        Session s( dev, clk );
        QVERIFY( s.open() );
        const Passport &p = s.passport();
        QVERIFY( p.valid );
        QCOMPARE( p.model, std::string( "ISDS205B" ) );
        QCOMPARE( p.id, deviceId( 3136672271u, 122u ) );
        QCOMPARE( p.memoryKb, 1024u );
        QCOMPARE( p.adcBits, 8 );
        QCOMPARE( p.sampleRates.size(), size_t( 5 ) );
        QVERIFY( p.hasDds );
        QVERIFY( !p.hasHardTrigger );
        // Полярность IsSupportAcDc обратная: прибор ответил 1,
        // значит НЕ поддерживается.
        QVERIFY( !p.hasAcDc );
    }

    void passport_isNotReReadInHotLoop() {
        FakeClock clk;
        FakeDevice dev( clk );
        Session s( dev, clk );
        QVERIFY( s.open() );
        Settings st = goodSettings();
        QVERIFY( s.applySettings( st ) );
        const int before = dev.countOf( "IsSupportDDSDevice" );
        for ( int i = 0; i < 3; ++i ) {
            QVERIFY( s.startCapture( 4 ) );
            QVERIFY( s.waitReady( Timing::READY_TIMEOUT_MS ) );
            QVERIFY( s.readFrame().valid );
        }
        // Свойство прибора не меняется; спрашивать его каждый кадр —
        // трафик по USB в горячем цикле.
        QCOMPARE( dev.countOf( "IsSupportDDSDevice" ), before );
    }

    // --- настройки: только изменившееся ---

    void settings_writeOnlyWhatChanged() {
        FakeClock clk;
        FakeDevice dev( clk );
        Session s( dev, clk );
        QVERIFY( s.open() );
        Settings st = goodSettings();
        QVERIFY( s.applySettings( st ) );
        const int rangeWrites = dev.countOf( "SetOscChannelRange" );
        QCOMPARE( rangeWrites, 2 );
        // Повтор тех же настроек не должен дойти до прибора вовсе.
        QVERIFY( s.applySettings( st ) );
        QCOMPARE( dev.countOf( "SetOscChannelRange" ), rangeWrites );
        QCOMPARE( dev.countOf( "SetOscSample" ), 1 );
        // А изменившийся предел — должен, и только он.
        st.range[ 1 ] = RangeMv{ -2000, 2000 };
        QVERIFY( s.applySettings( st ) );
        QCOMPARE( dev.countOf( "SetOscChannelRange" ), rangeWrites + 1 );
        QCOMPARE( dev.countOf( "SetOscSample" ), 1 );
    }

    void settings_samplerateTruthIsWhatDeviceAccepted() {
        FakeClock clk;
        FakeDevice dev( clk );
        dev.setSampleResult = 8000000; // прибор принял не то, что просили
        Session s( dev, clk );
        QVERIFY( s.open() );
        QVERIFY( s.applySettings( goodSettings() ) );
        QCOMPARE( s.settings().samplerate, 8000000u );
    }

    void settings_refuseWhenSetOscSampleFails() {
        FakeClock clk;
        FakeDevice dev( clk );
        Session s( dev, clk );
        QVERIFY( s.open() );
        Settings st = goodSettings();
        st.samplerate = 0; // SetOscSample об отказе сообщает нулём
        QVERIFY( !s.applySettings( st ) );
    }

    void settings_acDcNotTouchedWhenUnsupported() {
        FakeClock clk;
        FakeDevice dev( clk );
        Session s( dev, clk );
        QVERIFY( s.open() );
        Settings st = goodSettings();
        st.acCoupling[ 0 ] = true;
        QVERIFY( s.applySettings( st ) );
        // У сеттера возврата нет: на неумеющем приборе это вызов в
        // пустоту, который нечем отследить. Значит его не должно быть.
        QVERIFY( !dev.called( "SetAcDc" ) );
    }

    void settings_acDcVerifiedByReadingBack() {
        FakeClock clk;
        FakeDevice dev( clk );
        dev.supportsAcDc = true;
        Session s( dev, clk );
        QVERIFY( s.open() );
        Settings st = goodSettings();
        st.acCoupling[ 0 ] = true;
        QVERIFY( s.applySettings( st ) );
        QVERIFY( dev.called( "SetAcDc" ) );
        // Единственная возможная проверка того, что запись дошла.
        QVERIFY( dev.called( "GetAcDc" ) );
    }

    // --- запрет: смена настроек между готовностью и чтением ---

    void settings_refusedBetweenReadyAndRead() {
        FakeClock clk;
        FakeDevice dev( clk );
        Session s( dev, clk );
        QVERIFY( s.open() );
        QVERIFY( s.applySettings( goodSettings() ) );
        QVERIFY( s.startCapture( 4 ) );
        Settings other = goodSettings();
        other.samplerate = 8000000;
        // Захват не завершён: на 205-й серии смена скорости здесь
        // теряет данные.
        QVERIFY( !s.applySettings( other ) );
        QVERIFY( s.waitReady( Timing::READY_TIMEOUT_MS ) );
        QCOMPARE( s.state(), Session::State::Ready );
        // Кадр готов, но не прочитан — запрет тот же.
        QVERIFY( !s.applySettings( other ) );
        QVERIFY( s.readFrame().valid );
        // Кадр прочитан — теперь можно.
        QVERIFY( s.applySettings( other ) );
    }

    // --- запуск захвата ---

    void capture_alwaysForcesLength() {
        FakeClock clk;
        FakeDevice dev( clk );
        Session s( dev, clk );
        QVERIFY( s.open() );
        QVERIFY( s.applySettings( goodSettings() ) );
        QVERIFY( s.startCapture( 4 ) );
        // Без force библиотека режет захват примерно секундой данных.
        QVERIFY( dev.called( "Capture:4:1" ) );
    }

    void capture_lengthTruthIsTheReturn() {
        FakeClock clk;
        FakeDevice dev( clk );
        dev.captureReturnsKb = 2; // просили 4 КБ, прибор дал 2
        Session s( dev, clk );
        QVERIFY( s.open() );
        QVERIFY( s.applySettings( goodSettings() ) );
        QVERIFY( s.startCapture( 4 ) );
        QCOMPARE( s.capturedKb(), 2u );
    }

    void capture_refusedBeforeSettingsWritten() {
        FakeClock clk;
        FakeDevice dev( clk );
        Session s( dev, clk );
        QVERIFY( s.open() );
        QVERIFY( !s.startCapture( 4 ) );
        QVERIFY( !dev.called( "Capture" ) );
    }

    // --- запрет: чтение до готовности ---

    void read_refusedBeforeDataReady() {
        FakeClock clk;
        FakeDevice dev( clk );
        Session s( dev, clk );
        QVERIFY( s.open() );
        QVERIFY( s.applySettings( goodSettings() ) );
        QVERIFY( s.startCapture( 4 ) );
        const Frame f = s.readFrame();
        QVERIFY( !f.valid );
        // На 205A/B это не неполный кадр, а access violation.
        QVERIFY( !dev.called( "ReadVoltageDatas" ) );
    }

    void wait_pollsUntilReadyThenStops() {
        FakeClock clk;
        FakeDevice dev( clk );
        dev.readyAfterPolls = 3;
        Session s( dev, clk );
        QVERIFY( s.open() );
        QVERIFY( s.applySettings( goodSettings() ) );
        QVERIFY( s.startCapture( 4 ) );
        QVERIFY( s.waitReady( Timing::READY_TIMEOUT_MS ) );
        QCOMPARE( dev.countOf( "IsDataReady" ), 4 );
    }

    void wait_timesOutWithoutHanging() {
        FakeClock clk;
        FakeDevice dev( clk );
        dev.readyAfterPolls = 1000000; // «Capture принят, готовности нет»
        Session s( dev, clk );
        QVERIFY( s.open() );
        QVERIFY( s.applySettings( goodSettings() ) );
        QVERIFY( s.startCapture( 4 ) );
        QVERIFY( !s.waitReady( 50 ) );
        QCOMPARE( s.state(), Session::State::Capturing );
    }

    // --- чтение кадра ---

    void read_lengthFromReturnNotFromRequest() {
        FakeClock clk;
        FakeDevice dev( clk );
        // Аномалия 205A/B: на запрос 4096 при пределах ≥2000 мВ
        // приходит 2044. По длине окна считает FFT.
        dev.nCh0 = 2044;
        dev.nCh1 = 2044;
        Session s( dev, clk );
        QVERIFY( s.open() );
        QVERIFY( s.applySettings( goodSettings() ) );
        QVERIFY( s.startCapture( 4 ) );
        QVERIFY( s.waitReady( Timing::READY_TIMEOUT_MS ) );
        const Frame f = s.readFrame();
        QVERIFY( f.valid );
        QCOMPARE( f.channel[ 0 ].size(), size_t( 2044 ) );
        QCOMPARE( f.channel[ 1 ].size(), size_t( 2044 ) );
    }

    void read_channelsCutToCommonLength() {
        FakeClock clk;
        FakeDevice dev( clk );
        dev.nCh0 = 2044;
        dev.nCh1 = 1000;
        Session s( dev, clk );
        QVERIFY( s.open() );
        QVERIFY( s.applySettings( goodSettings() ) );
        QVERIFY( s.startCapture( 4 ) );
        QVERIFY( s.waitReady( Timing::READY_TIMEOUT_MS ) );
        const Frame f = s.readFrame();
        QCOMPARE( f.channel[ 0 ].size(), size_t( 1000 ) );
        QCOMPARE( f.channel[ 1 ].size(), size_t( 1000 ) );
    }

    void read_clipFlagIsPerChannel() {
        FakeClock clk;
        FakeDevice dev( clk );
        dev.outRange0 = 1;
        dev.outRange1 = 0;
        Session s( dev, clk );
        QVERIFY( s.open() );
        QVERIFY( s.applySettings( goodSettings() ) );
        QVERIFY( s.startCapture( 1 ) );
        QVERIFY( s.waitReady( Timing::READY_TIMEOUT_MS ) );
        const Frame f = s.readFrame();
        QVERIFY( f.clipped[ 0 ] );
        QVERIFY( !f.clipped[ 1 ] );
    }

    void read_returnsToIdleSoNextCaptureIsAllowed() {
        FakeClock clk;
        FakeDevice dev( clk );
        Session s( dev, clk );
        QVERIFY( s.open() );
        QVERIFY( s.applySettings( goodSettings() ) );
        QVERIFY( s.startCapture( 1 ) );
        QVERIFY( s.waitReady( Timing::READY_TIMEOUT_MS ) );
        QVERIFY( s.readFrame().valid );
        QCOMPARE( s.state(), Session::State::Idle );
        QVERIFY( s.startCapture( 1 ) );
    }

    void read_emptyReadIsNotAValidFrame() {
        FakeClock clk;
        FakeDevice dev( clk );
        dev.readReturnsZero = true;
        Session s( dev, clk );
        QVERIFY( s.open() );
        QVERIFY( s.applySettings( goodSettings() ) );
        QVERIFY( s.startCapture( 1 ) );
        QVERIFY( s.waitReady( Timing::READY_TIMEOUT_MS ) );
        // Прибор отдал ноль отсчётов: кадра нет, но и зависания нет —
        // состояние возвращается в Idle, следующий захват разрешён.
        QVERIFY( !s.readFrame().valid );
        QCOMPARE( s.state(), Session::State::Idle );
        QVERIFY( s.startCapture( 1 ) );
    }

    void capture_refusesZeroLength() {
        FakeClock clk;
        FakeDevice dev( clk );
        Session s( dev, clk );
        QVERIFY( s.open() );
        QVERIFY( s.applySettings( goodSettings() ) );
        QVERIFY( !s.startCapture( 0 ) );
        QVERIFY( !dev.called( "Capture" ) );
    }

    // --- охрана единицы данных ---

    void read_refusedWhenUnitIsNotEstablished() {
        FakeClock clk;
        FakeDevice dev( clk );
        // Шаг квантования, не сходящийся с пределом ни в каком порядке:
        // единица данных не установлена.
        dev.resolution = 1e-9;
        Session s( dev, clk );
        QVERIFY( s.open() );
        QVERIFY( s.applySettings( goodSettings() ) );
        QVERIFY( !s.unitsChecked() );
        QVERIFY( s.startCapture( 1 ) );
        QVERIFY( s.waitReady( Timing::READY_TIMEOUT_MS ) );
        const Frame f = s.readFrame();
        // Величина без установленной единицы в конвейер не идёт.
        QVERIFY( !f.valid );
        QVERIFY( !dev.called( "ReadVoltageDatas" ) );
    }

    // --- восстановление ---

    void recover_reappliesSettingsBecauseDeviceForgetsThem() {
        FakeClock clk;
        FakeDevice dev( clk );
        Session s( dev, clk );
        QVERIFY( s.open() );
        QVERIFY( s.applySettings( goodSettings() ) );
        const int before = dev.countOf( "SetOscChannelRange" );
        QVERIFY( s.recover() );
        // Прибор настроек не сохранил, а прочитать их у него нечем:
        // геттера предела в API нет вовсе.
        QCOMPARE( dev.countOf( "SetOscChannelRange" ), before + 2 );
        QCOMPARE( dev.countOf( "SetOscSample" ), 2 );
    }

    void recover_skipsTriggerForceWhenUnsupported() {
        FakeClock clk;
        FakeDevice dev( clk );
        dev.supportsTriggerForce = false; // ISDS205: аппаратного триггера нет
        Session s( dev, clk );
        QVERIFY( s.open() );
        QVERIFY( s.applySettings( goodSettings() ) );
        QVERIFY( s.recover() );
        QVERIFY( !dev.called( "TriggerForce" ) );
        // Первая ступень пропущена — начинается со сброса прибора.
        QVERIFY( dev.called( "ResetDevice" ) );
    }

    void recover_stopsAtTheFirstStageThatWorks() {
        FakeClock clk;
        FakeDevice dev( clk );
        dev.supportsTriggerForce = true;
        Session s( dev, clk );
        QVERIFY( s.open() );
        QVERIFY( s.applySettings( goodSettings() ) );
        QVERIFY( s.recover() );
        QVERIFY( dev.called( "TriggerForce" ) );
        // Полный перезапуск библиотеки стоит секунду; в цикле ему не место.
        QVERIFY( !dev.called( "ResetDevice" ) );
        QCOMPARE( dev.countOf( "FinishDll" ), 0 );
    }

    void recover_climbsToLibraryRestartWhenNothingElseHelps() {
        FakeClock clk;
        FakeDevice dev( clk );
        dev.supportsTriggerForce = true;
        Session s( dev, clk );
        QVERIFY( s.open() );
        QVERIFY( s.applySettings( goodSettings() ) );
        // Прибор оживает только после перезапуска библиотеки — это и
        // есть наблюдавшийся отказ: Capture принят, готовности нет.
        dev.availableAfterStage = 4;
        QVERIFY( s.recover() );
        QVERIFY( dev.called( "TriggerForce" ) );
        QVERIFY( dev.called( "ResetDevice" ) );
        QVERIFY( dev.called( "FinishDll" ) );
        QCOMPARE( dev.countOf( "InitDll" ), 2 );
    }

    // --- Генератор: то, чего на пути B нет вовсе ---

    void dds_capabilitiesAreAskedOnceAtConnect() {
        FakeClock clk;
        FakeDevice dev( clk );
        Session s( dev, clk );
        QVERIFY( s.open() );
        const Passport &p = s.passport();
        QVERIFY( p.hasDds );
        QCOMPARE( p.ddsBoxingMask, 0x1F );
        QVERIFY( p.ddsSoftZoomBias );
        // Формы спрашиваются ДВУМЯ вызовами: сперва количество с нулевым
        // указателем, потом заполнение. Один вызов — потерянная маска.
        QVERIFY( dev.called( "GetDDSSupportBoxingStyle:count" ) );
        QVERIFY( dev.called( "GetDDSSupportBoxingStyle:fill" ) );
    }

    void dds_notAskedOnDeviceWithoutGenerator() {
        FakeClock clk;
        FakeDevice dev( clk );
        dev.hasDdsDevice = false;
        Session s( dev, clk );
        QVERIFY( s.open() );
        QVERIFY( !s.passport().hasDds );
        // Спрашивать возможности генератора у прибора без генератора —
        // трафик ради заведомо пустого ответа.
        QVERIFY( !dev.called( "GetDDSSupportBoxingStyle:count" ) );
        DdsSettings d;
        d.frequencyHz = 1000;
        QVERIFY( !s.applyDds( d ) );
    }

    void dds_refusesShapeOutsideDeviceMask() {
        FakeClock clk;
        FakeDevice dev( clk );
        dev.ddsBoxingMask = 0x03; // только синус и меандр
        Session s( dev, clk );
        QVERIFY( s.open() );
        DdsSettings d;
        d.boxingStyle = 4; // пила вниз
        d.frequencyHz = 1000;
        QVERIFY( !s.applyDds( d ) );
        QVERIFY( !dev.called( "SetDDSBoxingStyle" ) );
        d.boxingStyle = 1;
        QVERIFY( s.applyDds( d ) );
    }

    void dds_zoomAndBiasVerifiedByReadingBack() {
        FakeClock clk;
        FakeDevice dev( clk );
        Session s( dev, clk );
        QVERIFY( s.open() );
        DdsSettings d;
        d.frequencyHz = 1000;
        d.zoomResistance = 42;
        d.biasResistance = 7;
        QVERIFY( s.applyDds( d ) );
        QVERIFY( dev.called( "GetDDSZoomResistance" ) );
        QVERIFY( dev.called( "GetDDSBiasResistance" ) );
        // Запись, которая не дошла, обязана быть замечена: возврата у
        // сеттера нет, чтение назад — единственная проверка.
        dev.ddsZoomSticks = false;
        d.zoomResistance = 99;
        QVERIFY( !s.applyDds( d ) );
    }

    void dds_zoomBiasRefusedWhereUnsupported() {
        FakeClock clk;
        FakeDevice dev( clk );
        dev.ddsSoftZoomBias = false;
        Session s( dev, clk );
        QVERIFY( s.open() );
        QVERIFY( !s.passport().ddsSoftZoomBias );
        DdsSettings d;
        d.frequencyHz = 1000;
        d.zoomResistance = 10;
        QVERIFY( !s.applyDds( d ) );
        QVERIFY( !dev.called( "SetDDSZoomResistance" ) );
    }

    void dds_outputVerifiedByReadingBack() {
        FakeClock clk;
        FakeDevice dev( clk );
        Session s( dev, clk );
        QVERIFY( s.open() );
        DdsSettings d;
        d.frequencyHz = 1000;
        d.output = true;
        QVERIFY( s.applyDds( d ) );
        QVERIFY( dev.called( "IsDDSOutputEnable" ) );
        dev.ddsOutputSticks = false;
        d.output = false;
        QVERIFY( !s.applyDds( d ) );
    }

    void dds_dutyOutsideRangeRefused() {
        FakeClock clk;
        FakeDevice dev( clk );
        Session s( dev, clk );
        QVERIFY( s.open() );
        DdsSettings d;
        d.frequencyHz = 1000;
        d.dutyPercent = 0;
        QVERIFY( !s.applyDds( d ) );
        d.dutyPercent = 100;
        QVERIFY( !s.applyDds( d ) );
        d.dutyPercent = 50;
        QVERIFY( s.applyDds( d ) );
    }

    // --- Цифровой ввод-вывод ---

    void io_numberAskedOnlyWhenSupported() {
        FakeClock clk;
        FakeDevice dev( clk );
        Session s( dev, clk );
        QVERIFY( s.open() );
        QVERIFY( !s.passport().hasIo );
        QVERIFY( !dev.called( "GetSupportIoNumber" ) );

        FakeClock clk2;
        FakeDevice dev2( clk2 );
        dev2.ioSupported = true;
        dev2.ioNumber = 4;
        Session s2( dev2, clk2 );
        QVERIFY( s2.open() );
        QCOMPARE( s2.passport().ioChannels, 4 );
    }

    void io_stateFailureIsMinusOneNotByte() {
        // GetIOState объявлен как char и отдаёт −1 при неуспехе: приём в
        // unsigned char превратил бы отказ в значение 255.
        FakeClock clk;
        FakeDevice dev( clk );
        const char v = dev.GetIOState( 0 );
        QCOMPARE( int( v ), -1 );
        QVERIFY( int( v ) < 0 );
    }

    // --- Полнота набора команд ---

    void api_coversTheWholeVendorSet() {
        // Поддельный прибор реализует IVdso::Api целиком — иначе класс
        // абстрактен и тест не соберётся. Это машинная проверка того, что
        // интерфейс покрывает набор вендора: 73 экспорта.
        FakeClock clk;
        FakeDevice dev( clk );
        Api &api = dev;
        QCOMPARE( api.IsSupportAcDc(), 1 ); // на 205B: НЕ поддерживается
        QVERIFY( !acDcSupported( api.IsSupportAcDc() ) );
    }

    void recover_countsItsOwnFailures() {
        FakeClock clk;
        FakeDevice dev( clk );
        Session s( dev, clk );
        QVERIFY( s.open() );
        QVERIFY( s.applySettings( goodSettings() ) );
        dev.availResult = 0;
        dev.availableAfterStage = -1;
        QVERIFY( !s.recover() );
        QCOMPARE( s.failedRecoveries(), 1 );
        QVERIFY( !s.recover() );
        QCOMPARE( s.failedRecoveries(), 2 );
    }
};

QTEST_MAIN( TestIVdsoSession )
#include "Itest_ivdsosession.moc"
