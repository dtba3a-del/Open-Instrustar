// SPDX-License-Identifier: GPL-3.0-or-later
// Last edited: 2026-09-04 UTC
//
// Путь A, вендорский стек `vdso.dll`. ПОЛНЫЙ набор команд: все 73 экспорта.
//
// Распоряжение автора 2026-09-04: «XY.py работает по пути VDSO (на усечённом
// коммандсете vdso.h), однако ты прочитал полный список коммандсета vdso.dll —
// используй полный.» Прежняя редакция объявляла 25 функций «тех, которыми
// пользуется коннектор»; это был выбор исполнителя, и он снят.
//
// **Сколько их на самом деле — проверено, а не взято на слово.** Разбор
// каталога экспорта `references/vendor/instrustar-sdk/bin/vdso.dll`
// (PE32+ x86-64): **73 функции, ноль манглированных имён**, ровно те же, что
// объявлены в `VdsoLib.h` V20191217, и ровно те же, что в дампе `dumpbin`,
// снятом автором на целевой машине (`references/dll-exports/`).
//
// Отдельно о числе 126 из `VDSO-COMMANDSET.md`: столько C-символов экспортирует
// **Linux-сборка** `libvdso.so.1.0`. Разность в 71 символ — не скрытые команды
// прибора, а внутренности библиотеки ЦОС, вылезшие наружу из-за сборки без
// `-fvisibility=hidden`: `hanning`, `hamming`, `blackman`, `kaiser`, `cheb`,
// `dit2_fft`, `dit4_fft`, `dft_real`, `Fft_Amplitude`, `V_Conver_dBs`, `CAL_2M`
// и подобное. На Windows их нет и вызвать их оттуда нельзя. Прежняя запись
// «53 экспорта сверх заголовка — работающий интерфейс» этим опровергнута.
//
// **Имена — вендорские, буква в букву.** Своего написания здесь нет намеренно:
// второе имя у одной функции есть двоевластие (`docs/ДВОЕВЛАСТИЕ.md`), а сверка
// с заголовком вендора при собственных именах превращается в перевод.
//
// **Классы функций: Р — задание режима, Д — действие, З — запрос.**
// Поправка автора 2026-09-04: прежде здесь стояло «ловушка типов канала», и
// это было неверно по смыслу. Разные типы — следствие, а не суть. Суть в том,
// что функции принадлежат разным классам и одна НЕ переписывает другую:
// `SetOscChannelRange` задаёт чувствительность канала, `ReadVoltageDatas`
// читает из него данные, а номер канала в каждом случае означает своё —
// «какому задать» против «из какого читать». Смешение классов и даёт ту самую
// кашу на выходе; она и есть настоящая ловушка, а не тип аргумента.
//
//   Р — меняет состояние прибора и действует до следующей записи;
//   Д — однократный поступок, состояния не оставляет;
//   З — ничего не меняет: отвечает о приборе (свойство, спрашивается раз при
//       подключении) либо отдаёт результат (меняется каждый кадр).
//
// Отсюда практическое: приводить номер канала к одному типу нельзя не потому,
// что «так объявлено», а потому, что у `unsigned char` в подсистеме
// ввода-вывода это номер ЦИФРОВОЙ ЛИНИИ, а не аналогового канала. Одинаковое
// слово, разные предметы. И у `char` это значения 0 и 1, а не символы '0'/'1'.
// Разбор — `docs/ОТВЕТЫ-2026-09-05.md` §1.
//
// Разбор: `docs/INSTRUSTAR-CONNECTOR.md`.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace IVdso {

/// Полярность ответа IsSupportAcDc у вендора обратная остальному
/// семейству Is*: 0 = поддерживается, 1 = НЕ поддерживается.
/// Дословно из VdsoLib.h: "Return value 0 :support AC/DC switch,
/// 1 :not support". Единственная функция с такой полярностью,
/// поэтому она вынесена в именованную свободную функцию, а не
/// повторяется по месту вызова.
bool acDcSupported( int isSupportAcDcReturn );

// Обработчики вендора. Вызываются ИЗ ЧУЖОГО ПОТОКА библиотеки: в Qt-программе
// из них нельзя трогать виджеты, только ставить очередь.
extern "C" {
typedef void ( *AddCallBack )( void *ppara );
typedef void ( *RemoveCallBack )( void *ppara );
typedef void ( *DataReadyCallBack )( void *ppara );
// Три аргумента, а не один: дословно из VdsoLib.h V20191217 —
// typedef void (CALLBACK* IOReadStateCallBack)(void* ppara, unsigned char channel, char state);
// Комментарий вендора над этим typedef обещает два аргумента без ppara и
// с самим объявлением расходится; библиотека собрана по объявлению.
typedef void ( *IOReadStateCallBack )( void *ppara, unsigned char channel, char state );
}

/// Интерфейс библиотеки пути A: ВСЕ 73 экспорта, в порядке заголовка вендора.
///
/// Событийные функции берут `void *` там, где у вендора `HANDLE`: на Windows
/// это и есть `void *`, а интерфейс остаётся переносимым, и тесты не тянут
/// за собой `windows.h`.
class Api {
  public:
    virtual ~Api() = default;

    // --- Жизненный цикл библиотеки. Класс Д ---
    virtual int InitDll() = 0;
    virtual int FinishDll() = 0;

    // --- Прибор: номер и сброс. GetOnlyId — З (свойство), ResetDevice — Д ---
    virtual unsigned int GetOnlyId0() = 0;
    virtual unsigned int GetOnlyId1() = 0;
    virtual int ResetDevice() = 0;

    // --- Состояние USB. Set*CallBack/Event — Р, IsDevAvailable — З ---
    virtual void SetDevNoticeCallBack( void* ppara, AddCallBack addcallback, RemoveCallBack rmvcallback ) = 0;
    virtual void SetDevNoticeEvent( void * addevent, void * rmvevent ) = 0;
    virtual int IsDevAvailable() = 0;

    // --- Предел канала: Р, «какому каналу задать чувствительность». МИЛЛИВОЛЬТЫ, интервал, не «В/дел» ---
    virtual int SetOscChannelRange( int channel, int minmv, int maxmv ) = 0;

    // --- Скорость выборки. GetOscSupportSample* — З (свойство), SetOscSample — Р ---
    virtual int GetOscSupportSampleNum() = 0;
    virtual int GetOscSupportSamples( unsigned int* sample, int maxnum ) = 0;
    virtual unsigned int GetOscSample() = 0;
    virtual unsigned int SetOscSample( unsigned int sample ) = 0;

    // --- Триггер. IsSupport*/Get* — З, Set* — Р, TriggerForce — Д. На ISDS205 аппаратного нет ---
    virtual int IsSupportHardTrigger() = 0;
    virtual unsigned int GetTriggerMode() = 0;
    virtual void SetTriggerMode( unsigned int mode ) = 0;
    virtual unsigned int GetTriggerStyle() = 0;
    virtual void SetTriggerStyle( unsigned int style ) = 0;
    virtual int GetTriggerPulseWidthNsMin() = 0;
    virtual int GetTriggerPulseWidthNsMax() = 0;
    virtual int GetTriggerPulseWidthDownNs() = 0;
    virtual int GetTriggerPulseWidthUpNs() = 0;
    virtual void SetTriggerPulseWidthNs( int down_ns, int up_ns ) = 0;
    virtual unsigned int GetTriggerSource() = 0;
    virtual void SetTriggerSource( unsigned int source ) = 0;
    virtual int GetTriggerLevel() = 0;
    virtual void SetTriggerLevel( int level ) = 0;
    virtual int IsSupportTriggerSense() = 0;
    virtual double GetTriggerSenseDiv() = 0;
    virtual void SetTriggerSenseDiv( double sense ) = 0;
    virtual bool IsSupportPreTriggerPercent() = 0;
    virtual int GetPreTriggerPercent() = 0;
    virtual void SetPreTriggerPercent( int front ) = 0;
    virtual int IsSupportTriggerForce() = 0;
    virtual void TriggerForce() = 0;

    // --- Связь AC/DC: SetAcDc — Р, GetAcDc — З. ВНИМАНИЕ: у IsSupportAcDc ОБРАТНАЯ полярность ---
    virtual int IsSupportAcDc() = 0;
    virtual void SetAcDc( unsigned int chn, int ac ) = 0;
    virtual int GetAcDc( unsigned int chn ) = 0;

    // --- Roll Mode: IsSupport — З, SetRollMode — Р ---
    virtual int IsSupportRollMode() = 0;
    virtual int SetRollMode( unsigned int en ) = 0;

    // --- Память — З (свойство); Capture — Д. КИЛОБАЙТЫ, не отсчёты ---
    virtual unsigned int GetMemoryLength() = 0;
    virtual int Capture( int length, char force_length ) = 0;

    // --- Готовность кадра: Set*CallBack/Event — Р, IsDataReady — З (результат) ---
    virtual void SetDataReadyCallBack( void* ppara, DataReadyCallBack datacallback ) = 0;
    virtual void SetDataReadyEvent( void * dataevent ) = 0;
    virtual int IsDataReady() = 0;

    // --- Чтение данных: всё З (результат), «из какого канала читать» ---
    virtual unsigned int ReadVoltageDatas( char channel, double* buffer, unsigned int length ) = 0;
    virtual int IsVoltageDatasOutRange( char channel ) = 0;
    virtual double GetVoltageResolution( char channel ) = 0;

    // --- DDS: Is*/Get* — З, Set*/DDSOutputEnable — Р. Пути B этого не даёт вовсе ---
    virtual int IsSupportDDSDevice() = 0;
    virtual int GetDDSSupportBoxingStyle( int* style ) = 0;
    virtual void SetDDSBoxingStyle( unsigned int boxing ) = 0;
    virtual void SetDDSPinlv( unsigned int pinlv ) = 0;
    virtual void SetDDSDutyCycle( int cycle ) = 0;
    virtual void DDSOutputEnable( int enable ) = 0;
    virtual int IsDDSOutputEnable() = 0;
    virtual int IsDDSSupportSoftwareControlZoomBias() = 0;
    virtual int GetDDSBiasResistanceRangeMin() = 0;
    virtual int GetDDSBiasResistanceRangeMax() = 0;
    virtual void SetDDSBiasResistance( int Resistance ) = 0;
    virtual int GetDDSBiasResistance() = 0;
    virtual int GetDDSZoomResistanceRangeMin() = 0;
    virtual int GetDDSZoomResistanceRangeMax() = 0;
    virtual void SetDDSZoomResistance( int Resistance ) = 0;
    virtual int GetDDSZoomResistance() = 0;

    // --- Цифровой ввод-вывод: номер здесь — ЦИФРОВАЯ ЛИНИЯ, не канал АЦП. Пути B этого не даёт вовсе ---
    virtual int IsSupportIODevice() = 0;
    virtual int GetSupportIoNumber() = 0;
    virtual void SetIOReadStateCallBack( void* ppara, IOReadStateCallBack callback ) = 0;
    virtual void SetIOReadStateReadyEvent( void * dataevent ) = 0;
    virtual int IsIOReadStateReady() = 0;
    virtual void SetIOInOut( unsigned char channel, unsigned char inout ) = 0;
    virtual void SetIOState( unsigned char channel, unsigned char state ) = 0;
    virtual void ReadIOState( unsigned char channel ) = 0;
    virtual char GetIOState( unsigned char channel ) = 0;};

} // namespace IVdso
