// SPDX-License-Identifier: GPL-3.0-or-later
// Last edited: 2026-09-04 UTC
//
// Путь A: загрузка `vdso.dll` и связывание ВСЕХ 73 экспортов.
//
// Единственный файл дерева, который знает про Windows. Всё остальное
// работает через IVdso::Api и проверяется без прибора.
//
// Почему загрузка не сводится к LoadLibrary("vdso.dll"):
// библиотека тянет за собой MSVCP140.dll, VCRUNTIME140.dll,
// VCRUNTIME140_1.dll, WINUSB.DLL, SETUPAPI.dll. Первые три — рантайм
// MSVC, которого в окружении MinGW64 нет. Если они лежат рядом с
// vdso.dll, каталог обязан быть в путях поиска, иначе LoadLibrary
// возвращает 126 «модуль не найден» — и это выглядит как отсутствие
// самой vdso.dll, хотя не найдена её зависимость.
//
// Разрядность: vdso.dll — PE32+ x86-64. 32-битная сборка её не
// загрузит; проверка разрядности процесса стоит в load().
//
// **Связывание всё или ничего.** Если хоть одного из 73 имён нет,
// load() отказывает и называет недостающие. Библиотека с неполным
// набором — не та библиотека, и выяснять это посреди захвата поздно.

#pragma once

#include "ivdsoapi.h"

#include <string>
#include <vector>

namespace IVdso {

class Loader : public Api {
  public:
    Loader() = default;
    ~Loader() override;

    Loader( const Loader & ) = delete;
    Loader &operator=( const Loader & ) = delete;

    /// Загружает библиотеку. `path` — полный путь к файлу; каталог из
    /// него добавляется в пути поиска ДО загрузки, ради зависимостей.
    /// Пустой путь — искать по обычным правилам системы.
    bool load( const std::string &path );
    void unload();
    bool loaded() const { return m_handle != nullptr; }
    const std::string &lastError() const { return m_lastError; }

    /// Сколько имён связано (0, пока не загружено). Ожидается 73.
    int boundSymbols() const { return m_bound; }

    /// Имена файла, под которыми библиотека встречается: вендорское
    /// демо грузит "VDSO.dll", в репозитории лежит "vdso.dll". NTFS
    /// регистр не различает, а подстановка имени в настройки —
    /// различает, поэтому искать надо оба.
    static std::vector< std::string > fileNameCandidates();

    // --- IVdso::Api: все 73 экспорта ---
    int InitDll() override;
    int FinishDll() override;
    unsigned int GetOnlyId0() override;
    unsigned int GetOnlyId1() override;
    int ResetDevice() override;
    void SetDevNoticeCallBack( void* ppara, AddCallBack addcallback, RemoveCallBack rmvcallback ) override;
    void SetDevNoticeEvent( void * addevent, void * rmvevent ) override;
    int IsDevAvailable() override;
    int SetOscChannelRange( int channel, int minmv, int maxmv ) override;
    int GetOscSupportSampleNum() override;
    int GetOscSupportSamples( unsigned int* sample, int maxnum ) override;
    unsigned int GetOscSample() override;
    unsigned int SetOscSample( unsigned int sample ) override;
    int IsSupportHardTrigger() override;
    unsigned int GetTriggerMode() override;
    void SetTriggerMode( unsigned int mode ) override;
    unsigned int GetTriggerStyle() override;
    void SetTriggerStyle( unsigned int style ) override;
    int GetTriggerPulseWidthNsMin() override;
    int GetTriggerPulseWidthNsMax() override;
    int GetTriggerPulseWidthDownNs() override;
    int GetTriggerPulseWidthUpNs() override;
    void SetTriggerPulseWidthNs( int down_ns, int up_ns ) override;
    unsigned int GetTriggerSource() override;
    void SetTriggerSource( unsigned int source ) override;
    int GetTriggerLevel() override;
    void SetTriggerLevel( int level ) override;
    int IsSupportTriggerSense() override;
    double GetTriggerSenseDiv() override;
    void SetTriggerSenseDiv( double sense ) override;
    bool IsSupportPreTriggerPercent() override;
    int GetPreTriggerPercent() override;
    void SetPreTriggerPercent( int front ) override;
    int IsSupportTriggerForce() override;
    void TriggerForce() override;
    int IsSupportAcDc() override;
    void SetAcDc( unsigned int chn, int ac ) override;
    int GetAcDc( unsigned int chn ) override;
    int IsSupportRollMode() override;
    int SetRollMode( unsigned int en ) override;
    unsigned int GetMemoryLength() override;
    int Capture( int length, char force_length ) override;
    void SetDataReadyCallBack( void* ppara, DataReadyCallBack datacallback ) override;
    void SetDataReadyEvent( void * dataevent ) override;
    int IsDataReady() override;
    unsigned int ReadVoltageDatas( char channel, double* buffer, unsigned int length ) override;
    int IsVoltageDatasOutRange( char channel ) override;
    double GetVoltageResolution( char channel ) override;
    int IsSupportDDSDevice() override;
    int GetDDSSupportBoxingStyle( int* style ) override;
    void SetDDSBoxingStyle( unsigned int boxing ) override;
    void SetDDSPinlv( unsigned int pinlv ) override;
    void SetDDSDutyCycle( int cycle ) override;
    void DDSOutputEnable( int enable ) override;
    int IsDDSOutputEnable() override;
    int IsDDSSupportSoftwareControlZoomBias() override;
    int GetDDSBiasResistanceRangeMin() override;
    int GetDDSBiasResistanceRangeMax() override;
    void SetDDSBiasResistance( int Resistance ) override;
    int GetDDSBiasResistance() override;
    int GetDDSZoomResistanceRangeMin() override;
    int GetDDSZoomResistanceRangeMax() override;
    void SetDDSZoomResistance( int Resistance ) override;
    int GetDDSZoomResistance() override;
    int IsSupportIODevice() override;
    int GetSupportIoNumber() override;
    void SetIOReadStateCallBack( void* ppara, IOReadStateCallBack callback ) override;
    void SetIOReadStateReadyEvent( void * dataevent ) override;
    int IsIOReadStateReady() override;
    void SetIOInOut( unsigned char channel, unsigned char inout ) override;
    void SetIOState( unsigned char channel, unsigned char state ) override;
    void ReadIOState( unsigned char channel ) override;
    char GetIOState( unsigned char channel ) override;
  private:
    void *m_handle = nullptr;
    std::string m_lastError;
    int m_bound = 0;

    struct Table;
    Table *m_fn = nullptr;
};

} // namespace IVdso
