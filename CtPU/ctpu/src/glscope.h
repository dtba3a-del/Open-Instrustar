// SPDX-License-Identifier: GPL-2.0-or-later
// Last edited: 2026-08-19 06:48:51 UTC

#pragma once

#include <list>
#include <memory>

#include <QOpenGLBuffer>

#include <utility>
#include <vector>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLTexture>

#include <QImage>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <QtGlobal>

#include "glscopegraph.h"
#include "hantekprotocol/types.h"
#include "scopesettings.h"

#include <array>

struct DsoSettingsView;
struct DsoSettingsScope;
struct DsoSettingsScopeCursor;
class XYRecorder;
class PPresult;

// Метки диалекта GLSL и исходники слоя камеры — одно место на оба
// потребителя, см. `camerashaders.h`.
#include "camerashaders.h"

/// \brief OpenGL accelerated widget that displays the oscilloscope screen.
class GlScope : public QOpenGLWidget {
    Q_OBJECT

  public:
    static GlScope *createNormal( DsoSettingsScope *scope, DsoSettingsView *view, QWidget *parent = nullptr );
    static GlScope *createZoomed( DsoSettingsScope *scope, DsoSettingsView *view, QWidget *parent = nullptr );

    static void useOpenGLSLversion( QString version = GLSL120 );
    static QString getOpenGLversion();
    static QString getGLSLversion() { return GLSLversion; }
    /**
     * Show new post processed data
     * @param data
     */
    /// \brief Кадр камеры для самого заднего слоя (задание 3 очереди).
    /// Пустой QImage гасит слой. Прозрачность — в `DsoSettingsView`.
    void setCameraFrame( const QImage &frame );

    /// \brief Почему слоя камеры нет, если он не построился.
    ///
    /// Пустая строка — построился. Причина обязана доходить до оператора:
    /// молчаливый отказ шейдера и был тем дефектом, из-за которого кадр не
    /// попадал на холст, а отказ сборки при этом не наступал.
    QString cameraLayerError() const { return m_cameraLayerError; }

    /// \brief Single-curve XY update (legacy, kept for backward compatibility).
    void updateXY( const XYRecorder *recorder );
    /// \brief Multi-curve XY update (TZ §7.5).
    /// Renders up to `DsoSettingsScope::maxXYCurves` trajectories on the same
    /// grid. Each entry of `curves` is a (recorder, config) pair; entries with
    /// `config->enabled == false` are skipped. A null recorder pointer is also
    /// skipped. After all curves are drawn, drawXYLegend() is called.
    void updateXY( const std::array< const XYRecorder *, DsoSettingsScope::maxXYCurves > &recorders,
                   const std::array< const XYCurveConfig *, DsoSettingsScope::maxXYCurves > &configs );
    void showData( std::shared_ptr< PPresult > newData );
    void selectCursor( int index );
    void updateCursor( int index = 0 );
    void generateGrid( int index = -1, double value = 0.0, bool pressed = false );
    void setVisible( bool visible ) override;

  protected:
    /// \brief Initializes the scope widget.
    /// \param settings The settings that should be used.
    /// \param parent The parent widget.
    GlScope( DsoSettingsScope *scope, DsoSettingsView *view, QWidget *parent = nullptr );
    ~GlScope() override;
    GlScope( const GlScope & ) = delete;

    /// \brief Initializes OpenGL output.
    void initializeGL() override;

    /// \brief Draw the graphs, marker and the grid.
    void paintGL() override;

    /// \brief Resize the widget.
    /// \param width The new width of the widget.
    /// \param height The new height of the widget.
    void resizeGL( int width, int height ) override;

    void mousePressEvent( QMouseEvent *event ) override;
    void mouseMoveEvent( QMouseEvent *event ) override;
    void mouseReleaseEvent( QMouseEvent *event ) override;
    void mouseDoubleClickEvent( QMouseEvent *event ) override;
    void wheelEvent( QWheelEvent *event ) override;
    void paintEvent( QPaintEvent *event ) override;

    /// \brief Draw the grid.
    void drawGrid();
    /// Draw vertical lines at marker positions
    void drawMarkers();
    void generateVertices( int marker, const DsoSettingsScopeCursor &cursor );
    void drawVertices( QOpenGLFunctions *gl, int marker, QColor color );

    void drawVoltageChannelGraph( ChannelID channel, Graph &graph, int historyIndex );
    void drawHistogramChannelGraph( ChannelID channel, Graph &graph, int historyIndex );
    void drawSpectrumChannelGraph( ChannelID channel, Graph &graph, int historyIndex );
    /// \brief Draw one frame-XY trajectory, indexed by curve slot (TZ §7.1).
    void drawXYCurveGraph( int curve, Graph &graph, int historyIndex );
    /// \brief Draw the XY multi-curve legend (TZ §7.2).
    /// Called from paintGL() after all curves have been rendered. Uses
    /// QPainter on top of the OpenGL surface; one line per enabled curve,
    /// formatted as `Name(x=<gainX>/div; y=<gainY>/div)`.
    void drawXYLegend();
    QPointF posToScopePos( QPointF pos );
    void rightMouseEvent( QMouseEvent *event );

  signals:
    void markerMoved( int cursorIndex, int marker );
    void cursorMeasurement( QPointF measurePosition = QPointF(), QPoint globalPosition = QPoint(), bool status = false );

  private:
    // User settings
    DsoSettingsScope *scope;
    DsoSettingsView *view;
    bool zoomed = false;

    // Marker
    const int NO_MARKER = INT_MAX;
#pragma pack( push, 1 )
    struct Vertices {
        QVector3D a, b, c, d;
    };
#pragma pack( pop )
    const int VERTICES_ARRAY_SIZE = sizeof( Vertices ) / sizeof( QVector3D );
    std::vector< Vertices > vaMarker;
    int selectedMarker = NO_MARKER;
    QOpenGLBuffer m_marker;
    QOpenGLVertexArrayObject m_vaoMarker;

    // Cursors
    std::vector< DsoSettingsScopeCursor * > cursorInfo;
    int selectedCursor = 0;
    bool rightMouseInside = false;

    // Grid
    QOpenGLBuffer m_grid;
    static const int gridItems = 4;
    QOpenGLVertexArrayObject m_vaoGrid[ gridItems ];
    GLsizei gridDrawCounts[ gridItems ];
    void draw4Cross( std::vector< QVector3D > &va, int section, float x, float y );
    QColor triggerLineColor = QColor( "black" );

    // Graphs
    std::list< Graph > m_GraphHistory;

    // XY Recorder rendering
    // [MOD] TZ §7.5 — multi-curve: one VAO/VBO per curve slot.
    QOpenGLBuffer m_xyBuffer[ DsoSettingsScope::maxXYCurves ];
    QOpenGLVertexArrayObject m_vaoXY[ DsoSettingsScope::maxXYCurves ];
    int xyPointCount[ DsoSettingsScope::maxXYCurves ] = { 0 };
    /// Отрезки кривой XY: {первая вершина, сколько}. Ломаная рвётся на
    /// границах пакетов — между ними сигнала не было, и соединять их прямой
    /// значит рисовать несуществующую связь.
    std::vector< std::pair< int, int > > xySegments[ DsoSettingsScope::maxXYCurves ];
    /// \brief Cached XY curve configs from the last updateXY() call (TZ §7.2).
    /// Used by drawXYLegend() to render the legend with per-curve gain info.
    std::array< XYCurveConfig, DsoSettingsScope::maxXYCurves > m_xyCurveConfigs;
    bool m_xyCurveConfigsValid = false;
    unsigned currentGraphInHistory = 0;

    // OpenGL shader, matrix, var-locations
    static QString OpenGLversion;
    static QString GLSLversion;
    QString renderInfo;
    bool shaderCompileSuccess = false;
    QString errorMessage;
    std::unique_ptr< QOpenGLShaderProgram > m_program;

    /// \name Задание 3 очереди: кадр камеры самым задним слоем
    ///
    /// Отдельная программа нужна потому, что основная красит сплошным цветом,
    /// а кадру нужна выборка из текстуры. Прозрачность задаётся снаружи:
    /// слой обязан быть виден СКВОЗЬ всё, что нарисовано выше.
    ///@{
    std::unique_ptr< QOpenGLShaderProgram > m_cameraProgram;
    std::unique_ptr< QOpenGLTexture > m_cameraTexture;
    QOpenGLVertexArrayObject m_vaoCamera;
    QOpenGLBuffer m_cameraQuad;
    int m_cameraVertexLoc = -1;
    int m_cameraTexCoordLoc = -1;
    int m_cameraAlphaLoc = -1;
    int m_cameraMatrixLoc = -1;
    int m_cameraFitLoc = -1;
    /// Доля холста под кадр: пропорции кадра сохраняются, растяжения нет.
    QSizeF m_cameraFit = QSizeF( 1.0, 1.0 );
    /// Размер кадра ПОСЛЕ уменьшения до холста — от него считается доля.
    QSize m_cameraFrameSize;
    /// Предел драйвера на сторону текстуры; 0 — ещё не спрошен.
    GLint m_cameraMaxTextureSize = 0;
    /// Почему слоя нет, если он не собрался. Пустая строка — собрался.
    QString m_cameraLayerError;
    void drawCameraLayer();
    QSize canvasPixelSize() const;
    ///@}
    QMatrix4x4 pmvMatrix; ///< projection, view matrix
    int colorLocation;
    int vertexLocation;
    int matrixLocation;
    int selectionLocation;
};
