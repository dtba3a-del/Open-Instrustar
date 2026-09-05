// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QColor>
#include <QPushButton>

/// \brief A widget for the selection of a color.
class ColorBox : public QPushButton {
    Q_OBJECT

  public:
    ColorBox( QColor color, QWidget *parent = nullptr );
    ~ColorBox() override;

    const QColor getColor();

  public slots:
    void setColor( QColor color );
    void waitForColor();

  private:
    QColor color;

    /// \brief Быстрая палитра основных цветов.
    /// Жёсткая привязка «CH1 жёлтый / CH2 синий» неудобна на практике:
    /// у щупов разные маркировочные кольца, и трасса должна совпадать с
    /// кольцом своего щупа, иначе оператор путает каналы (замечание
    /// пользователя 2026-08-21). Ряд взят по маркировке проводов и щупов
    /// (МЭК 60757 / ГОСТ 28763): чёрный, коричневый, красный, оранжевый,
    /// жёлтый, зелёный, синий, фиолетовый, серый, белый + голубой и
    /// пурпурный как ходовые на экране.
    static const QVector< QPair< QColor, QString > > &basicColors();

  signals:
    void colorChanged( QColor color ); ///< The color has been changed
};
