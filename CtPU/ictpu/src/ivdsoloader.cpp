// SPDX-License-Identifier: GPL-3.0-or-later
// Last edited: 2026-09-04 UTC

#include "ivdsoloader.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace IVdso {

// WINAPI у вендора — это __stdcall. На x64 соглашение единственное, и
// атрибут пуст; выписан он ради 32-битной сборки, где значим, и ради
// того, чтобы объявление совпадало с заголовком вендора дословно.
#ifdef _WIN32
#define IVDSO_CALL __stdcall
#else
#define IVDSO_CALL
#endif

// Экспорты недекорированы (библиотека собрана как extern "C"), поэтому
// GetProcAddress идёт по чистому имени. Список порождён из заголовка
// вендора V20191217 и сверен с каталогом экспорта самой библиотеки:
// 73 имени, ноль расхождений.
#define IVDSO_SYMBOLS( X ) \
    X( int, InitDll, () ) \
    X( int, FinishDll, () ) \
    X( unsigned int, GetOnlyId0, () ) \
    X( unsigned int, GetOnlyId1, () ) \
    X( int, ResetDevice, () ) \
    X( void, SetDevNoticeCallBack, ( void*, AddCallBack, RemoveCallBack ) ) \
    X( void, SetDevNoticeEvent, ( void *, void * ) ) \
    X( int, IsDevAvailable, () ) \
    X( int, SetOscChannelRange, ( int, int, int ) ) \
    X( int, GetOscSupportSampleNum, () ) \
    X( int, GetOscSupportSamples, ( unsigned int*, int ) ) \
    X( unsigned int, GetOscSample, () ) \
    X( unsigned int, SetOscSample, ( unsigned int ) ) \
    X( int, IsSupportHardTrigger, () ) \
    X( unsigned int, GetTriggerMode, () ) \
    X( void, SetTriggerMode, ( unsigned int ) ) \
    X( unsigned int, GetTriggerStyle, () ) \
    X( void, SetTriggerStyle, ( unsigned int ) ) \
    X( int, GetTriggerPulseWidthNsMin, () ) \
    X( int, GetTriggerPulseWidthNsMax, () ) \
    X( int, GetTriggerPulseWidthDownNs, () ) \
    X( int, GetTriggerPulseWidthUpNs, () ) \
    X( void, SetTriggerPulseWidthNs, ( int, int ) ) \
    X( unsigned int, GetTriggerSource, () ) \
    X( void, SetTriggerSource, ( unsigned int ) ) \
    X( int, GetTriggerLevel, () ) \
    X( void, SetTriggerLevel, ( int ) ) \
    X( int, IsSupportTriggerSense, () ) \
    X( double, GetTriggerSenseDiv, () ) \
    X( void, SetTriggerSenseDiv, ( double ) ) \
    X( bool, IsSupportPreTriggerPercent, () ) \
    X( int, GetPreTriggerPercent, () ) \
    X( void, SetPreTriggerPercent, ( int ) ) \
    X( int, IsSupportTriggerForce, () ) \
    X( void, TriggerForce, () ) \
    X( int, IsSupportAcDc, () ) \
    X( void, SetAcDc, ( unsigned int, int ) ) \
    X( int, GetAcDc, ( unsigned int ) ) \
    X( int, IsSupportRollMode, () ) \
    X( int, SetRollMode, ( unsigned int ) ) \
    X( unsigned int, GetMemoryLength, () ) \
    X( int, Capture, ( int, char ) ) \
    X( void, SetDataReadyCallBack, ( void*, DataReadyCallBack ) ) \
    X( void, SetDataReadyEvent, ( void * ) ) \
    X( int, IsDataReady, () ) \
    X( unsigned int, ReadVoltageDatas, ( char, double*, unsigned int ) ) \
    X( int, IsVoltageDatasOutRange, ( char ) ) \
    X( double, GetVoltageResolution, ( char ) ) \
    X( int, IsSupportDDSDevice, () ) \
    X( int, GetDDSSupportBoxingStyle, ( int* ) ) \
    X( void, SetDDSBoxingStyle, ( unsigned int ) ) \
    X( void, SetDDSPinlv, ( unsigned int ) ) \
    X( void, SetDDSDutyCycle, ( int ) ) \
    X( void, DDSOutputEnable, ( int ) ) \
    X( int, IsDDSOutputEnable, () ) \
    X( int, IsDDSSupportSoftwareControlZoomBias, () ) \
    X( int, GetDDSBiasResistanceRangeMin, () ) \
    X( int, GetDDSBiasResistanceRangeMax, () ) \
    X( void, SetDDSBiasResistance, ( int ) ) \
    X( int, GetDDSBiasResistance, () ) \
    X( int, GetDDSZoomResistanceRangeMin, () ) \
    X( int, GetDDSZoomResistanceRangeMax, () ) \
    X( void, SetDDSZoomResistance, ( int ) ) \
    X( int, GetDDSZoomResistance, () ) \
    X( int, IsSupportIODevice, () ) \
    X( int, GetSupportIoNumber, () ) \
    X( void, SetIOReadStateCallBack, ( void*, IOReadStateCallBack ) ) \
    X( void, SetIOReadStateReadyEvent, ( void * ) ) \
    X( int, IsIOReadStateReady, () ) \
    X( void, SetIOInOut, ( unsigned char, unsigned char ) ) \
    X( void, SetIOState, ( unsigned char, unsigned char ) ) \
    X( void, ReadIOState, ( unsigned char ) ) \
    X( char, GetIOState, ( unsigned char ) )

struct Loader::Table {
#define IVDSO_DECL( ret, name, args ) ret( IVDSO_CALL *name ) args = nullptr;
    IVDSO_SYMBOLS( IVDSO_DECL )
#undef IVDSO_DECL
};

std::vector< std::string > Loader::fileNameCandidates() { return { "VDSO.dll", "vdso.dll" }; }

Loader::~Loader() { unload(); }

#ifdef _WIN32

static std::wstring widen( const std::string &s ) {
    if ( s.empty() )
        return std::wstring();
    const int n = MultiByteToWideChar( CP_UTF8, 0, s.c_str(), int( s.size() ), nullptr, 0 );
    std::wstring w( size_t( n ), L'\0' );
    MultiByteToWideChar( CP_UTF8, 0, s.c_str(), int( s.size() ), &w[ 0 ], n );
    return w;
}

bool Loader::load( const std::string &path ) {
    unload();
    m_lastError.clear();

    if ( sizeof( void * ) != 8 ) {
        m_lastError = "vdso.dll — x86-64; 32-битная сборка её не загрузит";
        return false;
    }

    // Путь приводится к АБСОЛЮТНОМУ до всего остального.
    //
    // `LoadLibraryExW` с флагами `LOAD_LIBRARY_SEARCH_*` относительного пути
    // не принимает: он отвечает кодом 87 (ERROR_INVALID_PARAMETER), и это
    // выглядит как «библиотека негодная», хотя негоден вызов. То же с
    // `AddDllDirectory` — ему нужен абсолютный каталог. Дефект найден
    // дымовой проверкой на сборочной машине (прогон 188), а не автором.
    std::wstring wpath = widen( path );
    {
        DWORD need = GetFullPathNameW( wpath.c_str(), 0, nullptr, nullptr );
        if ( need ) {
            std::wstring full( size_t( need ), L'\0' );
            const DWORD got = GetFullPathNameW( wpath.c_str(), need, &full[ 0 ], nullptr );
            if ( got && got < need ) {
                full.resize( size_t( got ) );
                wpath.swap( full );
            }
        }
    }

    // Каталог библиотеки в пути поиска ДО загрузки: иначе не найдётся
    // не она сама, а её рантайм MSVC, и ошибка будет неотличима.
    const size_t slash = wpath.find_last_of( L"\\/" );
    if ( slash != std::wstring::npos ) {
        SetDefaultDllDirectories( LOAD_LIBRARY_SEARCH_DEFAULT_DIRS );
        AddDllDirectory( wpath.substr( 0, slash ).c_str() );
    }

    HMODULE h = LoadLibraryExW( wpath.c_str(), nullptr,
                                LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR );
    if ( !h ) {
        const DWORD e = GetLastError();
        m_lastError = "LoadLibrary отказал, код " + std::to_string( (unsigned long) e );
        if ( e == 126 )
            m_lastError += " (не найдена сама библиотека ИЛИ её зависимость: "
                           "MSVCP140.dll, VCRUNTIME140.dll, VCRUNTIME140_1.dll)";
        else if ( e == 87 )
            m_lastError += " (неверный параметр вызова: с флагами поиска путь обязан быть "
                           "абсолютным)";
        return false;
    }

    Table *t = new Table();
    std::string missing;
    int bound = 0;
#define IVDSO_BIND( ret, name, args )                                                              \
    t->name = reinterpret_cast< ret( IVDSO_CALL * ) args >(                                        \
        reinterpret_cast< void * >( GetProcAddress( h, #name ) ) );                                \
    if ( t->name ) {                                                                               \
        ++bound;                                                                                   \
    } else {                                                                                       \
        if ( !missing.empty() )                                                                    \
            missing += ", ";                                                                       \
        missing += #name;                                                                          \
    }
    IVDSO_SYMBOLS( IVDSO_BIND )
#undef IVDSO_BIND

    if ( !missing.empty() ) {
        delete t;
        FreeLibrary( h );
        m_lastError = "библиотека загрузилась, но экспортов нет: " + missing;
        return false;
    }

    m_handle = (void *) h;
    m_fn = t;
    m_bound = bound;
    return true;
}

void Loader::unload() {
    delete m_fn;
    m_fn = nullptr;
    m_bound = 0;
    if ( m_handle ) {
        FreeLibrary( (HMODULE) m_handle );
        m_handle = nullptr;
    }
}

#else // не Windows

bool Loader::load( const std::string & ) {
    // Путь A существует только под Windows: прибор там работает под
    // вендорским драйвером, привязанным собственным Dso.inf. Отказ
    // здесь — не заглушка на будущее, а факт: на других системах этого
    // стека нет. Сборка под Linux нужна лишь затем, чтобы проверять
    // тестами всё, что выше загрузчика.
    m_lastError = "путь A доступен только под Windows";
    return false;
}

void Loader::unload() {
    delete m_fn;
    m_fn = nullptr;
    m_bound = 0;
    m_handle = nullptr;
}

#endif

// Проброс. Незагруженная библиотека отвечает отказом по конвенции
// каждой функции, а не падает: состояние «не загрузилась» проходит по
// тем же путям, что и «прибор отказал». У IsSupportAcDc полярность
// обратная, поэтому её отказ — единица, а не ноль; у GetIOState отказ
// объявлен вендором как −1.
#define IVDSO_GUARD( fail )                                                                        \
    if ( !m_fn )                                                                                   \
    return fail

int Loader::InitDll() {
    IVDSO_GUARD( 0 );
    return m_fn->InitDll();
}
int Loader::FinishDll() {
    IVDSO_GUARD( 0 );
    return m_fn->FinishDll();
}
unsigned int Loader::GetOnlyId0() {
    IVDSO_GUARD( 0 );
    return m_fn->GetOnlyId0();
}
unsigned int Loader::GetOnlyId1() {
    IVDSO_GUARD( 0 );
    return m_fn->GetOnlyId1();
}
int Loader::ResetDevice() {
    IVDSO_GUARD( 0 );
    return m_fn->ResetDevice();
}
void Loader::SetDevNoticeCallBack( void* ppara, AddCallBack addcallback, RemoveCallBack rmvcallback ) {
    IVDSO_GUARD();
    m_fn->SetDevNoticeCallBack( ppara, addcallback, rmvcallback );
}
void Loader::SetDevNoticeEvent( void * addevent, void * rmvevent ) {
    IVDSO_GUARD();
    m_fn->SetDevNoticeEvent( addevent, rmvevent );
}
int Loader::IsDevAvailable() {
    IVDSO_GUARD( 0 );
    return m_fn->IsDevAvailable();
}
int Loader::SetOscChannelRange( int channel, int minmv, int maxmv ) {
    IVDSO_GUARD( 0 );
    return m_fn->SetOscChannelRange( channel, minmv, maxmv );
}
int Loader::GetOscSupportSampleNum() {
    IVDSO_GUARD( 0 );
    return m_fn->GetOscSupportSampleNum();
}
int Loader::GetOscSupportSamples( unsigned int* sample, int maxnum ) {
    IVDSO_GUARD( 0 );
    return m_fn->GetOscSupportSamples( sample, maxnum );
}
unsigned int Loader::GetOscSample() {
    IVDSO_GUARD( 0 );
    return m_fn->GetOscSample();
}
unsigned int Loader::SetOscSample( unsigned int sample ) {
    IVDSO_GUARD( 0 );
    return m_fn->SetOscSample( sample );
}
int Loader::IsSupportHardTrigger() {
    IVDSO_GUARD( 0 );
    return m_fn->IsSupportHardTrigger();
}
unsigned int Loader::GetTriggerMode() {
    IVDSO_GUARD( 0 );
    return m_fn->GetTriggerMode();
}
void Loader::SetTriggerMode( unsigned int mode ) {
    IVDSO_GUARD();
    m_fn->SetTriggerMode( mode );
}
unsigned int Loader::GetTriggerStyle() {
    IVDSO_GUARD( 0 );
    return m_fn->GetTriggerStyle();
}
void Loader::SetTriggerStyle( unsigned int style ) {
    IVDSO_GUARD();
    m_fn->SetTriggerStyle( style );
}
int Loader::GetTriggerPulseWidthNsMin() {
    IVDSO_GUARD( 0 );
    return m_fn->GetTriggerPulseWidthNsMin();
}
int Loader::GetTriggerPulseWidthNsMax() {
    IVDSO_GUARD( 0 );
    return m_fn->GetTriggerPulseWidthNsMax();
}
int Loader::GetTriggerPulseWidthDownNs() {
    IVDSO_GUARD( 0 );
    return m_fn->GetTriggerPulseWidthDownNs();
}
int Loader::GetTriggerPulseWidthUpNs() {
    IVDSO_GUARD( 0 );
    return m_fn->GetTriggerPulseWidthUpNs();
}
void Loader::SetTriggerPulseWidthNs( int down_ns, int up_ns ) {
    IVDSO_GUARD();
    m_fn->SetTriggerPulseWidthNs( down_ns, up_ns );
}
unsigned int Loader::GetTriggerSource() {
    IVDSO_GUARD( 0 );
    return m_fn->GetTriggerSource();
}
void Loader::SetTriggerSource( unsigned int source ) {
    IVDSO_GUARD();
    m_fn->SetTriggerSource( source );
}
int Loader::GetTriggerLevel() {
    IVDSO_GUARD( 0 );
    return m_fn->GetTriggerLevel();
}
void Loader::SetTriggerLevel( int level ) {
    IVDSO_GUARD();
    m_fn->SetTriggerLevel( level );
}
int Loader::IsSupportTriggerSense() {
    IVDSO_GUARD( 0 );
    return m_fn->IsSupportTriggerSense();
}
double Loader::GetTriggerSenseDiv() {
    IVDSO_GUARD( 0.0 );
    return m_fn->GetTriggerSenseDiv();
}
void Loader::SetTriggerSenseDiv( double sense ) {
    IVDSO_GUARD();
    m_fn->SetTriggerSenseDiv( sense );
}
bool Loader::IsSupportPreTriggerPercent() {
    IVDSO_GUARD( false );
    return m_fn->IsSupportPreTriggerPercent();
}
int Loader::GetPreTriggerPercent() {
    IVDSO_GUARD( 0 );
    return m_fn->GetPreTriggerPercent();
}
void Loader::SetPreTriggerPercent( int front ) {
    IVDSO_GUARD();
    m_fn->SetPreTriggerPercent( front );
}
int Loader::IsSupportTriggerForce() {
    IVDSO_GUARD( 0 );
    return m_fn->IsSupportTriggerForce();
}
void Loader::TriggerForce() {
    IVDSO_GUARD();
    m_fn->TriggerForce();
}
int Loader::IsSupportAcDc() {
    IVDSO_GUARD( 1 );
    return m_fn->IsSupportAcDc();
}
void Loader::SetAcDc( unsigned int chn, int ac ) {
    IVDSO_GUARD();
    m_fn->SetAcDc( chn, ac );
}
int Loader::GetAcDc( unsigned int chn ) {
    IVDSO_GUARD( 0 );
    return m_fn->GetAcDc( chn );
}
int Loader::IsSupportRollMode() {
    IVDSO_GUARD( 0 );
    return m_fn->IsSupportRollMode();
}
int Loader::SetRollMode( unsigned int en ) {
    IVDSO_GUARD( 0 );
    return m_fn->SetRollMode( en );
}
unsigned int Loader::GetMemoryLength() {
    IVDSO_GUARD( 0 );
    return m_fn->GetMemoryLength();
}
int Loader::Capture( int length, char force_length ) {
    IVDSO_GUARD( 0 );
    return m_fn->Capture( length, force_length );
}
void Loader::SetDataReadyCallBack( void* ppara, DataReadyCallBack datacallback ) {
    IVDSO_GUARD();
    m_fn->SetDataReadyCallBack( ppara, datacallback );
}
void Loader::SetDataReadyEvent( void * dataevent ) {
    IVDSO_GUARD();
    m_fn->SetDataReadyEvent( dataevent );
}
int Loader::IsDataReady() {
    IVDSO_GUARD( 0 );
    return m_fn->IsDataReady();
}
unsigned int Loader::ReadVoltageDatas( char channel, double* buffer, unsigned int length ) {
    IVDSO_GUARD( 0 );
    return m_fn->ReadVoltageDatas( channel, buffer, length );
}
int Loader::IsVoltageDatasOutRange( char channel ) {
    IVDSO_GUARD( 0 );
    return m_fn->IsVoltageDatasOutRange( channel );
}
double Loader::GetVoltageResolution( char channel ) {
    IVDSO_GUARD( 0.0 );
    return m_fn->GetVoltageResolution( channel );
}
int Loader::IsSupportDDSDevice() {
    IVDSO_GUARD( 0 );
    return m_fn->IsSupportDDSDevice();
}
int Loader::GetDDSSupportBoxingStyle( int* style ) {
    IVDSO_GUARD( 0 );
    return m_fn->GetDDSSupportBoxingStyle( style );
}
void Loader::SetDDSBoxingStyle( unsigned int boxing ) {
    IVDSO_GUARD();
    m_fn->SetDDSBoxingStyle( boxing );
}
void Loader::SetDDSPinlv( unsigned int pinlv ) {
    IVDSO_GUARD();
    m_fn->SetDDSPinlv( pinlv );
}
void Loader::SetDDSDutyCycle( int cycle ) {
    IVDSO_GUARD();
    m_fn->SetDDSDutyCycle( cycle );
}
void Loader::DDSOutputEnable( int enable ) {
    IVDSO_GUARD();
    m_fn->DDSOutputEnable( enable );
}
int Loader::IsDDSOutputEnable() {
    IVDSO_GUARD( 0 );
    return m_fn->IsDDSOutputEnable();
}
int Loader::IsDDSSupportSoftwareControlZoomBias() {
    IVDSO_GUARD( 0 );
    return m_fn->IsDDSSupportSoftwareControlZoomBias();
}
int Loader::GetDDSBiasResistanceRangeMin() {
    IVDSO_GUARD( 0 );
    return m_fn->GetDDSBiasResistanceRangeMin();
}
int Loader::GetDDSBiasResistanceRangeMax() {
    IVDSO_GUARD( 0 );
    return m_fn->GetDDSBiasResistanceRangeMax();
}
void Loader::SetDDSBiasResistance( int Resistance ) {
    IVDSO_GUARD();
    m_fn->SetDDSBiasResistance( Resistance );
}
int Loader::GetDDSBiasResistance() {
    IVDSO_GUARD( 0 );
    return m_fn->GetDDSBiasResistance();
}
int Loader::GetDDSZoomResistanceRangeMin() {
    IVDSO_GUARD( 0 );
    return m_fn->GetDDSZoomResistanceRangeMin();
}
int Loader::GetDDSZoomResistanceRangeMax() {
    IVDSO_GUARD( 0 );
    return m_fn->GetDDSZoomResistanceRangeMax();
}
void Loader::SetDDSZoomResistance( int Resistance ) {
    IVDSO_GUARD();
    m_fn->SetDDSZoomResistance( Resistance );
}
int Loader::GetDDSZoomResistance() {
    IVDSO_GUARD( 0 );
    return m_fn->GetDDSZoomResistance();
}
int Loader::IsSupportIODevice() {
    IVDSO_GUARD( 0 );
    return m_fn->IsSupportIODevice();
}
int Loader::GetSupportIoNumber() {
    IVDSO_GUARD( 0 );
    return m_fn->GetSupportIoNumber();
}
void Loader::SetIOReadStateCallBack( void* ppara, IOReadStateCallBack callback ) {
    IVDSO_GUARD();
    m_fn->SetIOReadStateCallBack( ppara, callback );
}
void Loader::SetIOReadStateReadyEvent( void * dataevent ) {
    IVDSO_GUARD();
    m_fn->SetIOReadStateReadyEvent( dataevent );
}
int Loader::IsIOReadStateReady() {
    IVDSO_GUARD( 0 );
    return m_fn->IsIOReadStateReady();
}
void Loader::SetIOInOut( unsigned char channel, unsigned char inout ) {
    IVDSO_GUARD();
    m_fn->SetIOInOut( channel, inout );
}
void Loader::SetIOState( unsigned char channel, unsigned char state ) {
    IVDSO_GUARD();
    m_fn->SetIOState( channel, state );
}
void Loader::ReadIOState( unsigned char channel ) {
    IVDSO_GUARD();
    m_fn->ReadIOState( channel );
}
char Loader::GetIOState( unsigned char channel ) {
    IVDSO_GUARD( -1 );
    return m_fn->GetIOState( channel );
}
} // namespace IVdso
