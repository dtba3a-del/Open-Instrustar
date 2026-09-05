// SPDX-License-Identifier: GPL-2.0-or-later
// Last edited: 2026-08-11 11:31:17 UTC

#include <QDialog>
#include <memory>

class DsoConfigAnalysisPage;
class DsoConfigScopePage;
class DsoConfigColorsPage;
class DsoConfigCtpuMathPage;
class DsoSettings;
class PPresult;

class QHBoxLayout;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QShortcut;
class QStackedWidget;
class QVBoxLayout;

////////////////////////////////////////////////////////////////////////////////
/// \class DsoConfigDialog                                        configdialog.h
/// \brief The dialog for the configuration options.
class DsoConfigDialog : public QDialog {
    Q_OBJECT

  public:
    DsoConfigDialog( DsoSettings *settings, QWidget *parent = nullptr );
    ~DsoConfigDialog() override;

  public slots:
    void accept() override;
    void apply();

    void changePage( QListWidgetItem *current, QListWidgetItem *previous );
    /// \brief Forwards live acquisition data to the CtPU/Math page's "Live"
    /// column while this dialog is open. See DsoConfigCtpuMathPage::updateLiveData().
    void updateLiveData( std::shared_ptr< PPresult > data );

  private:
    void createIcons();

    DsoSettings *settings;

    QVBoxLayout *mainLayout;
    QHBoxLayout *sectionsLayout;
    QHBoxLayout *buttonsLayout;

    QListWidget *contentsWidget;
    QStackedWidget *pagesWidget;

    DsoConfigScopePage *scopePage;
    DsoConfigAnalysisPage *analysisPage;
    DsoConfigColorsPage *colorsPage;
    DsoConfigCtpuMathPage *ctpuMathPage;

    QPushButton *acceptButton, *applyButton, *rejectButton;
    QShortcut *rejectShortcut;
};
