// SPDX-License-Identifier: GPL-3.0-or-later
// Last edited: 2026-09-05 UTC
//
// Окно генератора DDS.
//
// Распоряжение автора 2026-09-04: «в GUI нет кнопки окна DDS, органов
// управления и окна DDS. напиши кнопку окна DDS, окно DDS и в нем органы
// управления DDS.»
//
// Порядок работы проекта (`docs/GUI-FIRST.md`): панель строится первой и
// служит спецификацией; функционал прописывается в движок, а не в элемент.
// Поэтому окно ничего не знает ни про `vdso.dll`, ни про USB — оно работает
// через DdsBackend. Пока задней стороны нет, органы неактивны, и причина
// названа словами, а не показана пустым окном.
//
// Чего здесь НЕТ намеренно (распоряжение автора того же дня):
//
//   * **органов «размах» (Zoom) и «смещение» (Bias)**. В таблице подбора
//     модели вендора у генератора ровно три строки — разрядность ЦАП,
//     диапазон частот и шаг; амплитуды и смещения там нет ни у одной
//     модели. На 205B и 210B они не реализованы, а 205X в работе не
//     используется. Орган, который ничего не делает, хуже отсутствующего:
//     он лжёт молча;
//   * **логического анализатора**. Он есть только у 205C и 205X, которые
//     не используются, и вендор прямо пишет: «DDS и осциллограф работают
//     одновременно; логический анализатор и осциллограф — нет».

#pragma once

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;

/// Что окно просит у генератора. Целые герцы — не упрощение: вендорский
/// `SetDDSPinlv` принимает `unsigned int` и дробных не берёт вовсе.
struct DdsRequest {
    unsigned int waveform = 0;    ///< 0 синус, 1 меандр, 2 треугольник, 3/4 пилы, 5/6 не описаны
    unsigned int frequencyHz = 1000;
    int dutyPercent = 50;
    bool output = false;
};

/// Задняя сторона окна. Реализуется тем, кто умеет говорить с прибором;
/// сегодня таких нет — путь B (fx2lafw) генератора не имеет вовсе, а путь A
/// в приложение ещё не сведён.
class DdsBackend {
  public:
    virtual ~DdsBackend() = default;

    /// Прибор с генератором доступен прямо сейчас.
    virtual bool available() const = 0;
    /// Почему недоступен — текстом для оператора, а не кодом.
    virtual QString unavailableReason() const = 0;
    /// Маска поддержанных форм сигнала: бит N = форма N. 0 — маска
    /// неизвестна, и тогда окно не запрещает ничего, но и не обещает.
    virtual unsigned int waveformMask() const = 0;
    /// Границы частоты, Гц. У 205B/210B/220B — 1…20 000 000.
    virtual unsigned int frequencyMinHz() const = 0;
    virtual unsigned int frequencyMaxHz() const = 0;
    /// Применить. Ложь — не применено; текст ошибки в lastError().
    virtual bool apply( const DdsRequest &request ) = 0;
    virtual QString lastError() const = 0;

    /// Задней стороне не хватает файла библиотеки, и оператор может его
    /// указать. По умолчанию — нет: не всякая задняя сторона так устроена.
    virtual bool needsLibraryPath() const { return false; }
    /// Указать файл библиотеки. Ложь — не подошёл, причина в lastError().
    virtual bool setLibraryPath( const QString & ) { return false; }
    /// Какой файл используется сейчас (пусто — никакой).
    virtual QString libraryPath() const { return QString(); }
};

/// \brief Окно «Генератор DDS».
class DdsDialog : public QDialog {
    Q_OBJECT

  public:
    /// `backend` может быть nullptr: окно тогда показывает, чего не хватает.
    /// Владение не передаётся.
    explicit DdsDialog( DdsBackend *backend, QWidget *parent = nullptr );

  private:
    void refreshState();
    void applyRequest();
    DdsRequest currentRequest() const;

    DdsBackend *m_backend;

    QLabel *m_state = nullptr;
    QComboBox *m_waveform = nullptr;
    QSpinBox *m_frequency = nullptr;
    QSpinBox *m_duty = nullptr;
    QCheckBox *m_output = nullptr;
    QPushButton *m_apply = nullptr;
    QPushButton *m_pickLibrary = nullptr;
    QLabel *m_result = nullptr;
};
