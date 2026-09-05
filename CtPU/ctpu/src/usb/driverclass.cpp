// SPDX-License-Identifier: GPL-3.0-or-later
// Last edited: 2026-09-05 UTC

#include "driverclass.h"

#include <QCoreApplication>

#ifdef Q_OS_WIN
#include <windows.h>
// setupapi.h обязан идти ПОСЛЕ windows.h: он опирается на его объявления.
#include <setupapi.h>
#endif

namespace UsbDriverClass {

#ifdef Q_OS_WIN

namespace {

QString devProperty( HDEVINFO set, SP_DEVINFO_DATA &data, DWORD property ) {
    DWORD size = 0;
    SetupDiGetDeviceRegistryPropertyW( set, &data, property, nullptr, nullptr, 0, &size );
    if ( !size )
        return QString();
    QVector< wchar_t > buf( int( size / sizeof( wchar_t ) ) + 1, 0 );
    if ( !SetupDiGetDeviceRegistryPropertyW( set, &data, property, nullptr,
                                             reinterpret_cast< PBYTE >( buf.data() ), size, nullptr ) )
        return QString();
    return QString::fromWCharArray( buf.constData() );
}

} // namespace

Verdict forDevice( uint16_t vid, uint16_t pid ) {
    Verdict v;

    // Ищем по идентификатору оборудования — тому самому, по которому Windows
    // и выбирает драйвер (`docs/ОТВЕТЫ-2026-09-05.md` §2).
    const QString want = QStringLiteral( "VID_%1&PID_%2" )
                             .arg( vid, 4, 16, QChar( '0' ) )
                             .arg( pid, 4, 16, QChar( '0' ) )
                             .toUpper();

    HDEVINFO set = SetupDiGetClassDevsW( nullptr, L"USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT );
    if ( set == INVALID_HANDLE_VALUE )
        return v;

    SP_DEVINFO_DATA data;
    data.cbSize = sizeof( data );
    for ( DWORD i = 0; SetupDiEnumDeviceInfo( set, i, &data ); ++i ) {
        const QString hw = devProperty( set, data, SPDRP_HARDWAREID ).toUpper();
        if ( !hw.contains( want ) )
            continue;
        v.service = devProperty( set, data, SPDRP_SERVICE );
        v.description = devProperty( set, data, SPDRP_DEVICEDESC );

        // Опознание вендорского драйвера — по его собственному описанию.
        // Вендорский `Dso.inf` объявляет его дословно как
        // `YiXingDianZi USB DSO(vN) Driver`; служба у него та же WINUSB, что
        // и у поставленного Zadig, поэтому по службе их не различить.
        // Признак — имя вендора, и оно из первоисточника, а не придумано:
        // `references/vendor/instrustar-installed-en/ЧТЕНИЕ-ПО-ФАЙЛАМ/Dso.inf.md`.
        if ( v.description.contains( QStringLiteral( "YiXingDianZi" ), Qt::CaseInsensitive ) )
            v.cls = Class::VendorInstruStar;
        else if ( !v.service.isEmpty() )
            v.cls = Class::LibusbFamily;
        break;
    }
    SetupDiDestroyDeviceInfoList( set );
    return v;
}

#else

Verdict forDevice( uint16_t, uint16_t ) {
    // Вендорского стека вне Windows нет вовсе, различать нечего.
    return Verdict();
}

#endif


bool firmwareWriteAllowed( const Verdict &v ) { return v.cls != Class::VendorInstruStar; }


QString explain( const Verdict &v ) {
    switch ( v.cls ) {
    case Class::VendorInstruStar:
        return QCoreApplication::translate(
                   "UsbDriverClass",
                   "The instrument is held by the vendor driver (%1). Writing fx2lafw firmware into it "
                   "is refused: the write would go through, the instrument would renumerate as "
                   "1d50:608e, and no driver in the system answers for that identity — neither the "
                   "vendor stack nor this application could use it until the device is re-plugged. "
                   "To walk this application's own path, switch the driver with Zadig first." )
            .arg( v.description.isEmpty() ? QStringLiteral( "InstruStar" ) : v.description );
    case Class::LibusbFamily:
        return QCoreApplication::translate( "UsbDriverClass", "Driver: %1 — firmware upload is allowed." )
            .arg( v.service );
    case Class::Unknown:
        break;
    }
    return QCoreApplication::translate( "UsbDriverClass",
                                        "The driver class could not be established; the upload is not "
                                        "refused on that ground alone, but it is not silent either." );
}

} // namespace UsbDriverClass
