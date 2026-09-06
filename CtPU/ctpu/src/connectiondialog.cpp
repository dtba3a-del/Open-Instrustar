// SPDX-License-Identifier: GPL-3.0-or-later

#include "connectiondialog.h"

#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QMessageBox>
#include <QProcess>
#include <QTimer>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>

#include <memory>

#include "hantekdso/hantekdsocontrol.h"
#include "hantekdso/controlspecification.h"
#include "dsomodel.h"
#include "selectdevice/selectsupporteddevice.h"
#include "usb/scopedevice.h"

#ifdef Q_OS_FREEBSD
#include <libusb.h>
#else
#include <libusb-1.0/libusb.h>
#endif


ConnectionDialog::ConnectionDialog( HantekDsoControl *dsoControl, QWidget *parent )
    : QDialog( parent ), dsoControl( dsoControl ) {
    setWindowTitle( tr( "Connection" ) );
    resize( 640, 480 );

    auto *layout = new QVBoxLayout( this );
    info = new QTextBrowser( this );
    info->setOpenExternalLinks( true );
    layout->addWidget( info );

    auto *buttons = new QDialogButtonBox( QDialogButtonBox::Close, this );

    // Кнопка выбора устройства. Она здесь, а не при запуске программы, по
    // распоряжению автора: конфигурировать оборудование программа вправе
    // только тогда, когда оператор этого потребовал.
    QPushButton *choose = buttons->addButton( tr( "Select device .." ), QDialogButtonBox::ActionRole );
    choose->setToolTip( tr( "Open the device list. Nothing is written to any instrument by opening it; "
                            "firmware upload is a separate button there. Choosing a device restarts the "
                            "application on it." ) );
    connect( choose, &QPushButton::clicked, this, &ConnectionDialog::chooseDevice );

    QPushButton *rescan = buttons->addButton( tr( "Refresh" ), QDialogButtonBox::ActionRole );
    connect( rescan, &QPushButton::clicked, this, &ConnectionDialog::refresh );
    connect( buttons, &QDialogButtonBox::rejected, this, &QDialog::reject );
    layout->addWidget( buttons );

    refresh();
}


void ConnectionDialog::chooseDevice() {
    // Свой контекст libusb, как и у опроса шины: окно не имеет права трогать
    // контекст работающего конвейера.
    libusb_context *ctx = nullptr;
    if ( libusb_init( &ctx ) != LIBUSB_SUCCESS ) {
        QMessageBox::warning( this, tr( "Select device" ),
                              tr( "libusb did not start, the bus cannot be asked." ) );
        return;
    }

    std::unique_ptr< ScopeDevice > chosen;
    {
        SelectSupportedDevice selector( this );
        // autoConnect = false: окно открыто ПО ТРЕБОВАНИЮ оператора, и
        // захлопываться само, едва увидев прибор, оно не должно — оператор
        // мог прийти сюда именно затем, чтобы записать прошивку в другой.
        chosen = selector.showSelectDeviceModal( ctx, 0, false, true );
    }

    if ( !chosen || chosen->isDemoDevice() ) {
        libusb_exit( ctx );
        refresh();
        return;
    }

    const QString name = chosen->getModel() ? chosen->getModel()->name : tr( "device" );
    chosen.reset(); // отпустить прибор до перезапуска, иначе новый процесс его не откроет
    libusb_exit( ctx );

    const auto answer = QMessageBox::question(
        this, tr( "Restart on the selected device" ),
        tr( "<p>Selected: <b>%1</b>.</p>"
            "<p>The instrument is bound when the acquisition pipeline is built, so the "
            "application will restart to work with it. Unsaved settings of the current "
            "session are kept in the configuration file as usual.</p>"
            "<p>Restart now?</p>" )
            .arg( name.toHtmlEscaped() ),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes );
    if ( answer != QMessageBox::Yes ) {
        refresh();
        return;
    }

    // Перезапуск: новый процесс сам найдёт этот прибор молчаливым опросом —
    // он теперь единственный готовый на шине.
    const QString program = QCoreApplication::applicationFilePath();
    QStringList args = QCoreApplication::arguments();
    if ( !args.isEmpty() )
        args.removeFirst();
    if ( !QProcess::startDetached( program, args ) ) {
        QMessageBox::warning( this, tr( "Restart failed" ),
                              tr( "The application could not start a new instance. Close it and start "
                                  "it again by hand to use the selected instrument." ) );
        return;
    }
    // Сначала закрыть это окно, и только потом гасить программу: `quit()`,
    // вызванный из вложенного цикла модального диалога, до главного цикла
    // дойдёт лишь после его завершения, и порядок здесь не косметический.
    accept();
    QTimer::singleShot( 0, qApp, []() { QCoreApplication::quit(); } );
}


QString ConnectionDialog::usbScanHtml() const {
    // Опрос идёт в своём контексте libusb и его же закрывает: окно не имеет
    // права трогать контекст рабочего конвейера, а второй контекст в одном
    // процессе libusb допускает.
    QString html = tr( "<h3>USB bus &mdash; what is on it right now</h3>" );

    libusb_context *ctx = nullptr;
    if ( libusb_init( &ctx ) != LIBUSB_SUCCESS ) {
        return html + tr( "<p><b>libusb did not start.</b> The bus cannot be asked; "
                          "everything below this line is from the moment the program "
                          "started, not from now.</p>" );
    }

    libusb_device **list = nullptr;
    const ssize_t count = libusb_get_device_list( ctx, &list );
    if ( count < 0 ) {
        libusb_exit( ctx );
        return html + tr( "<p><b>The device list could not be read</b> (%1).</p>" )
                          .arg( QString::fromUtf8( libusb_error_name( int( count ) ) ) );
    }

    // Идентификаторы прибора в двух его состояниях. Это НЕ перенос факта
    // между стеками: оба числа принадлежат одному прибору и различают
    // именно то, в каком он сейчас состоянии.
    //   d4a2:566x — постоянная прошивка вендора (состояние пути A);
    //   1d50:608e — прибор уже перечислился заново с fx2lafw (путь B).
    int found = 0;
    QString rows;
    for ( ssize_t i = 0; i < count; ++i ) {
        libusb_device_descriptor d;
        if ( libusb_get_device_descriptor( list[ i ], &d ) != LIBUSB_SUCCESS )
            continue;
        const bool vendorState = ( d.idVendor == 0xd4a2 );
        const bool ourState = ( d.idVendor == 0x1d50 && d.idProduct == 0x608e );
        if ( !vendorState && !ourState )
            continue;
        ++found;
        rows += tr( "<tr><td><tt>%1:%2</tt></td><td>bus %3, address %4</td><td>%5</td></tr>" )
                    .arg( d.idVendor, 4, 16, QChar( '0' ) )
                    .arg( d.idProduct, 4, 16, QChar( '0' ) )
                    .arg( libusb_get_bus_number( list[ i ] ) )
                    .arg( libusb_get_device_address( list[ i ] ) )
                    .arg( ourState ? tr( "renumerated with fx2lafw &mdash; <b>this is path B, "
                                         "the application's own path</b>" )
                                   : tr( "permanent vendor firmware &mdash; <b>path A state</b>" ) );
    }

    html += tr( "<p>Devices visible to libusb: <b>%1</b>; of them InstruStar: <b>%2</b>.</p>" )
                .arg( qlonglong( count ) )
                .arg( found );
    if ( found )
        html += QStringLiteral( "<table cellpadding='4' border='1' cellspacing='0'>" ) +
                tr( "<tr><th>VID:PID</th><th>Where</th><th>State</th></tr>" ) + rows +
                QStringLiteral( "</table>" );

    // Разбор случая, который иначе выглядит как «программа сломалась».
    html += tr( "<h4>How to read this</h4><ul>"
                "<li><b><tt>1d50:608e</tt> present</b> &mdash; the instrument has already "
                "renumerated with fx2lafw; this application can work with it.</li>"
                "<li><b><tt>d4a2:566x</tt> present</b> &mdash; the instrument answers with its "
                "permanent firmware. Firmware cannot be pushed into it while the vendor driver "
                "holds it: that fails in <i>every</i> application, the vendor's own included. "
                "The driver decides the path, not the program.</li>"
                "<li><b>Nothing listed</b> &mdash; the bus gives this application nothing. On "
                "Windows that is what a driver outside the WinUSB/libusbK family looks like from "
                "here: the instrument may be perfectly alive and working in the vendor software "
                "at the same moment.</li>"
                "</ul>"
                "<p>Switching the driver is <b>not symmetric</b>: replacing the vendor driver "
                "with WinUSB through Zadig is a few clicks, restoring the vendor driver is "
                "markedly harder. Establish which path is wanted <i>before</i> touching the "
                "driver &mdash; <tt>docs/ACCESS_PATHS.md</tt>.</p>" );

    libusb_free_device_list( list, 1 );
    libusb_exit( ctx );
    return html;
}


void ConnectionDialog::refresh() {
    QString html;

    // Настоящий опрос шины идёт первым: именно за ним нажимают «Refresh».
    html += usbScanHtml();

    // --- 1. Что подключено сейчас: только измеримые факты ---------------
    html += tr( "<h3>Current connection</h3>" );
    if ( dsoControl && dsoControl->getDevice() ) {
        const ScopeDevice *dev = dsoControl->getDevice();
        const DSOModel *model = dsoControl->getModel();
        html += QStringLiteral( "<table cellpadding='3'>" );
        html += tr( "<tr><td><b>Device</b></td><td>%1</td></tr>" ).arg( model ? model->name : tr( "unknown" ) );
        html += tr( "<tr><td><b>Mode</b></td><td>%1</td></tr>" )
                    .arg( dev->isRealHW() ? tr( "real hardware" ) : tr( "demo (no device)" ) );
        if ( dev->isRealHW() ) {
            html += tr( "<tr><td><b>Firmware</b></td><td>0x%1</td></tr>" )
                        .arg( dev->getFwVersion(), 4, 16, QChar( '0' ) );
            html += tr( "<tr><td><b>USB VID:PID</b></td><td>%1:%2</td></tr>" )
                        .arg( model ? model->vendorID : 0, 4, 16, QChar( '0' ) )
                        .arg( model ? model->productID : 0, 4, 16, QChar( '0' ) );
        }
        html += tr( "<tr><td><b>Transport</b></td><td>USB &mdash; fx2lafw firmware + libusb "
                    "(<i>path B</i>)</td></tr>" );
        html += QStringLiteral( "</table>" );
    } else {
        html += tr( "<p>No device object &mdash; the application is not connected.</p>" );
    }

    // --- 2. Транспорты: реализовано / не реализовано --------------------
    html += tr( "<h3>Transports</h3>"
                "<table cellpadding='4' border='1' cellspacing='0'>"
                "<tr><th>Transport</th><th>Device</th><th>State</th></tr>"

                "<tr><td>USB, fx2lafw + libusb</td><td>InstruStar ISDS205B</td>"
                "<td><b>works</b> &mdash; this is the application's own path (B)</td></tr>"

                "<tr><td>USB, vendor <tt>vdso.dll</tt></td><td>InstruStar (all models)</td>"
                "<td><i>not used by design</i> &mdash; vendor stack (path A) belongs to the "
                "Python prototype; facts from A are never transferred to B</td></tr>"

                "<tr><td>COM / Bluetooth, OBEX frames; <b>also</b> uart_over_USB</td><td>Oscill</td>"
                "<td><b>not implemented</b> &mdash; protocol studied and written down, "
                "code not written</td></tr>"

                // Е7-12 — распоряжение автора 2026-08-30: поставить рядом с
                // остальными, без кода, и обозначить, что прорабатывается.
                "<tr><td>GPIB (&#1050;&#1054;&#1055;, &#1043;&#1054;&#1057;&#1058; 26.003-80) "
                "via a GPIB&ndash;USB adapter</td><td>&#1045;7-12 immittance meter</td>"
                "<td><b>not implemented</b> &mdash; to be worked through; the instrument has no "
                "USB of its own, the adapter carries the bus</td></tr>"

                // Заявка автора 2026-09-02: приборы поставлены в список, чтобы
                // они были видны рядом с остальными. Расписывать их здесь
                // нечего - дополнительные сведения у автора, задача будет
                // поставлена отдельно. Пустая строка честнее выдуманной.
                // Транспорты у этих четырёх — оба пути сразу (поправка автора
                // 2026-09-02). Это единственное место в приложении, где путь A
                // и путь B стоят у одного прибора; правило `ACCESS_PATHS.md`
                // при этом не нарушено — оно запрещает ПЕРЕНОС фактов между
                // путями, а не наличие двух путей к прибору.
                "<tr><td>USB, fx2lafw + libusb<br/><b>or</b> USB, vendor <tt>vdso.dll</tt></td>"
                "<td>&#1051;2-56</td>"
                "<td><i>declared</i> &mdash; helper/assistant</td></tr>"

                "<tr><td>USB, fx2lafw + libusb<br/><b>or</b> USB, vendor <tt>vdso.dll</tt></td>"
                "<td>Tektronix 577</td>"
                "<td><i>declared</i> &mdash; helper/assistant</td></tr>"

                "<tr><td>USB, fx2lafw + libusb<br/><b>or</b> USB, vendor <tt>vdso.dll</tt></td>"
                "<td>C-V meter A</td>"
                "<td><i>declared</i> &mdash; helper/assistant</td></tr>"

                "<tr><td>USB, fx2lafw + libusb<br/><b>or</b> USB, vendor <tt>vdso.dll</tt></td>"
                "<td>C-V meter B</td>"
                "<td><i>declared</i> &mdash; helper/assistant</td></tr>"

                "<tr><td>uart_over_USB</td><td>VO2ATAS</td>"
                "<td><i>declared</i> &mdash; known command_set uart/USB</td></tr>"

                "<tr><td>uart_over_USB</td><td>Multimeters</td>"
                "<td><i>declared</i> &mdash; known command_set uart/USB</td></tr>"

                "<tr><td>uart_over_USB</td><td>RCLmeters DE5000 compatible</td>"
                "<td><i>declared</i> &mdash; known command_set uart/USB</td></tr>"

                "</table>"
                "<p><i>&laquo;declared&raquo; means the row exists because the author put the instrument on the "
                "list; no transport is written and none is claimed.</i></p>" );

    // --- 2а. Е7-12: что уже установлено, чтобы строка выше не была пустой ---
    html += tr( "<h3>&#1045;7-12 &mdash; what is already known</h3>"
                "<ul>"
                "<li>Bus: &#1050;&#1054;&#1055; (GOST 26.003-80, the IEC-625 / IEEE-488 family), "
                "reached through a GPIB&ndash;USB adapter &mdash; the meter itself has no USB.</li>"
                "<li><b>The interface has no notion of time.</b> Every exchange is handshaked by "
                "bus signals; a timeout or a delay in a driver would assume time where there is "
                "none. The adapter carries the handshake, it does not replace it.</li>"
                "<li>No processor inside: the measurement cycle is a fixed 32-step program in the "
                "clock generator ROM (2.724.011 &#1058;&#1054;, &sect;4.2.15).</li>"
                "<li>A byte is a front-panel <i>key</i>, not a command with parameters.</li>"
                "<li>The result is a fixed 11-byte message carrying both quantities: the meter has "
                "two computers, so the companion parameter (tg&delta;, Q) arrives with the main "
                "one.</li>"
                "<li>Capability in the tree: <tt>PanelInstrument</tt> &mdash; key, display, bus "
                "event. Notes: <tt>docs/INSTRUMENT-TREE.md</tt>.</li>"
                "</ul>"
                "<p><i>Honest status: the row above exists so the instrument is visible in the "
                "list; the transport is not written yet.</i></p>" );

    // --- 3. Что именно известно про Oscill: без этого пункт 2 пустой ----
    html += tr( "<h3>Oscill &mdash; what is already known</h3>"
                "<ul>"
                "<li>Link: OBEX frames over a COM port or Bluetooth serial; the vendor tool "
                "(<i>Oscilink</i>) also offers a USB driver choice (Auto / Ver.2 / Ver.3).</li>"
                "<li>Properties and registers: MC 50/100 MHz, time unit 10 ps; TS/RS/AP/QS/TD; "
                "RIS sampling; roll mode; in-instrument averaging up to 256 sweeps; "
                "sensitivity 20 mV/div .. 10 V/div.</li>"
                "<li>Analog front end is better thought out than ISDS205: hardware trigger "
                "(comparator + DAC1), hardware offset (DAC0), GND position in the attenuator, "
                "bandwidth independent of the range.</li>"
                "<li>Sources in the repository: <tt>references/vendor/oscill/NOTES.md</tt>, "
                "<tt>analog-part-vs-isds205.md</tt>, <tt>gui-live-study.md</tt>, and the full "
                "vendor distribution in <tt>references/vendor/oscill/distrib/</tt>.</li>"
                "</ul>"
                "<p><i>Honest status: the button and this window exist so the operator can see "
                "the connection; they do not pretend that the Oscill transport is working.</i></p>" );

    info->setHtml( html );
}
