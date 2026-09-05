////////////////////////////////////////////////////////////////////////////////
//
//  OpenHantek
//  colorbox.cpp
//
//  Copyright (C) 2010  Oliver Haag
//  oliver.haag@gmail.com
//
//  This program is free software: you can redistribute it and/or modify it
//  under the terms of the GNU General Public License as published by the Free
//  Software Foundation, either version 3 of the License, or (at your option)
//  any later version.
//
//  This program is distributed in the hope that it will be useful, but WITHOUT
//  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
//  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
//  more details.
//
//  You should have received a copy of the GNU General Public License along with
//  this program.  If not, see <http://www.gnu.org/licenses/>.
//
////////////////////////////////////////////////////////////////////////////////

#include <QColorDialog>
#include <QFocusEvent>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QVector>

#include "colorbox.h"

////////////////////////////////////////////////////////////////////////////////
// class ColorBox
/// \brief Initializes the widget.
/// \param color_ Initial color value.
/// \param parent The parent widget.
ColorBox::ColorBox( QColor color_, QWidget *parent ) : QPushButton( parent ) {
    setColor( color_ );
    connect( this, &QAbstractButton::clicked, this, &ColorBox::waitForColor );
}

/// \brief Cleans up the widget.
ColorBox::~ColorBox() {}

/// \brief Get the current color.
/// \return The current color as QColor.
const QColor ColorBox::getColor() { return color; }

/// \brief Sets the color.
/// \param newColor The new color.
void ColorBox::setColor( QColor newColor ) {
    color = newColor;
    setText( QString( "#%1" ).arg( unsigned( color.rgba() ), 8, 16, QChar( '0' ) ) );
    setPalette( QPalette( color ) );
    emit colorChanged( color );
}

const QVector< QPair< QColor, QString > > &ColorBox::basicColors() {
    static const QVector< QPair< QColor, QString > > table = {
        { QColor( 0xff, 0xff, 0x00 ), QObject::tr( "Yellow" ) },
        { QColor( 0x00, 0xa0, 0xff ), QObject::tr( "Blue" ) },
        { QColor( 0xff, 0x40, 0x40 ), QObject::tr( "Red" ) },
        { QColor( 0x40, 0xe0, 0x40 ), QObject::tr( "Green" ) },
        { QColor( 0xff, 0xa0, 0x30 ), QObject::tr( "Orange" ) },
        { QColor( 0x40, 0xe0, 0xe0 ), QObject::tr( "Cyan" ) },
        { QColor( 0xd0, 0x60, 0xff ), QObject::tr( "Violet" ) },
        { QColor( 0xff, 0x60, 0xc0 ), QObject::tr( "Magenta" ) },
        { QColor( 0xa0, 0x70, 0x40 ), QObject::tr( "Brown" ) },
        { QColor( 0xc0, 0xc0, 0xc0 ), QObject::tr( "Grey" ) },
        { QColor( 0xff, 0xff, 0xff ), QObject::tr( "White" ) },
        { QColor( 0x30, 0x30, 0x30 ), QObject::tr( "Black" ) },
    };
    return table;
}


/// \brief Меню быстрых цветов, затем — стандартный диалог.
/// Порядок именно такой: в 9 случаях из 10 нужен цвет кольца щупа, он
/// тут же в списке; произвольный оттенок остаётся доступен пунктом
/// «Другой цвет ..», который открывает штатный QColorDialog.
void ColorBox::waitForColor() {
    setFocus();
    setDown( true );

    QMenu menu( this );
    for ( const auto &entry : basicColors() ) {
        QPixmap swatch( 24, 16 );
        swatch.fill( entry.first );
        QPainter p( &swatch );
        p.setPen( QColor( 0x80, 0x80, 0x80 ) );
        p.drawRect( 0, 0, 23, 15 );
        p.end();
        QAction *a = menu.addAction( QIcon( swatch ), entry.second );
        a->setData( entry.first );
    }
    menu.addSeparator();
    QAction *custom = menu.addAction( tr( "Other color .." ) );

    QAction *chosen = menu.exec( mapToGlobal( rect().bottomLeft() ) );
    if ( !chosen ) {
        setDown( false );
        return;
    }
    if ( chosen != custom ) {
        QColor picked = chosen->data().value< QColor >();
        picked.setAlpha( color.alpha() ? color.alpha() : 255 );
        setColor( picked );
        setDown( false );
        return;
    }
    QColor newColor = QColorDialog::getColor( color, this, nullptr, QColorDialog::ShowAlphaChannel );
    if ( newColor.isValid() )
        setColor( newColor );
}
