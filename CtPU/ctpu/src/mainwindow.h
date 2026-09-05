// SPDX-License-Identifier: GPL-2.0-or-later
// Last edited: 2026-08-11 11:31:17 UTC

#pragma once
#include "post/ppresult.h"
#include <QElapsedTimer>
#include <QLineEdit>
#include <QMainWindow>
#include <QPointer>
#include <memory>

#include "scopesettings.h"

class SpectrumGenerator;
class HantekDsoControl;
class DsoSettings;
class ExporterRegistry;
class DsoWidget;
class HorizontalDock;
class TriggerDock;
class SpectrumDock;
class VoltageDock;
class DsoConfigDialog;

namespace Ui {
class MainWindow;
}

/// \brief The main window of the application.
/// The main window contains the classic oszilloscope-screen and the gui
/// elements used to control the oszilloscope.
class MainWindow : public QMainWindow {
    Q_OBJECT

  public:
    explicit MainWindow( HantekDsoControl *dsoControl, DsoSettings *dsoSettings, ExporterRegistry *exporterRegistry,
                         QWidget *parent = nullptr );
    ~MainWindow() override;
    QElapsedTimer elapsedTime;

  public slots:
    void showNewData( std::shared_ptr< PPresult > newData );
    void exporterStatusChanged( const QString &exporterName, const QString &status );
    void exporterProgressChanged();

  private slots:
    /// \brief Renders the newest pending frame (mailbox-1 consumer, D-05).
    void renderPendingData();

  protected:
    void closeEvent( QCloseEvent *event ) override;

  private:
    Ui::MainWindow *ui;
    QIcon iconPause;
    QIcon iconPlay;
    QLineEdit *commandEdit;
    QString lastSaveAsDir = "";
    /// Окно камеры. Создаётся при первом обращении и живёт дальше: правило
    /// GUI-FIRST §4а - один значок открывает одно и то же окно, а не новое
    /// каждый раз.
    class CameraDialog *cameraDialog = nullptr;

    /// Задняя сторона окна генератора — путь A через вендорскую vdso.dll.
    /// Создаётся при первом открытии окна и живёт до конца работы: подъём
    /// библиотеки стоит секунду на перечисление USB.
    std::unique_ptr< class VdsoBackend > ddsBackend;

    // Central widgets
    DsoWidget *dsoWidget;
    /// \brief Tracks the currently-open Settings dialog, if any, so
    /// showNewData() can forward live samples to its CtPU/Math "Live"
    /// column. QPointer auto-nulls when the dialog is closed/destroyed, so
    /// no explicit cleanup is needed here.
    QPointer< DsoConfigDialog > openConfigDialog;

    /// \brief Mailbox-1 display slot (D-05, REALTIME-FEEL.md): holds the
    /// newest frame not yet rendered. A new frame REPLACES an unrendered
    /// one instead of queueing behind it, so display latency is bounded by
    /// one render, never by queue depth. Data-bearing consumers (XY/BinTape
    /// recorders) are fed per-frame in showNewData() BEFORE displacement —
    /// only pixels are coalesced, never data. Non-empty also means a
    /// renderPendingData() call is already queued. GUI-thread only.
    std::shared_ptr< PPresult > pendingRenderData;

    // Settings used for the whole program
    DsoSettings *dsoSettings;
    ExporterRegistry *exporterRegistry;

    // Taking screenshots
    enum screenshotType_t { SCREENSHOT, HARDCOPY, PRINTER };
    screenshotType_t screenshotType;
    void screenShot( screenshotType_t screenshotType = SCREENSHOT, bool autoSave = false );

    /// \brief Сохранить кадр камеры или холст целиком (две кнопки камеры).
    ///
    /// Распоряжение автора 2026-09-02: «фото с камеры сохраняется в 2х
    /// вариантах: чистое фото и эта область, где под сеткой находится слой
    /// изображения камеры». Два варианта - две кнопки и два имени файла,
    /// чтобы снимки не путались между собой.
    /// \param wholeCanvas false - чистый кадр камеры; true - холст со всеми
    ///        слоями в экранном порядке.
    void saveCameraImage( bool wholeCanvas );

    bool openDocument( QString docName );

  signals:
    void settingsLoaded( DsoSettingsScope *scope, const Dso::ControlSpecification *spec );
};
