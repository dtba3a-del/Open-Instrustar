// SPDX-License-Identifier: GPL-3.0-or-later

#include "instrumentregistry.h"

#include <QDebug>
#include <algorithm>

namespace Instrument {

Registry &Registry::instance() {
    static Registry reg;
    return reg;
}


bool Registry::declare( Type type, const QString &name, Factory factory ) {
    const int key = int( type );
    if ( m_entries.count( key ) ) {
        // Двойное объявление типа — это две модели под одним номером, то
        // есть молчаливая подмена прибора. Лучше шумно, чем тихо.
        qWarning() << "Instrument::Registry: тип" << key << "уже объявлен как" << m_entries[ key ].name
                   << "- повторное объявление" << name << "отклонено";
        return false;
    }
    m_entries[ key ] = Entry{ name, std::move( factory ) };
    return true;
}


std::unique_ptr< Backend > Registry::create( Type type, int slot ) const {
    auto it = m_entries.find( int( type ) );
    if ( it == m_entries.end() || !it->second.factory )
        return nullptr;
    return it->second.factory( slot );
}


QString Registry::name( Type type ) const {
    auto it = m_entries.find( int( type ) );
    return it == m_entries.end() ? QString() : it->second.name;
}


std::vector< Type > Registry::declared() const {
    std::vector< Type > out;
    out.reserve( m_entries.size() );
    for ( const auto &kv : m_entries )
        out.push_back( Type( kv.first ) );
    return out;
}


void Set::rebuild( const std::vector< Type > &types ) {
    // Связь прежних приборов разрывается явно: молчаливое разрушение
    // объекта с открытым портом оставляет порт занятым до перезапуска.
    for ( auto &b : m_backends )
        if ( b )
            b->unlink();
    m_backends.clear();

    const int n = std::min( int( types.size() ), maxSlots );
    for ( int i = 0; i < n; ++i ) {
        if ( types[ size_t( i ) ] == Type::None )
            continue;
        auto backend = Registry::instance().create( types[ size_t( i ) ], i );
        if ( backend )
            m_backends.push_back( std::move( backend ) );
    }
}


void Set::update() {
    // Обход НЕ является путём данных, и это главное свойство: темпы
    // источников связки различаются на семь порядков (30 МБ/с у
    // осциллографа против единиц измерений в секунду у Е7-12). Один обход
    // по кругу заставил бы быстрый прибор ждать медленного — поток встал бы
    // на квитировании шины КОП.
    //
    // Поэтому трогаются ТОЛЬКО приборы, которые сами объявили, что отвечают
    // на опрос. Потоковый гонит данные своим ходом, событийный движим шиной;
    // их «опрос» был бы ожиданием.
    for ( auto &b : m_backends ) {
        if ( !b || !b->state().linked )
            continue;
        if ( b->flow().pace != Pace::OnRequest )
            continue;
        b->update();
    }
}


Backend *Set::slot( int index ) const {
    if ( index < 0 || index >= int( m_backends.size() ) )
        return nullptr;
    return m_backends[ size_t( index ) ].get();
}

} // namespace Instrument
