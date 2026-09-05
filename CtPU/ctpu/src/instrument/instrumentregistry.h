// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file instrumentregistry.h
/// \brief Фронтенд приборов: сколько их, какого типа, и как создаётся нужный.
///
/// Соответствует `RangeFinder` из `libraries/AP_RangeFinder/AP_RangeFinder.cpp`
/// (ArduPilot): держит N слотов, у каждого свой тип, и создаёт бэкенд по
/// типу — сам про конкретные приборы не зная ничего.
///
/// Регистрация модели — как у них: одна строка. Модель объявляет свой тип и
/// способ создания, фронтенд их не перечисляет.

#include "instrumentbackend.h"

#include <functional>
#include <map>

namespace Instrument {

/// Как создать бэкенд слота. Параметры слота (порт, VID:PID, скорость)
/// берутся из реестра параметров (`params.h`), а не из аргументов: иначе
/// каждый новый прибор добавлял бы аргумент, и подпись росла бы вечно —
/// та самая размазанность, ради избавления от которой всё и делается.
using Factory = std::function< std::unique_ptr< Backend >( int slot ) >;

/// \brief Реестр типов приборов. Единственное место, где тип связан с
/// созданием; перечня моделей во фронтенде нет.
class Registry {
  public:
    static Registry &instance();

    /// \brief Объявить модель. Вызывается один раз на модель.
    /// \return true, если тип не был занят (двойное объявление — ошибка
    ///         сборки проекта, а не молчаливая подмена).
    bool declare( Type type, const QString &name, Factory factory );

    /// \brief Создать бэкенд слота по типу; nullptr, если тип не объявлен.
    std::unique_ptr< Backend > create( Type type, int slot ) const;

    /// \brief Человекочитаемое имя типа («ISDS-205B», «Oscill», «Demo»).
    QString name( Type type ) const;

    /// \brief Все объявленные типы в порядке номеров — для перечня в
    /// интерфейсе и для метаданных параметра «тип слота».
    std::vector< Type > declared() const;

  private:
    struct Entry {
        QString name;
        Factory factory;
    };
    std::map< int, Entry > m_entries;
};

/// \brief Приборы текущей сессии: слоты, их бэкенды и общий ход.
///
/// Слот — это «место, куда цепляется прибор». Их несколько намеренно:
/// осциллограф и Oscill могут работать одновременно, манипулятор — третьим.
/// Прежде такого места не было вовсе, и второй прибор было некуда деть.
class Set {
  public:
    static constexpr int maxSlots = 4;

    /// \brief Пересоздать бэкенды по типам слотов из настроек.
    /// Вызывается при запуске и после смены типа слота пользователем.
    void rebuild( const std::vector< Type > &types );

    /// \brief Периодический ход по всем связанным приборам.
    void update();

    Backend *slot( int index ) const;
    int count() const { return int( m_backends.size() ); }

  private:
    std::vector< std::unique_ptr< Backend > > m_backends;
};

} // namespace Instrument
