// SPDX-License-Identifier: GPL-2.0-or-later

#include "finddevices.h"

#include <QCoreApplication>
#include <QDebug>
#include <QList>
#include <QTemporaryFile>
#include <algorithm>
#include "models/modelDEMO.h"
#include "modelregistry.h"

#ifdef Q_OS_FREEBSD
#include <libusb.h>
#else
#include <libusb-1.0/libusb.h>
#endif


FindDevices::FindDevices( libusb_context *context, int verboseLevel ) : context( context ), verboseLevel( verboseLevel ) {
    if ( verboseLevel > 1 )
        qDebug() << " FindDevices::FindDevices()";
}


// Iterate all devices on USB and keep track of all supported scopes
int FindDevices::updateDeviceList() {
    if ( verboseLevel > 2 )
        qDebug() << "  FindDevices::updateDeviceList()";
    libusb_device **deviceList;
    ssize_t deviceCount = libusb_get_device_list( context, &deviceList );
    if ( deviceCount < 0 ) {
        return int( deviceCount );
    }

    ++findIteration;
    int changes = 0;

    for ( ssize_t deviceIndex = 0; deviceIndex < deviceCount; ++deviceIndex ) {
        libusb_device *device = deviceList[ deviceIndex ];
        // Get device descriptor
        struct libusb_device_descriptor descriptor;
        libusb_get_device_descriptor( device, &descriptor );

        if ( 0x0000 == descriptor.idVendor ) // windows sometimes reports bogus value vid=0x0000
            continue;

        if ( 0x1d6b == descriptor.idVendor ) // skip linux foundation devices, e.g. usb root hubs
            continue;

        const UniqueUSBid USBid = ScopeDevice::computeUSBdeviceID( device );

        DeviceList::const_iterator inList = devices.find( USBid );
        if ( inList != devices.end() ) { // already in list, update heartbeat only
            inList->second->setFindIteration( findIteration );
            continue;
        }
        // else check against all supported models for match
        //
        // РАЗЛИЧЕНИЕ МОДЕЛЕЙ С ОДИНАКОВЫМ VID:PID (найдено на приборе
        // пользователя 2026-08-21). MDSO и ISDS205B обе объявлены как
        // 1d50:608e, а прежний код брал ПЕРВУЮ подошедшую модель и выходил.
        // ISDS205B определялся как MDSO, а вместе с моделью подменялись
        // таблицы прибора: voltageScale 250 вместо 1330 digit/V на
        // чувствительных пределах (ошибка в 5.3 раза во ВСЕХ показаниях) и
        // лишний предел 5 В/дел. Отличает их версия прошивки в дескрипторе
        // (bcdDevice): 0x0005 у ISDS205B, 0x0001 у MDSO — по ней и выбираем.
        DSOModel *exactMatch = nullptr;     // VID:PID и версия прошивки совпали
        DSOModel *flashedMatch = nullptr;   // VID:PID прошитого прибора совпали
        DSOModel *unflashedMatch = nullptr; // прибор ещё без прошивки
        for ( DSOModel *model : ModelRegistry::get()->models() ) {
            if ( DemoDeviceID == model->ID ) // skip the DEMO device
                continue;
            const bool flashed = descriptor.idVendor == model->vendorID && descriptor.idProduct == model->productID;
            // Devices without firmware have different VID/PIDs
            const bool unflashed =
                descriptor.idVendor == model->vendorIDnoFirmware && descriptor.idProduct == model->productIDnoFirmware;
            if ( flashed && descriptor.bcdDevice == model->firmwareVersion && !exactMatch )
                exactMatch = model;
            if ( flashed && !flashedMatch )
                flashedMatch = model;
            if ( unflashed && !unflashedMatch )
                unflashedMatch = model;
        }
        // Порядок предпочтения: точное совпадение прошивки → прошитый прибор
        // → непрошитый. Непрошитый различить нечем (bcdDevice ещё не тот),
        // и это честно: там VID:PID у моделей разные.
        DSOModel *chosen = exactMatch ? exactMatch : ( flashedMatch ? flashedMatch : unflashedMatch );
        if ( chosen ) { // put matching device into list if not already in use
            ++changes;
            if ( verboseLevel > 2 )
                qDebug() << "  +++" << QString( "0x%1" ).arg( USBid, 8, 16, QChar( '0' ) ) << chosen->name
                         << ( exactMatch ? "(fw match)" : "(vid:pid only)" );
            devices[ USBid ] = std::unique_ptr< ScopeDevice >( new ScopeDevice( chosen, device, findIteration ) );
        }
    }

    // Remove non existing devices
    for ( DeviceList::iterator it = devices.begin(); it != devices.end(); ) {
        if ( it->second->getFindIteration() != findIteration ) { // heartbeat not up to date, no more on the bus
            ++changes;
            if ( verboseLevel > 2 )
                qDebug() << "  ---" << QString( "0x%1" ).arg( it->first, 8, 16, QChar( '0' ) ) << it->second->getModel()->name;
            // printf( "- %016lX\n", it->first );
            it = devices.erase( it ); // it points to next entry
        } else {
            ++it;
        }
    }
    libusb_free_device_list( deviceList, false );
    return changes; // report number of all detected bus changes (added + removed devices)
}


const FindDevices::DeviceList *FindDevices::getDevices() { return &devices; }


std::unique_ptr< ScopeDevice > FindDevices::takeDevice( UniqueUSBid id ) {
    DeviceList::iterator it = devices.find( id );
    if ( it == devices.end() )
        return nullptr;
    return std::move( it->second );
}
