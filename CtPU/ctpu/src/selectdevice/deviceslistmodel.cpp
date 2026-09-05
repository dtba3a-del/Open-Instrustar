// SPDX-License-Identifier: GPL-2.0-or-later

#include "deviceslistmodel.h"
#include "dsomodel.h"
#include "usb/finddevices.h"
#include "usb/uploadFirmware.h"
#include <QColor>
#include <QDebug>

DevicesListModel::DevicesListModel( FindDevices *findDevices, int verboseLevel )
    : findDevices( findDevices ), verboseLevel( verboseLevel ) {}

int DevicesListModel::rowCount( const QModelIndex & ) const { return int( entries.size() ); }

int DevicesListModel::columnCount( const QModelIndex & ) const { return 2; }

QVariant DevicesListModel::headerData( int section, Qt::Orientation orientation, int role ) const {
    if ( orientation == Qt::Vertical )
        return QAbstractTableModel::headerData( section, orientation, role );
    if ( role == Qt::DisplayRole ) {
        switch ( section ) {
        case 0:
            return QObject::tr( "Devicename" );
        case 1:
            return QObject::tr( "Status" );
        default:
            return QVariant();
        }
    }
    return QAbstractTableModel::headerData( section, orientation, role );
}

QVariant DevicesListModel::data( const QModelIndex &index, int role ) const {
    if ( !index.isValid() )
        return QVariant();
    const unsigned row = unsigned( index.row() );
    if ( role == Qt::UserRole )
        return QVariant::fromValue( entries[ row ].id );
    if ( role == Qt::UserRole + 1 )
        return QVariant::fromValue( entries[ row ].canConnect );
    if ( role == Qt::UserRole + 2 )
        return QVariant::fromValue( entries[ row ].needFirmware );
    if ( role == Qt::UserRole + 3 )
        return QVariant::fromValue( entries[ row ].errorMessage );

    if ( role == Qt::DisplayRole ) {
        if ( index.column() == 0 ) {
            return entries[ row ].name;
        } else if ( index.column() == 1 ) {
            return entries[ row ].getStatus();
        }
    }

    if ( role == Qt::BackgroundRole ) {
        if ( entries[ row ].canConnect )
            return QColor( Qt::darkGreen ).lighter();
        else if ( entries[ row ].needFirmware )
            return QColor( Qt::yellow ).lighter();
    }

    return QVariant();
}

bool DevicesListModel::uploadFirmware( UniqueUSBid id, QString &errorMessage ) {
    // Единственное место, где приложение пишет прошивку в прибор, и вызвать
    // его может только явное действие оператора. Сама запись ещё раз
    // спрашивает класс драйвера (`usb/driverclass.h`): под вендорским
    // драйвером она отказывает и объясняет, почему.
    const FindDevices::DeviceList *devices = findDevices->getDevices();
    auto it = devices->find( id );
    if ( it == devices->end() ) {
        errorMessage = tr( "The device is no longer on the bus." );
        return false;
    }
    UploadFirmware uf;
    if ( uf.startUpload( it->second.get() ) )
        return true;
    errorMessage = uf.getErrorMessage();
    return false;
}


void DevicesListModel::updateDeviceList() {
    beginResetModel();
    entries.clear();
    endResetModel();
    const FindDevices::DeviceList *devices = findDevices->getDevices();
    beginInsertRows( QModelIndex(), 0, int( devices->size() ) );
    for ( auto &i : *devices ) {
        DeviceListEntry entry;
        entry.name = i.second->getModel()->name;
        entry.id = i.first;
        if ( i.second->needsFirmware() ) {
            // ПЕРЕЧИСЛЕНИЕ НИЧЕГО НЕ ПИШЕТ В ПРИБОР (распоряжение автора
            // 2026-09-05). Прежде эта строка заливала прошивку прямо из
            // обхода списка — то есть открытое окно выбора перепрошивало
            // устройство само, раз в секунду, никого не спросив. Список —
            // это ЗАПРОС, а заливка — ДЕЙСТВИЕ; смешение классов и есть та
            // самая ошибка, о которой автор писал про набор команд пути A
            // (`docs/ОТВЕТЫ-2026-09-05.md` §1).
            //
            // Теперь строка только сообщает: прибору нужна прошивка.
            // Заливка — по явному выбору оператора, через uploadFirmware().
            if ( verboseLevel > 2 )
                qDebug() << "  DevicesListModel::updateDeviceList()" << entry.name << "needs firmware";
            entry.needFirmware = true;
        } else if ( i.second->connectDevice( entry.errorMessage ) ) {
            if ( verboseLevel > 2 )
                qDebug() << "  DevicesListModel::updateDeviceList()" << entry.name << "can connect";
            entry.canConnect = true;
            i.second->disconnectFromDevice();
        } else {
            if ( verboseLevel > 2 )
                qDebug() << "  DevicesListModel::updateDeviceList()" << entry.name << "cannot connect";
            entry.canConnect = false;
        }
        entries.push_back( entry );
    }
    endInsertRows();
}
