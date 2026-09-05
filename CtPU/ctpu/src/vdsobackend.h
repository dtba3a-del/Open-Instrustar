// SPDX-License-Identifier: GPL-3.0-or-later
// Last edited: 2026-09-06 UTC
//
// Задняя сторона окна генератора: путь A через вендорскую `vdso.dll`.
//
// **`vdso.dll` в наш архив не кладётся и класть её нельзя.** Вендорской
// лицензии не существует, а наша GPL-3.0 включения не допускает
// (`docs/THIRD_PARTY.md`). Библиотека **загружается** там, где она уже лежит
// у оператора, — это правомерно и разобрано в
// `docs/ВЫЗОВ-ЧУЖИХ-МОДУЛЕЙ.md`: мы пользуемся чужим программно-аппаратным
// комплексом по назначению, без изменений и без вскрытия.
//
// **Где её взять.** Установщик вендорского приложения `vdso.dll` НЕ ставит —
// проверено по установленной папке: там `signal.dll`, `winusb.dll` и
// соинсталляторы драйвера, а `vdso.dll` нет. Она входит в **SDK** вендора и
// лежит в этом репозитории: `references/vendor/instrustar-sdk/bin/vdso.dll`.
// Достаточно положить её рядом с `ctpubintapetimechannel.exe` — или указать
// путь в окне генератора один раз, он запомнится.

#pragma once

#include "ddsdialog.h"

#include <QString>
#include <memory>

namespace IVdso {
class Loader;
class Session;
class Clock;
} // namespace IVdso

class VdsoBackend : public DdsBackend {
  public:
    VdsoBackend();
    ~VdsoBackend() override;

    // --- DdsBackend ---
    bool available() const override;
    QString unavailableReason() const override;
    unsigned int waveformMask() const override;
    unsigned int frequencyMinHz() const override;
    unsigned int frequencyMaxHz() const override;
    bool apply( const DdsRequest &request ) override;
    QString lastError() const override;

    bool needsLibraryPath() const override;
    bool setLibraryPath( const QString &path ) override;
    QString libraryPath() const override;

    /// Где искать библиотеку, по порядку: рядом с исполняемым файлом,
    /// затем сохранённый выбор оператора, затем обычные места установки.
    /// Пустая строка — не нашлась нигде.
    static QString findLibrary();

    /// Открыть прибор, если ещё не открыт. Вызывается лениво: подъём стоит
    /// секунду (перечисление USB), и платить её при старте программы
    /// незачем — генератор нужен не всегда.
    void ensureOpen();

  private:
    std::unique_ptr< IVdso::Clock > m_clock;
    std::unique_ptr< IVdso::Loader > m_loader;
    std::unique_ptr< IVdso::Session > m_session;
    QString m_path;
    QString m_error;
    bool m_tried = false;
};
