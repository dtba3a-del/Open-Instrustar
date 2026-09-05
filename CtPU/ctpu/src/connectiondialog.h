// SPDX-License-Identifier: GPL-3.0-or-later
// Окно связи: с каким прибором и по какому протоколу мы работаем.

#pragma once

#include <QDialog>

class HantekDsoControl;
class QTextBrowser;

/// \brief Диалог «Связь» — единая точка правды о подключении.
///
/// Замечание пользователя (2026-08-21): Oscill подключён физически, а в
/// приложении нет ни кнопки, ни окна, где видно, с каким прибором и по
/// какому протоколу идёт работа. У штатного ПО Oscill такое окно есть
/// (Oscilink: порт, скорость, OBEX, выбор драйвера USB) — там это
/// отдельное окно, и это правильно.
///
/// Окно показывает ТОЛЬКО факты и честно разделяет реализованное и
/// запланированное: выдавать неработающий транспорт за рабочий — та же
/// «правдоподобная выдумка», против которой заведён ENGINEERING_LOG.
class ConnectionDialog : public QDialog {
    Q_OBJECT

  public:
    explicit ConnectionDialog( HantekDsoControl *dsoControl, QWidget *parent = nullptr );

  private:
    void refresh();

    /// Опрос шины USB в момент нажатия «Refresh».
    ///
    /// До 2026-09-04 кнопка «Refresh» перерисовывала текст и **ничего не
    /// опрашивала**: сведения брались из объекта прибора, созданного при
    /// запуске. Замечание автора — «в окне Connect кнопка refresh
    /// фиктивная?» — верно: если прибора при запуске не было, нажатие
    /// не могло его найти. Здесь идёт настоящее перечисление шины.
    QString usbScanHtml() const;

    HantekDsoControl *dsoControl;
    QTextBrowser *info;
};
