// SPDX-License-Identifier: GPL-3.0-or-later

#include "params.h"

#include "ctpu.h"
#include "scopesettings.h"

#include <QCoreApplication>

namespace Params {

namespace {

/// Единственное объявление параметров. Порядок здесь = порядок в интерфейсе.
///
/// Группа `ctpu` — перевод канала в физические единицы, P = k·V + b.
/// Переехала сюда первой: это главная функция прибора, а жила она в
/// четвёртой вкладке диалога настроек, глубже, чем настройки, которые
/// трогают раз в жизни (замечание пользователя 2026-08-22).
std::vector< Def > buildRegistry() {
    std::vector< Def > out;

    Def mode;
    mode.group = QStringLiteral( "ctpu" );
    mode.key = QStringLiteral( "ctpuMode" );
    mode.kind = Kind::Choice;
    mode.label = QCoreApplication::translate( "Params", "Mode" );
    mode.choices << QCoreApplication::translate( "Params", "Off (volts)" )
                 << QCoreApplication::translate( "Params", "Formula P = k·V + b" )
                 << QCoreApplication::translate( "Params", "Calibrated (CCtPU)" );
    mode.tip = QCoreApplication::translate(
        "Params", "Off keeps volts. Formula applies your own k and b. Calibrated derives them "
                  "from a two-point measurement against a physical reference." );
    mode.defaultValue = int( CtPU::Mode::OFF );
    mode.get = []( const DsoSettingsScope &s, ChannelID ch ) {
        return int( s.voltage[ ch ].ctpuMode );
    };
    mode.set = []( DsoSettingsScope &s, ChannelID ch, const QVariant &v ) {
        s.voltage[ ch ].ctpuMode = CtPU::Mode( v.toInt() );
    };
    out.push_back( std::move( mode ) );

    Def unit;
    unit.group = QStringLiteral( "ctpu" );
    unit.key = QStringLiteral( "ctpuUnit" );
    unit.kind = Kind::Text;
    unit.label = QCoreApplication::translate( "Params", "Unit" );
    unit.max = 8; // для Text max — предел длины строки
    unit.tip = QCoreApplication::translate( "Params", "Physical unit shown on the scale: V, °C, A, kPa, Ω …" );
    unit.defaultValue = QStringLiteral( "V" );
    unit.get = []( const DsoSettingsScope &s, ChannelID ch ) { return s.voltage[ ch ].ctpuUnit; };
    unit.set = []( DsoSettingsScope &s, ChannelID ch, const QVariant &v ) {
        s.voltage[ ch ].ctpuUnit = v.toString();
    };
    out.push_back( std::move( unit ) );

    Def k;
    k.group = QStringLiteral( "ctpu" );
    k.key = QStringLiteral( "ctpuK" );
    k.kind = Kind::Real;
    k.label = QCoreApplication::translate( "Params", "Sensitivity k" );
    k.unit = QCoreApplication::translate( "Params", "unit/V" );
    k.min = -1e9;
    k.max = 1e9;
    k.decimals = 6;
    k.tip = QCoreApplication::translate( "Params", "How many physical units one volt at the input represents." );
    k.defaultValue = 1.0;
    k.get = []( const DsoSettingsScope &s, ChannelID ch ) { return s.voltage[ ch ].ctpuK; };
    k.set = []( DsoSettingsScope &s, ChannelID ch, const QVariant &v ) {
        const double val = v.toDouble();
        // Ноль запрещён: шкала В/дел умножается на k, при k = 0 весь экран
        // схлопывается в одну линию и прибор молча перестаёт показывать.
        s.voltage[ ch ].ctpuK = ( val == 0.0 ) ? 1.0 : val;
    };
    out.push_back( std::move( k ) );

    Def b;
    b.group = QStringLiteral( "ctpu" );
    b.key = QStringLiteral( "ctpuB" );
    b.kind = Kind::Real;
    b.label = QCoreApplication::translate( "Params", "Offset b" );
    b.unit = QCoreApplication::translate( "Params", "unit" );
    b.min = -1e9;
    b.max = 1e9;
    b.decimals = 6;
    b.tip = QCoreApplication::translate( "Params", "Constant added after scaling: the physical value at zero volts." );
    b.defaultValue = 0.0;
    b.get = []( const DsoSettingsScope &s, ChannelID ch ) { return s.voltage[ ch ].ctpuB; };
    b.set = []( DsoSettingsScope &s, ChannelID ch, const QVariant &v ) {
        s.voltage[ ch ].ctpuB = v.toDouble();
    };
    out.push_back( std::move( b ) );

    return out;
}

} // namespace


const std::vector< Def > &registry() {
    static const std::vector< Def > reg = buildRegistry();
    return reg;
}


std::vector< const Def * > group( const QString &name ) {
    std::vector< const Def * > out;
    for ( const Def &d : registry() )
        if ( d.group == name )
            out.push_back( &d );
    return out;
}


QVariant clamp( const Def &def, const QVariant &value ) {
    switch ( def.kind ) {
    case Kind::Bool:
        return value.toBool();
    case Kind::Int: {
        const int v = value.toInt();
        return int( qBound( def.min, double( v ), def.max ) );
    }
    case Kind::Real:
        return qBound( def.min, value.toDouble(), def.max );
    case Kind::Text: {
        QString s = value.toString();
        const int limit = def.max > 0 ? int( def.max ) : s.size();
        if ( s.size() > limit )
            s.truncate( limit );
        return s;
    }
    case Kind::Choice: {
        const int v = value.toInt();
        if ( def.choices.isEmpty() )
            return v;
        return qBound( 0, v, def.choices.size() - 1 );
    }
    }
    return value;
}

} // namespace Params
