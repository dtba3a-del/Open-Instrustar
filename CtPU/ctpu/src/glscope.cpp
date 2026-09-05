// SPDX-License-Identifier: GPL-2.0-or-later
// Last edited: 2026-08-19 06:48:51 UTC

#include <cmath>

#include <QColor>
#include <QCoreApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QMatrix4x4>
#include <QMessageBox>
#include <QMouseEvent>
#include <QOpenGLShaderProgram>
#include <QPainter>

#include <QOffscreenSurface>
#include <QOpenGLFunctions>

#include "glscope.h"
#include "xyrecorder.h"

#include "post/graphgenerator.h"
#include "post/ppresult.h"
#include "scopesettings.h"
#include "viewconstants.h"
#include "viewsettings.h"


// static info strings
QString GlScope::OpenGLversion;
QString GlScope::GLSLversion;


QString GlScope::getOpenGLversion() {
    if ( OpenGLversion.isNull() ) {
        QOffscreenSurface surface;
        surface.create();
        QOpenGLContext context;
        context.create();
        context.makeCurrent( &surface );
        OpenGLversion = reinterpret_cast< const char * >( context.functions()->glGetString( GL_VERSION ) );
        surface.destroy();
    }
    return OpenGLversion;
}


// this static function will be called early from main to set up OpenGL
void GlScope::useOpenGLSLversion( QString renderer ) {
    QSurfaceFormat format;
    GLSLversion = renderer;
    format.setSamples( 4 );
    format.setProfile( QSurfaceFormat::CoreProfile );
    if ( renderer == GLES100 ) {
        format.setRenderableType( QSurfaceFormat::OpenGLES );
    } else {
        format.setRenderableType( QSurfaceFormat::OpenGL );
    }
    QSurfaceFormat::setDefaultFormat( format );
}


GlScope::GlScope( DsoSettingsScope *scope, DsoSettingsView *view, QWidget *parent )
    : QOpenGLWidget( parent ), scope( scope ), view( view ) {
    if ( scope->verboseLevel > 1 )
        qDebug() << " GLScope::GLScope()";
    cursorInfo.clear();
    cursorInfo.push_back( &scope->horizontal.cursor );
    selectedCursor = 0;
    for ( ChannelID channel = 0; channel < scope->voltage.size(); ++channel ) {
        cursorInfo.push_back( &scope->voltage[ channel ].cursor );
    }
    for ( ChannelID channel = 0; channel < scope->spectrum.size(); ++channel ) {
        cursorInfo.push_back( &scope->spectrum[ channel ].cursor );
    }
    vaMarker.resize( cursorInfo.size() );
}


GlScope::~GlScope() {
    if ( scope->verboseLevel > 1 )
        qDebug() << " GLScope::~GLScope()";
}


// static
GlScope *GlScope::createNormal( DsoSettingsScope *scope, DsoSettingsView *view, QWidget *parent ) {
    GlScope *s = new GlScope( scope, view, parent );
    s->zoomed = false;
    return s;
}


// static
GlScope *GlScope::createZoomed( DsoSettingsScope *scope, DsoSettingsView *view, QWidget *parent ) {
    GlScope *s = new GlScope( scope, view, parent );
    s->zoomed = true;
    return s;
}


void GlScope::setVisible( bool visible ) {
    if ( !visible && rightMouseInside ) {
        QGuiApplication::restoreOverrideCursor();
        emit cursorMeasurement();
    }
    rightMouseInside = false;
    QWidget::setVisible( visible );
}


// convert widget position ([0..width()], [0..height()]) to scope-div values (x=[-5..5], y=[4..-4])
QPointF GlScope::posToScopePos( QPointF pos ) {
    QPointF position( ( pos.x() - double( width() ) / 2.0 ) * DIVS_TIME / double( width() ),
                      ( double( height() ) / 2.0 - pos.y() ) * DIVS_VOLTAGE / double( height() ) );
    if ( zoomed ) {
        double m1 = scope->getMarker( 0 );
        double m2 = scope->getMarker( 1 );
        if ( m1 > m2 )
            std::swap( m1, m2 );
        position.setX( m1 + ( 0.5 + ( position.x() / DIVS_TIME ) ) * ( m2 - m1 ) );
    }
    return position;
}


void GlScope::rightMouseEvent( QMouseEvent *event ) {
    if ( rect().contains( event->pos() ) ) {
        if ( !rightMouseInside ) {
            QGuiApplication::setOverrideCursor( QCursor( Qt::CrossCursor ) );
        }
        rightMouseInside = true;
        emit cursorMeasurement( posToScopePos( event->pos() ), event->globalPos(), true );
    } else {
        if ( rightMouseInside ) {
            QGuiApplication::restoreOverrideCursor();
        }
        rightMouseInside = false;
        emit cursorMeasurement();
    }
}


void GlScope::mousePressEvent( QMouseEvent *event ) {
    QPointF position = posToScopePos( event->pos() );
    if ( scope->verboseLevel > 3 )
        qDebug() << "   GLS::mPE()" << event;
    if ( !( zoomed && selectedCursor == 0 ) && event->button() == Qt::LeftButton ) {
        selectedMarker = NO_MARKER;
        DsoSettingsScopeCursor *cursor = cursorInfo[ size_t( selectedCursor ) ];
        double dX0 = fabs( cursor->pos[ 0 ].x() - position.x() );
        double dX1 = fabs( cursor->pos[ 1 ].x() - position.x() );
        double dY0 = fabs( cursor->pos[ 0 ].y() - position.y() );
        double dY1 = fabs( cursor->pos[ 1 ].y() - position.y() );

        switch ( cursor->shape ) {
        case DsoSettingsScopeCursor::RECTANGULAR:
            if ( std::min( dX0, dX1 ) < 1.0 / DIVS_SUB && std::min( dY0, dY1 ) < 1.0 / DIVS_SUB ) {
                if ( ( dX0 < dX1 && dY0 > dY1 ) || ( dX0 > dX1 && dY0 < dY1 ) ) {
                    std::swap( cursor->pos[ 0 ].ry(), cursor->pos[ 1 ].ry() );
                }
                selectedMarker = ( dX0 < dX1 ) ? 0 : 1;
            }
            break;
        case DsoSettingsScopeCursor::VERTICAL:
            if ( dX0 < dX1 ) {
                if ( dX0 < 1.0 / DIVS_SUB )
                    selectedMarker = 0;
            } else {
                if ( dX1 < 1.0 / DIVS_SUB )
                    selectedMarker = 1;
            }
            break;
        case DsoSettingsScopeCursor::HORIZONTAL:
            if ( dY0 < dY1 ) {
                if ( dY0 < 1.0 / DIVS_SUB )
                    selectedMarker = 0;
            } else {
                if ( dY1 < 1.0 / DIVS_SUB )
                    selectedMarker = 1;
            }
            break;
        case DsoSettingsScopeCursor::NONE:
            break;
        }
        if ( selectedMarker != NO_MARKER ) {
            cursorInfo[ size_t( selectedCursor ) ]->pos[ size_t( selectedMarker ) ] = position;
            if ( selectedCursor == 0 )
                emit markerMoved( selectedCursor, selectedMarker );
        }
    } else if ( ( event->buttons() & Qt::RightButton ) )
        rightMouseEvent( event );
    event->accept();
}


void GlScope::mouseMoveEvent( QMouseEvent *event ) {
    QPointF position = posToScopePos( event->pos() );
    if ( scope->verboseLevel > 3 )
        qDebug() << "   GLS::mME()" << event << position;
    if ( !( zoomed && selectedCursor == 0 ) && ( event->buttons() & Qt::LeftButton ) != 0 ) {
        if ( selectedMarker == NO_MARKER ) {
            for ( int marker = 0; marker < 2; ++marker ) {
                cursorInfo[ size_t( selectedCursor ) ]->pos[ marker ] = position;
                emit markerMoved( selectedCursor, marker );
                selectedMarker = marker;
            }
        } else if ( selectedMarker < 2 ) {
            cursorInfo[ size_t( selectedCursor ) ]->pos[ selectedMarker ] = position;
            emit markerMoved( selectedCursor, selectedMarker );
        }
    } else if ( event->buttons() & Qt::RightButton )
        rightMouseEvent( event );
    event->accept();
}


void GlScope::mouseReleaseEvent( QMouseEvent *event ) {
    if ( scope->verboseLevel > 3 )
        qDebug() << "   GLS::mRE()" << event;
    if ( !( zoomed && selectedCursor == 0 ) && event->button() == Qt::LeftButton ) {
        QPointF position = posToScopePos( event->pos() );
        if ( selectedMarker < 2 ) {
            cursorInfo[ size_t( selectedCursor ) ]->pos[ selectedMarker ] = position;
            emit markerMoved( selectedCursor, selectedMarker );
        }
        selectedMarker = NO_MARKER;
    }
    if ( rightMouseInside ) {
        QGuiApplication::restoreOverrideCursor();
        emit cursorMeasurement();
    }
    rightMouseInside = false;
    event->accept();
}


void GlScope::mouseDoubleClickEvent( QMouseEvent *event ) {
    if ( scope->verboseLevel > 3 )
        qDebug() << "   GLS::mDCE()" << event;
    if ( !( zoomed && selectedCursor == 0 ) && ( event->buttons() & Qt::LeftButton ) != 0 ) {
        QPointF position = posToScopePos( event->pos() );
        if ( selectedMarker == NO_MARKER ) {
            QPointF p = QPointF( 0.5, 0 );
            if ( event->modifiers() & Qt::CTRL )
                p /= 10;
            if ( event->modifiers() & Qt::SHIFT )
                position = QPointF( 10 * scope->trigger.position - 5, 0 );
            cursorInfo[ size_t( selectedCursor ) ]->pos[ 0 ] = position - p;
            emit markerMoved( selectedCursor, 0 );
            cursorInfo[ size_t( selectedCursor ) ]->pos[ 1 ] = position + p;
            emit markerMoved( selectedCursor, 1 );
            selectedMarker = NO_MARKER;
        }
    } else if ( !( zoomed && selectedCursor == 0 ) && ( event->buttons() & Qt::RightButton ) != 0 ) {
        cursorInfo[ size_t( selectedCursor ) ]->pos[ 0 ] = QPointF( MARGIN_LEFT, 0 );
        cursorInfo[ size_t( selectedCursor ) ]->pos[ 1 ] = QPointF( MARGIN_RIGHT, 0 );
        emit markerMoved( selectedCursor, 0 );
        emit markerMoved( selectedCursor, 1 );
    }
    event->accept();
}


void GlScope::wheelEvent( QWheelEvent *event ) {
    if ( scope->verboseLevel > 3 )
        qDebug() << "   GLS::wE()" << event;
    if ( selectedMarker == NO_MARKER ) {
        double step = event->angleDelta().y() / 1200.0;
        double &m1 = cursorInfo[ size_t( selectedCursor ) ]->pos[ 0 ].rx();
        double &m2 = cursorInfo[ size_t( selectedCursor ) ]->pos[ 1 ].rx();
        if ( m1 > m2 )
            std::swap( m1, m2 );
        double dm = m2 - m1;
        if ( event->modifiers() & Qt::CTRL ) {
            if ( ( step > 0 && dm <= 1 ) || ( step < 0 && dm <= 0.99 ) )
                step *= 0.1;
            if ( step < 0 || dm >= 5 * step ) {
                m1 += step;
                m2 -= step;
            } else {
                double mm = ( m1 + m2 ) / 2;
                m1 = mm - 0.01;
                m2 = mm + 0.01;
            }
        } else {
            if ( step < 0 )
                step = qMax( step, MARGIN_LEFT - m1 );
            else
                step = qMin( step, MARGIN_RIGHT - m2 );
            if ( event->modifiers() & Qt::SHIFT ) {
                if ( m2 - m1 < .1 )
                    step *= ( m2 - m1 );
                else
                    step *= 0.1;
            }
            m1 += step;
            m2 += step;
        }

        emit markerMoved( selectedCursor, 0 );
        emit markerMoved( selectedCursor, 1 );
        selectedMarker = NO_MARKER;
    }
    event->accept();
}


void GlScope::paintEvent( QPaintEvent *event ) {
    if ( shaderCompileSuccess ) {
        QOpenGLWidget::paintEvent( event );
    } else if ( !zoomed ) {
        QPainter painter( this );
        painter.setRenderHint( QPainter::Antialiasing, true );
        QFont font = painter.font();
        font.setPointSize( 18 );
        painter.setFont( font );
        painter.drawText( rect(), Qt::AlignCenter | Qt::TextWordWrap, errorMessage );
        fprintf( stderr, "%s", errorMessage.toUtf8().data() );
    }
    event->accept();
}


void GlScope::initializeGL() {
    if ( scope->verboseLevel )
        qDebug() << "GLScope::initializeGL()";
    if ( !QOpenGLShaderProgram::hasOpenGLShaderPrograms( context() ) ) {
        errorMessage = tr( "System does not support OpenGL Shading Language (GLSL)" );
        return;
    }
    if ( m_program ) {
        qWarning() << tr( "OpenGL init called twice!" );
        return;
    }

    auto program = std::unique_ptr< QOpenGLShaderProgram >( new QOpenGLShaderProgram( context() ) );

    const char *vertexShaderGL100ES = R"(
          #version 100
          attribute highp vec3 vertex;
          uniform mat4 matrix;
          void main()
          {
              gl_Position = matrix * vec4(vertex, 1.0);
              gl_PointSize = 1.0;
          }
    )";

    const char *vertexShaderGLSL120 = R"(
          #version 120
          attribute highp vec3 vertex;
          uniform mat4 matrix;
          void main()
          {
              gl_Position = matrix * vec4(vertex, 1.0);
              gl_PointSize = 1.0;
          }
    )";

    const char *vertexShaderGLSL150 = R"(
          #version 150
          in highp vec3 vertex;
          uniform mat4 matrix;
          void main()
          {
              gl_Position = matrix * vec4(vertex, 1.0);
              gl_PointSize = 1.0;
          }
    )";

    const char *fragmentShaderGL100ES = R"(
          #version 100
          uniform highp vec4 color;
          void main() { gl_FragColor = color; }
    )";

    const char *fragmentShaderGLSL120 = R"(
          #version 120
          uniform highp vec4 color;
          void main() { gl_FragColor = color; }
    )";

    const char *fragmentShaderGLSL150 = R"(
          #version 150
          uniform highp vec4 color;
          out vec4 flatColor;
          void main() { flatColor = color; }
    )";

    QString GLEShint;
    if ( GLES100 != GLSLversion )
        GLEShint = tr( "Try command line option '--useGLES'\n" );
    QString OpenGLinfo = "Graphic: " + OpenGLversion;
    renderInfo = OpenGLinfo + " - GLSL version " + GLSLversion;
    if ( !zoomed )
        qDebug() << renderInfo.toLocal8Bit().data();
    if ( GLSL150 == GLSLversion ) {
        if ( !program->addShaderFromSourceCode( QOpenGLShader::Vertex, vertexShaderGLSL150 ) ||
             !program->addShaderFromSourceCode( QOpenGLShader::Fragment, fragmentShaderGLSL150 ) ) {
            qWarning() << "Switching to GLSL version 1.20, use the command line option '--useGLSL120' or '--useGLES'";
            GLSLversion = GLSL120;
        }
    }
    if ( GLSL120 == GLSLversion ) {
        if ( !program->addShaderFromSourceCode( QOpenGLShader::Vertex, vertexShaderGLSL120 ) ||
             !program->addShaderFromSourceCode( QOpenGLShader::Fragment, fragmentShaderGLSL120 ) ) {
            errorMessage =
                tr( "Failed to compile OpenGL shader programs.\n" ) + GLEShint + OpenGLinfo + QString( "\n" ) + program->log();
            return;
        }
    }
    if ( GLES100 == GLSLversion ) {
        if ( !program->addShaderFromSourceCode( QOpenGLShader::Vertex, vertexShaderGL100ES ) ||
             !program->addShaderFromSourceCode( QOpenGLShader::Fragment, fragmentShaderGL100ES ) ) {
            errorMessage = tr( "Failed to compile OpenGL shader programs.\n" ) + OpenGLinfo + QString( "\n" ) + program->log();
            return;
        }
    }

    if ( !program->link() || !program->bind() ) {
        errorMessage =
            tr( "Failed to link/bind OpenGL shader programs.\n" ) + GLEShint + renderInfo + QString( "\n" ) + program->log();
        return;
    }

    vertexLocation = program->attributeLocation( "vertex" );
    matrixLocation = program->uniformLocation( "matrix" );
    colorLocation = program->uniformLocation( "color" );

    if ( vertexLocation == -1 || colorLocation == -1 || matrixLocation == -1 ) {
        qWarning() << tr( "Failed to locate shader variable." );
        return;
    }

    program->bind();

    auto *gl = context()->functions();
    gl->glDisable( GL_DEPTH_TEST );
    gl->glEnable( GL_BLEND );
    gl->glEnable( GL_DEPTH_TEST );
    gl->glEnable( GL_CULL_FACE );
    gl->glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );

    QColor bg = view->colors->background;
    gl->glClearColor( GLfloat( bg.redF() ), GLfloat( bg.greenF() ), GLfloat( bg.blueF() ), GLfloat( bg.alphaF() ) );

    // --- Задание 3 очереди: слой камеры -------------------------------------
    // Отдельная программа: основная красит сплошным цветом, здесь нужна
    // выборка из текстуры и общая прозрачность. Прямоугольник кладётся один
    // раз на всё поле сетки; порядок вершин — треугольная лента.
    m_cameraProgram = std::make_unique< QOpenGLShaderProgram >();
    // Исходники берутся готовыми под уже установленный диалект. Прежде они
    // склеивались как `GLSLversion + "attribute …"`, а `GLSLversion` — метка
    // ("1.50"), а не директива `#version`: исходник начинался строкой
    // `1.50attribute highp vec3 vertex;` и не компилировался никогда. Отказа
    // при этом не наступало — только предупреждение в поток, — и слоя камеры
    // на холсте не было ни разу с самого его появления.
    const CameraShaders::Sources cam = CameraShaders::forGlslVersion( GLSLversion );
    if ( m_cameraProgram->addShaderFromSourceCode( QOpenGLShader::Vertex, cam.vertex ) &&
         m_cameraProgram->addShaderFromSourceCode( QOpenGLShader::Fragment, cam.fragment ) && m_cameraProgram->link() ) {
        m_cameraLayerError.clear();
        m_cameraVertexLoc = m_cameraProgram->attributeLocation( "vertex" );
        m_cameraTexCoordLoc = m_cameraProgram->attributeLocation( "texCoord" );
        m_cameraMatrixLoc = m_cameraProgram->uniformLocation( "matrix" );
        m_cameraAlphaLoc = m_cameraProgram->uniformLocation( "alpha" );
        m_cameraFitLoc = m_cameraProgram->uniformLocation( "fit" );
        // x, y, z, u, v — прямоугольник во всю сетку. Текстура перевёрнута по
        // вертикали: у QImage начало сверху, у OpenGL снизу.
        const GLfloat quad[] = {
            -GLfloat( DIVS_TIME ) / 2, -GLfloat( DIVS_VOLTAGE ) / 2, 0.0f, 0.0f, 1.0f,
            GLfloat( DIVS_TIME ) / 2,  -GLfloat( DIVS_VOLTAGE ) / 2, 0.0f, 1.0f, 1.0f,
            -GLfloat( DIVS_TIME ) / 2, GLfloat( DIVS_VOLTAGE ) / 2,  0.0f, 0.0f, 0.0f,
            GLfloat( DIVS_TIME ) / 2,  GLfloat( DIVS_VOLTAGE ) / 2,  0.0f, 1.0f, 0.0f };
        m_vaoCamera.create();
        QOpenGLVertexArrayObject::Binder cb( &m_vaoCamera );
        m_cameraQuad.create();
        m_cameraQuad.bind();
        m_cameraQuad.setUsagePattern( QOpenGLBuffer::StaticDraw );
        m_cameraQuad.allocate( quad, int( sizeof( quad ) ) );
        m_cameraProgram->bind();
        m_cameraProgram->enableAttributeArray( m_cameraVertexLoc );
        m_cameraProgram->setAttributeBuffer( m_cameraVertexLoc, GL_FLOAT, 0, 3, 5 * int( sizeof( GLfloat ) ) );
        m_cameraProgram->enableAttributeArray( m_cameraTexCoordLoc );
        m_cameraProgram->setAttributeBuffer( m_cameraTexCoordLoc, GL_FLOAT, 3 * int( sizeof( GLfloat ) ), 2,
                                             5 * int( sizeof( GLfloat ) ) );
        program->bind(); // вернуть основную программу
    } else {
        // Отказ обязан быть слышен: без этого дефект и прожил незамеченным.
        // Текст уходит в окно камеры, а не только в поток вывода.
        m_cameraLayerError = tr( "The camera layer shader did not build (GLSL %1); the layer stays off.\n%2" )
                                 .arg( GLSLversion, m_cameraProgram->log().trimmed() );
        qWarning() << m_cameraLayerError.toLocal8Bit().data();
        m_cameraProgram.reset();
    }

    m_vaoMarker.create();
    QOpenGLVertexArrayObject::Binder b( &m_vaoMarker );
    m_marker.create();
    m_marker.bind();
    m_marker.setUsagePattern( QOpenGLBuffer::StaticDraw );
    m_marker.allocate( int( vaMarker.size() * sizeof( Vertices ) ) );
    program->enableAttributeArray( vertexLocation );
    program->setAttributeBuffer( vertexLocation, GL_FLOAT, 0, 3, 0 );

    updateCursor();

    m_program = std::move( program );

    generateGrid();

    shaderCompileSuccess = true;
}


void GlScope::showData( std::shared_ptr< PPresult > newData ) {
    if ( !shaderCompileSuccess )
        return;
    makeCurrent();
    while ( view->digitalPhosphorDraws() < m_GraphHistory.size() )
        m_GraphHistory.pop_back();

    if ( view->digitalPhosphorDraws() > m_GraphHistory.size() ) {
        m_GraphHistory.resize( m_GraphHistory.size() + 1 );
    }

    m_GraphHistory.splice( m_GraphHistory.begin(), m_GraphHistory, std::prev( m_GraphHistory.end() ) );

    m_GraphHistory.front().writeData( newData.get(), m_program.get(), vertexLocation );

    update();
}


// ============================================================
// XY RECORDER RENDERING
// ============================================================
void GlScope::updateXY( const XYRecorder *recorder ) {
    // Legacy single-curve entry point. Forwards to the multi-curve version
    // using a one-entry array (curve index 0, default config CH1×CH2).
    std::array< const XYRecorder *, DsoSettingsScope::maxXYCurves > recorders{};
    std::array< const XYCurveConfig *, DsoSettingsScope::maxXYCurves > configs{};
    XYCurveConfig defaultCfg;
    defaultCfg.enabled = true;
    defaultCfg.xChannel = 0;
    defaultCfg.yChannel = 1;
    recorders[ 0 ] = recorder;
    configs[ 0 ] = &defaultCfg;
    updateXY( recorders, configs );
}


void GlScope::updateXY( const std::array< const XYRecorder *, DsoSettingsScope::maxXYCurves > &recorders,
                        const std::array< const XYCurveConfig *, DsoSettingsScope::maxXYCurves > &configs ) {
    if ( !shaderCompileSuccess )
        return;

    makeCurrent();

    // Cache the configs so drawXYLegend() can render the legend later (TZ §7.2).
    for ( int i = 0; i < DsoSettingsScope::maxXYCurves; ++i ) {
        m_xyCurveConfigs[ i ] = configs[ i ] ? *configs[ i ] : XYCurveConfig{};
    }
    m_xyCurveConfigsValid = true;

    for ( int i = 0; i < DsoSettingsScope::maxXYCurves; ++i ) {
        const XYRecorder *recorder = recorders[ i ];
        const XYCurveConfig *cfg = configs[ i ];
        if ( !recorder || !cfg || !cfg->enabled || recorder->empty() ) {
            xyPointCount[ i ] = 0;
            xySegments[ i ].clear();
            continue;
        }

        if ( !m_vaoXY[ i ].isCreated() ) {
            m_vaoXY[ i ].create();
            m_xyBuffer[ i ].create();
        }

        QOpenGLVertexArrayObject::Binder b( &m_vaoXY[ i ] );
        m_xyBuffer[ i ].bind();
        m_xyBuffer[ i ].setUsagePattern( QOpenGLBuffer::DynamicDraw );

        const auto &traj = recorder->trajectory();

        std::vector< QVector3D > vertices;
        vertices.reserve( traj.size() );

        // Convert physical values to scope div coordinates using the curve's
        // (xChannel, yChannel) gains and offsets. physicalGain() already
        // includes the CtPU sensitivity k (TZ §6.1.1).
        const uint8_t xCh = cfg->xChannel;
        const uint8_t yCh = cfg->yChannel;
        // У3 — TimeChannel: ось времени рисуется рулоном самописца —
        // новейшая точка прижата к краю экрана, старые уезжают назад.
        // Чувствительность — с/дел из настроек (детенты 1-2-5).
        const bool timeX = ( xCh == DsoSettingsScope::timeChannelIndex );
        const bool timeY = ( yCh == DsoSettingsScope::timeChannelIndex );
        double gainX = 1.0, gainY = 1.0;
        double offsetX = 0.0, offsetY = 0.0;
        if ( timeX ) {
            gainX = scope->horizontal.xyTimeSecPerDiv;
        } else if ( xCh < scope->voltage.size() ) {
            // У4б: экранное увеличение действует и в нашем XY — все функции,
            // доступные для Y-T, доступны и здесь (задание 8 очереди).
            // displayGain() — единственное место пересчёта «деление ↔ величина».
            gainX = scope->displayGain( xCh );
            offsetX = scope->voltage[ xCh ].offset;
        }
        if ( timeY ) {
            gainY = scope->horizontal.xyTimeSecPerDiv;
        } else if ( yCh < scope->voltage.size() ) {
            gainY = scope->displayGain( yCh );
            offsetY = scope->voltage[ yCh ].offset;
        }
        // Guard against zero gain to avoid div-by-zero in the loop below.
        if ( gainX == 0.0 )
            gainX = 1.0;
        if ( gainY == 0.0 )
            gainY = 1.0;

        const double tNewest = ( timeX && !traj.empty() ) ? traj.back().x : ( timeY && !traj.empty() ) ? traj.back().y : 0.0;
        // Разбиение на отрезки: вершины кладутся подряд, а разрывы
        // запоминаются как диапазоны — рисуется каждый своим GL_LINE_STRIP.
        xySegments[ i ].clear();
        int segStart = 0;
        for ( const auto &p : traj ) {
            float xDiv = timeX ? float( ( p.x - tNewest ) / gainX + DIVS_TIME / 2 ) : float( p.x / gainX + offsetX );
            float yDiv = timeY ? float( ( p.y - tNewest ) / gainY + DIVS_VOLTAGE / 2 ) : float( p.y / gainY + offsetY );
            const int here = int( vertices.size() );
            if ( p.newSegment && here > segStart ) {
                xySegments[ i ].emplace_back( segStart, here - segStart );
                segStart = here;
            }
            vertices.emplace_back( QVector3D( xDiv, yDiv, 0.0f ) );
        }
        if ( int( vertices.size() ) > segStart )
            xySegments[ i ].emplace_back( segStart, int( vertices.size() ) - segStart );

        m_xyBuffer[ i ].allocate( vertices.data(), int( vertices.size() * sizeof( QVector3D ) ) );

        auto *prog = m_program.get();
        prog->enableAttributeArray( vertexLocation );
        prog->setAttributeBuffer( vertexLocation, GL_FLOAT, 0, 3, 0 );

        xyPointCount[ i ] = int( traj.size() );
        b.release(); // unbind before doneCurrent() to avoid UB in Binder dtor
    }

    doneCurrent();
    update();
}
// ============================================================


void GlScope::generateVertices( int marker, const DsoSettingsScopeCursor &cursor ) {
    const float Z_ORDER = 1.0f;
    switch ( cursor.shape ) {
    case DsoSettingsScopeCursor::NONE:
        vaMarker[ size_t( marker ) ] = {
            QVector3D( -DIVS_TIME, -DIVS_VOLTAGE, Z_ORDER ), QVector3D( -DIVS_TIME, DIVS_VOLTAGE, Z_ORDER ),
            QVector3D( DIVS_TIME, DIVS_VOLTAGE, Z_ORDER ), QVector3D( DIVS_TIME, -DIVS_VOLTAGE, Z_ORDER ) };
        break;
    case DsoSettingsScopeCursor::VERTICAL:
        vaMarker[ size_t( marker ) ] = { QVector3D( GLfloat( cursor.pos[ 0 ].x() ), -GLfloat( DIVS_VOLTAGE ), Z_ORDER ),
                                         QVector3D( GLfloat( cursor.pos[ 0 ].x() ), GLfloat( DIVS_VOLTAGE ), Z_ORDER ),
                                         QVector3D( GLfloat( cursor.pos[ 1 ].x() ), GLfloat( DIVS_VOLTAGE ), Z_ORDER ),
                                         QVector3D( GLfloat( cursor.pos[ 1 ].x() ), -GLfloat( DIVS_VOLTAGE ), Z_ORDER ) };
        break;
    case DsoSettingsScopeCursor::HORIZONTAL:
        vaMarker[ size_t( marker ) ] = { QVector3D( -GLfloat( DIVS_TIME ), GLfloat( cursor.pos[ 0 ].y() ), Z_ORDER ),
                                         QVector3D( GLfloat( DIVS_TIME ), GLfloat( cursor.pos[ 0 ].y() ), Z_ORDER ),
                                         QVector3D( GLfloat( DIVS_TIME ), GLfloat( cursor.pos[ 1 ].y() ), Z_ORDER ),
                                         QVector3D( -GLfloat( DIVS_TIME ), GLfloat( cursor.pos[ 1 ].y() ), Z_ORDER ) };
        break;
    case DsoSettingsScopeCursor::RECTANGULAR:
        if ( ( cursor.pos[ 1 ].x() - cursor.pos[ 0 ].x() ) * ( cursor.pos[ 1 ].y() - cursor.pos[ 0 ].y() ) > 0.0 ) {
            vaMarker[ size_t( marker ) ] = { QVector3D( GLfloat( cursor.pos[ 0 ].x() ), GLfloat( cursor.pos[ 0 ].y() ), Z_ORDER ),
                                             QVector3D( GLfloat( cursor.pos[ 1 ].x() ), GLfloat( cursor.pos[ 0 ].y() ), Z_ORDER ),
                                             QVector3D( GLfloat( cursor.pos[ 1 ].x() ), GLfloat( cursor.pos[ 1 ].y() ), Z_ORDER ),
                                             QVector3D( GLfloat( cursor.pos[ 0 ].x() ), GLfloat( cursor.pos[ 1 ].y() ), Z_ORDER ) };
        } else {
            vaMarker[ size_t( marker ) ] = { QVector3D( GLfloat( cursor.pos[ 0 ].x() ), GLfloat( cursor.pos[ 0 ].y() ), Z_ORDER ),
                                             QVector3D( GLfloat( cursor.pos[ 0 ].x() ), GLfloat( cursor.pos[ 1 ].y() ), Z_ORDER ),
                                             QVector3D( GLfloat( cursor.pos[ 1 ].x() ), GLfloat( cursor.pos[ 1 ].y() ), Z_ORDER ),
                                             QVector3D( GLfloat( cursor.pos[ 1 ].x() ), GLfloat( cursor.pos[ 0 ].y() ), Z_ORDER ) };
        }
        break;
    }
}


void GlScope::selectCursor( int index ) {
    selectedCursor = index;
    updateCursor( index );
}


void GlScope::updateCursor( int index ) {
    if ( index > 0 ) {
        generateVertices( index, *cursorInfo[ size_t( index ) ] );
    } else
        for ( index = 0; index < int( cursorInfo.size() ); ++index ) {
            generateVertices( index, *cursorInfo[ size_t( index ) ] );
        }
    makeCurrent();
    m_marker.bind();
    m_marker.write( 0, vaMarker.data(), int( vaMarker.size() * sizeof( Vertices ) ) );
}


void GlScope::paintGL() {
    if ( !shaderCompileSuccess )
        return;

    auto *gl = context()->functions();

    QColor bg = view->colors->background;
    gl->glClearColor( GLfloat( bg.redF() ), GLfloat( bg.greenF() ), GLfloat( bg.blueF() ), GLfloat( bg.alphaF() ) );

    gl->glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
    gl->glLineWidth( 1 );

    // Задание 3 очереди: кадр камеры рисуется ПЕРВЫМ и потому лежит ниже
    // всего. Прозрачность задана снаружи: слой обязан просвечивать сквозь
    // сетку и кривые, иначе он их закроет, а не подложится под них.
    drawCameraLayer();

    m_program->bind();

    // Задание 6 очереди: смешивание цветов кривых на перекрытии.
    // Прежде верхняя кривая закрывала нижнюю целиком и та переставала
    // существовать для оператора. Аддитивное сложение (GL_ONE) даёт
    // перекрытию цвет суммы по RGB: красная плюс зелёная = жёлтая.
    // На светлом фоне сложение уходит в белый и перекрытие пропадает,
    // поэтому там оставляем полупрозрачное наложение.
    if ( view->blendCurveOverlap && bg.lightnessF() < 0.5 )
        gl->glBlendFunc( GL_SRC_ALPHA, GL_ONE );
    else
        gl->glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );

    if ( zoomed ) {
        QMatrix4x4 m;
        m.scale( QVector3D( GLfloat( DIVS_TIME ) / GLfloat( fabs( scope->getMarker( 1 ) - scope->getMarker( 0 ) ) ), 1.0f, 1.0f ) );
        m.translate( -GLfloat( scope->getMarker( 0 ) + scope->getMarker( 1 ) ) / 2, 0.0f, 0.0f );
        m_program->setUniformValue( matrixLocation, pmvMatrix * m );
    }

    drawMarkers();

    // ============================================================
    // XY RECORDER MODE: draw up to maxXYCurves trajectories (TZ §7.5)
    // ============================================================
    if ( scope->horizontal.format == Dso::GraphFormat::XY && scope->horizontal.xyContinuous ) {
        bool anyCurveDrawn = false;
        for ( int i = 0; i < DsoSettingsScope::maxXYCurves; ++i ) {
            if ( xyPointCount[ i ] <= 1 )
                continue;
            anyCurveDrawn = true;
            // Задание 4 очереди: цвет назначается ЯВНО. Прежде он брался от
            // канала Y, поэтому две кривые с общим Y выходили одного цвета, и
            // способа это изменить не было. Явный цвет кривой имеет
            // преимущество; при недействительном значении — прежнее правило.
            const XYCurveConfig &cfg = m_xyCurveConfigs[ i ];
            QColor color = cfg.explicitColor.isValid()
                               ? cfg.explicitColor
                               : ( ( cfg.yChannel < view->colors->voltage.size() ) ? view->colors->voltage[ cfg.yChannel ]
                                                                                  : view->colors->voltage[ 0 ] );
            m_program->setUniformValue( colorLocation, color );
            m_vaoXY[ i ].bind();
            gl->glLineWidth( 2 );
            // По отрезку на пакет: конец одного и начало следующего прямой
            // не соединяются — сигнала между ними не было.
            for ( const auto &seg : xySegments[ i ] )
                if ( seg.second > 1 )
                    gl->glDrawArrays( GL_LINE_STRIP, seg.first, seg.second );
            m_vaoXY[ i ].release();

            // Draw the head of each trajectory in a brighter shade.
            if ( xyPointCount[ i ] > 0 ) {
                QColor headColor = color.lighter( 150 );
                m_program->setUniformValue( colorLocation, headColor );
                m_vaoXY[ i ].bind();
                gl->glDrawArrays( GL_POINTS, xyPointCount[ i ] - 1, 1 );
                m_vaoXY[ i ].release();
            }
        }
        (void)anyCurveDrawn;
    }
    // ============================================================
    // STANDARD OSCILLOSCOPE MODE
    // ============================================================
    else {
        unsigned historyIndex = 0;
        for ( Graph &graph : m_GraphHistory ) {
            if ( scope->horizontal.format == Dso::GraphFormat::XY ) {
                // Frame-XY (без непрерывного рекордера): кривые лежат в своих
                // ячейках vaoXYCurves, по одной на слот scope->xyCurves.
                for ( int curve = 0; curve < DsoSettingsScope::maxXYCurves; ++curve )
                    drawXYCurveGraph( curve, graph, int( historyIndex ) );
            } else {
                for ( ChannelID channel = 0; channel < scope->voltage.size(); ++channel ) {
                    drawSpectrumChannelGraph( channel, graph, int( historyIndex ) );
                    if ( scope->histogram ) {
                        drawHistogramChannelGraph( channel, graph, int( historyIndex ) );
                    }
                    drawVoltageChannelGraph( channel, graph, int( historyIndex ) );
                }
            }
            ++historyIndex;
        }
    }

    if ( zoomed ) {
        m_program->setUniformValue( matrixLocation, pmvMatrix );
    }

    drawGrid();
    m_program->release();

    // [MOD] TZ §7.2 — Overlay the multi-curve legend in XY continuous mode.
    // Done after m_program->release() so QPainter takes over the GL surface.
    if ( scope->horizontal.format == Dso::GraphFormat::XY && scope->horizontal.xyContinuous && m_xyCurveConfigsValid ) {
        drawXYLegend();
    }
}


void GlScope::resizeGL( int width, int height ) {
    if ( !shaderCompileSuccess )
        return;
    auto *gl = context()->functions();
    gl->glViewport( 0, 0, GLint( width ), GLint( height ) );

    float pixelizationWidthCorrection = float( width ) / float( width - 1 );
    float pixelizationHeightCorrection = float( height ) / float( height - 1 );

    pmvMatrix.setToIdentity();
    pmvMatrix.ortho( -float( DIVS_TIME ) / 2.0f * pixelizationWidthCorrection,
                     float( DIVS_TIME ) / 2.0f * pixelizationWidthCorrection,
                     -float( DIVS_VOLTAGE ) / 2.0f * pixelizationHeightCorrection,
                     float( DIVS_VOLTAGE ) / 2.0f * pixelizationHeightCorrection, -1.0f, 1.0f );

    m_program->bind();
    m_program->setUniformValue( matrixLocation, pmvMatrix );
    m_program->release();

    // Фон изменился — доля холста под кадр пересчитывается. Без этого кадр
    // сохранял пропорции прежнего окна и растягивался при каждом изменении
    // размера, то есть нормализация держалась бы только до первого resize.
    m_cameraFit = CameraShaders::fitScale( m_cameraFrameSize, QSize( width, height ) );
}


void GlScope::draw4Cross( std::vector< QVector3D > &va, int section, float x, float y ) {
    const float d = 0.05f;
    for ( float xSign : { -1.0f, 1.0f } ) {
        for ( float ySign : { -1.0f, 1.0f } ) {
            gridDrawCounts[ section ] += 4;
            va.push_back( QVector3D( xSign * ( x - d ), ySign * y, 0 ) );
            va.push_back( QVector3D( xSign * ( x + d ), ySign * y, 0 ) );
            va.push_back( QVector3D( xSign * x, ySign * ( y - d ), 0 ) );
            va.push_back( QVector3D( xSign * x, ySign * ( y + d ), 0 ) );
        }
    }
}


void GlScope::generateGrid( int index, double value, bool pressed ) {
    QOpenGLShaderProgram *program = m_program.get();
    if ( program == nullptr )
        return;

    for ( int iii = 0; iii < gridItems; ++iii )
        gridDrawCounts[ iii ] = 0;

    if ( !m_grid.isCreated() )
        m_grid.create();
    m_grid.bind();
    m_grid.setUsagePattern( QOpenGLBuffer::StaticDraw );

    std::vector< QVector3D > vaGrid;

    int item = 0;

    {
        if ( !m_vaoGrid[ item ].isCreated() )
            m_vaoGrid[ item ].create();
        QOpenGLVertexArrayObject::Binder b( &m_vaoGrid[ item ] );
        m_grid.bind();
        program->enableAttributeArray( vertexLocation );
        program->setAttributeBuffer( vertexLocation, GL_FLOAT, 0, 3, 0 );
    }

    for ( int vDiv = 1; vDiv < DIVS_TIME / 2; ++vDiv ) {
        for ( int dot = 1; dot < DIVS_VOLTAGE / 2 * DIVS_SUB; ++dot ) {
            float dotPosition = float( dot ) / DIVS_SUB;
            gridDrawCounts[ item ] += 4;
            vaGrid.push_back( QVector3D( -float( vDiv ), -dotPosition, 0 ) );
            vaGrid.push_back( QVector3D( -float( vDiv ), dotPosition, 0 ) );
            vaGrid.push_back( QVector3D( float( vDiv ), -dotPosition, 0 ) );
            vaGrid.push_back( QVector3D( float( vDiv ), dotPosition, 0 ) );
        }
    }
    for ( int hDiv = 1; hDiv < DIVS_VOLTAGE / 2; ++hDiv ) {
        for ( int dot = 1; dot < DIVS_TIME / 2 * DIVS_SUB; ++dot ) {
            if ( dot % DIVS_SUB == 0 )
                continue;
            float dotPosition = float( dot ) / DIVS_SUB;
            gridDrawCounts[ item ] += 4;
            vaGrid.push_back( QVector3D( -dotPosition, -float( hDiv ), 0 ) );
            vaGrid.push_back( QVector3D( dotPosition, -float( hDiv ), 0 ) );
            vaGrid.push_back( QVector3D( -dotPosition, float( hDiv ), 0 ) );
            vaGrid.push_back( QVector3D( dotPosition, float( hDiv ), 0 ) );
        }
    }

    ++item;

    {
        if ( !m_vaoGrid[ item ].isCreated() )
            m_vaoGrid[ item ].create();
        QOpenGLVertexArrayObject::Binder b( &m_vaoGrid[ item ] );
        m_grid.bind();
        program->enableAttributeArray( vertexLocation );
        program->setAttributeBuffer( vertexLocation, GL_FLOAT, int( vaGrid.size() * sizeof( QVector3D ) ), 3, 0 );
    }

    gridDrawCounts[ item ] += 2;
    vaGrid.push_back( QVector3D( -DIVS_TIME / 2, 0, 0 ) );
    vaGrid.push_back( QVector3D( DIVS_TIME / 2, 0, 0 ) );
    gridDrawCounts[ item ] += 2;
    vaGrid.push_back( QVector3D( 0, -DIVS_VOLTAGE / 2, 0 ) );
    vaGrid.push_back( QVector3D( 0, DIVS_VOLTAGE / 2, 0 ) );
    for ( int line = 1; line < DIVS_TIME / 2 * DIVS_SUB; ++line ) {
        float linePosition = float( line ) / DIVS_SUB;
        gridDrawCounts[ item ] += 4;
        vaGrid.push_back( QVector3D( linePosition, -0.05f, 0 ) );
        vaGrid.push_back( QVector3D( linePosition, 0.05f, 0 ) );
        vaGrid.push_back( QVector3D( -linePosition, -0.05f, 0 ) );
        vaGrid.push_back( QVector3D( -linePosition, 0.05f, 0 ) );
    }
    for ( int line = 1; line < DIVS_VOLTAGE / 2 * DIVS_SUB; ++line ) {
        float linePosition = float( line ) / DIVS_SUB;
        gridDrawCounts[ item ] += 4;
        vaGrid.push_back( QVector3D( -0.05f, linePosition, 0 ) );
        vaGrid.push_back( QVector3D( 0.05f, linePosition, 0 ) );
        vaGrid.push_back( QVector3D( -0.05f, -linePosition, 0 ) );
        vaGrid.push_back( QVector3D( 0.05f, -linePosition, 0 ) );
    }

    for ( int vDiv = 1; vDiv < DIVS_TIME / 2; ++vDiv ) {
        for ( int hDiv = 1; hDiv < DIVS_VOLTAGE / 2; ++hDiv ) {
            draw4Cross( vaGrid, 1, float( vDiv ), float( hDiv ) );
        }
    }
    for ( int hDiv = 1; hDiv < DIVS_VOLTAGE / 2; ++hDiv ) {
        for ( int vDiv = 1; vDiv < DIVS_TIME / 2; ++vDiv ) {
            if ( vDiv % DIVS_SUB == 0 )
                continue;
            draw4Cross( vaGrid, 1, float( vDiv ), float( hDiv ) );
        }
    }

    ++item;

    {
        if ( !m_vaoGrid[ item ].isCreated() )
            m_vaoGrid[ item ].create();
        QOpenGLVertexArrayObject::Binder b( &m_vaoGrid[ item ] );
        m_grid.bind();
        program->enableAttributeArray( vertexLocation );
        program->setAttributeBuffer( vertexLocation, GL_FLOAT, int( vaGrid.size() * sizeof( QVector3D ) ), 3, 0 );
    }
    gridDrawCounts[ item ] += 4;
    vaGrid.push_back( QVector3D( -DIVS_TIME / 2, -DIVS_VOLTAGE / 2, 0 ) );
    vaGrid.push_back( QVector3D( DIVS_TIME / 2, -DIVS_VOLTAGE / 2, 0 ) );
    vaGrid.push_back( QVector3D( DIVS_TIME / 2, DIVS_VOLTAGE / 2, 0 ) );
    vaGrid.push_back( QVector3D( -DIVS_TIME / 2, DIVS_VOLTAGE / 2, 0 ) );

    ++item;

    {
        if ( !m_vaoGrid[ item ].isCreated() )
            m_vaoGrid[ item ].create();
        QOpenGLVertexArrayObject::Binder b( &m_vaoGrid[ item ] );
        m_grid.bind();
        program->enableAttributeArray( vertexLocation );
        program->setAttributeBuffer( vertexLocation, GL_FLOAT, int( vaGrid.size() * sizeof( QVector3D ) ), 3, 0 );
    }
    if ( pressed && index >= 0 ) {
        triggerLineColor = view->colors->voltage[ unsigned( index ) ];
        if ( index != int( scope->trigger.source ) )
            triggerLineColor = triggerLineColor.darker();
        float yPos = float( ( value / scope->gain( unsigned( index ) ) + scope->voltage[ unsigned( index ) ].offset ) );
        gridDrawCounts[ item ] += 2;
        vaGrid.push_back( QVector3D( -DIVS_TIME / 2, yPos, 0 ) );
        vaGrid.push_back( QVector3D( DIVS_TIME / 2, yPos, 0 ) );
    }

    m_grid.allocate( &vaGrid[ 0 ], int( vaGrid.size() * sizeof( QVector3D ) ) );
    m_grid.release();
}


void GlScope::drawGrid() {
    auto *gl = context()->functions();

    gl->glLineWidth( 1 );

    int item = 3;
    m_vaoGrid[ item ].bind();
    m_program->setUniformValue( colorLocation, triggerLineColor );
    gl->glDrawArrays( GL_LINES, 0, gridDrawCounts[ item ] );
    m_vaoGrid[ item ].release();

    item = 0;
    m_vaoGrid[ item ].bind();
    m_program->setUniformValue( colorLocation, view->colors->grid );
    gl->glDrawArrays( GL_POINTS, 0, gridDrawCounts[ item ] );
    m_vaoGrid[ item ].release();

    ++item;
    m_vaoGrid[ item ].bind();
    m_program->setUniformValue( colorLocation, view->colors->axes );
    gl->glDrawArrays( GL_LINES, 0, gridDrawCounts[ item ] );
    m_vaoGrid[ item ].release();

    ++item;
    m_vaoGrid[ item ].bind();
    m_program->setUniformValue( colorLocation, view->colors->border );
    gl->glDrawArrays( GL_LINE_LOOP, 0, gridDrawCounts[ item ] );
    m_vaoGrid[ item ].release();
}


void GlScope::drawVertices( QOpenGLFunctions *gl, int marker, QColor color ) {
    m_program->setUniformValue( colorLocation, ( marker == selectedCursor ) ? color : color.darker() );
    gl->glDrawArrays( GL_LINE_LOOP, GLint( marker * VERTICES_ARRAY_SIZE ), GLint( VERTICES_ARRAY_SIZE ) );
    if ( cursorInfo[ size_t( marker ) ]->shape == DsoSettingsScopeCursor::RECTANGULAR ) {
        color.setAlphaF( 0.5 );
        m_program->setUniformValue( colorLocation, color.darker() );
        gl->glDrawArrays( GL_TRIANGLE_FAN, GLint( marker * VERTICES_ARRAY_SIZE ), GLint( VERTICES_ARRAY_SIZE ) );
    }
}


void GlScope::drawXYLegend() {
    // TZ §7.2 — overlay a per-curve legend in the top-left corner of the scope
    // area. Format: `Name(x=<gainX>/div; y=<gainY>/div)` using physicalGain()
    // so the displayed gains reflect the channel's CtPU sensitivity.
    if ( !m_xyCurveConfigsValid )
        return;

    QPainter painter( this );
    painter.setRenderHint( QPainter::Antialiasing, true );

    QFont font = painter.font();
    font.setPointSize( font.pointSize() > 0 ? font.pointSize() : 9 );
    painter.setFont( font );

    QFontMetrics fm( font );
    const int margin = 8;
    const int lineSpacing = fm.lineSpacing() + 2;

    int y = margin;
    int x = margin;
    int maxWidth = 0;

    for ( int i = 0; i < DsoSettingsScope::maxXYCurves; ++i ) {
        const XYCurveConfig &cfg = m_xyCurveConfigs[ i ];
        if ( !cfg.enabled )
            continue;
        // У3 — TimeChannel: индекс времени валиден, хотя и вне voltage[].
        const bool legTimeX = ( cfg.xChannel == DsoSettingsScope::timeChannelIndex );
        const bool legTimeY = ( cfg.yChannel == DsoSettingsScope::timeChannelIndex );
        if ( ( !legTimeX && cfg.xChannel >= scope->voltage.size() ) || ( !legTimeY && cfg.yChannel >= scope->voltage.size() ) )
            continue;

        const QString xName = legTimeX ? tr( "T" ) : scope->voltage[ cfg.xChannel ].name;
        const QString yName = legTimeY ? tr( "T" ) : scope->voltage[ cfg.yChannel ].name;
        const QString xUnit = legTimeX ? QStringLiteral( "s" )
                              : scope->voltage[ cfg.xChannel ].ctpuUnit.isEmpty()
                                  ? QStringLiteral( "V" )
                                  : scope->voltage[ cfg.xChannel ].ctpuUnit;
        const QString yUnit = legTimeY ? QStringLiteral( "s" )
                              : scope->voltage[ cfg.yChannel ].ctpuUnit.isEmpty()
                                  ? QStringLiteral( "V" )
                                  : scope->voltage[ cfg.yChannel ].ctpuUnit;
        const double xGain = legTimeX ? scope->horizontal.xyTimeSecPerDiv : scope->physicalGain( cfg.xChannel );
        const double yGain = legTimeY ? scope->horizontal.xyTimeSecPerDiv : scope->physicalGain( cfg.yChannel );

        // [FIX] «/дел» был захардкоженной русской строкой мимо tr() — тот же
        // класс дефекта, что уже чинился в preview-метке конфиг-диалога.
        const QString line = QStringLiteral( "%1(x=%2%4; y=%3%4)" )
                                 .arg( yName )
                                 .arg( valueToString( xGain, xUnit, 3 ) )
                                 .arg( valueToString( yGain, yUnit, 3 ) )
                                 .arg( tr( "/div" ) );
        const QColor color = ( cfg.yChannel < view->colors->voltage.size() ) ? view->colors->voltage[ cfg.yChannel ]
                                                                             : view->colors->voltage[ 0 ];

        // Draw a colored swatch + the text.
        painter.fillRect( x, y - fm.ascent(), 14, lineSpacing - 2, color );
        painter.setPen( QPen( view->colors->text, 1 ) );
        painter.drawText( x + 22, y, line );
        const int lineWidth = 22 + fm.horizontalAdvance( line );
        if ( lineWidth > maxWidth )
            maxWidth = lineWidth;
        y += lineSpacing;
    }

    (void)maxWidth;
    painter.end();
}


void GlScope::drawMarkers() {
    auto *gl = context()->functions();

    m_vaoMarker.bind();

    int marker = 0;
    drawVertices( gl, marker, view->colors->markers );
    ++marker;

    if ( view->cursorsVisible ) {
        gl->glDepthMask( GL_FALSE );
        for ( ChannelID channel = 0; channel < scope->voltage.size(); ++channel, ++marker ) {
            if ( scope->voltage[ channel ].used ) {
                drawVertices( gl, marker, view->colors->voltage[ channel ] );
            }
        }
        for ( ChannelID channel = 0; channel < scope->spectrum.size(); ++channel, ++marker ) {
            if ( scope->spectrum[ channel ].used ) {
                drawVertices( gl, marker, view->colors->spectrum[ channel ] );
            }
        }
        gl->glDepthMask( GL_TRUE );
    }

    m_vaoMarker.release();
}


void GlScope::drawVoltageChannelGraph( ChannelID channel, Graph &graph, int historyIndex ) {
    if ( !scope->voltage[ channel ].used )
        return;

    m_program->setUniformValue( colorLocation, view->colors->voltage[ channel ].darker( 100 + 10 * historyIndex ) );
    Graph::VaoCount &v = graph.vaoVoltage[ channel ];

    QOpenGLVertexArrayObject::Binder b( v.first );
    const GLenum dMode = ( view->interpolation == Dso::INTERPOLATION_OFF ) ? GL_POINTS : GL_LINE_STRIP;
    context()->functions()->glDrawArrays( dMode, 0, v.second );
}


/// Draws one frame-XY trajectory. Indexed by curve slot: the curve's own
/// (xChannel, yChannel) pair lives in scope->xyCurves[curve], and the colour
/// comes from the Y channel (TZ §7.1.3), matching the continuous recorder path.
void GlScope::drawXYCurveGraph( int curve, Graph &graph, int historyIndex ) {
    if ( curve < 0 || curve >= int( graph.vaoXYCurves.size() ) || curve >= int( scope->xyCurves.size() ) )
        return;
    const XYCurveConfig &cfg = scope->xyCurves[ size_t( curve ) ];
    if ( !cfg.enabled )
        return;
    Graph::VaoCount &v = graph.vaoXYCurves[ size_t( curve ) ];
    if ( !v.first || v.second <= 0 )
        return;

    const ChannelID yChannel = ( cfg.yChannel < view->colors->voltage.size() ) ? cfg.yChannel : 0;
    m_program->setUniformValue( colorLocation, view->colors->voltage[ yChannel ].darker( 100 + 10 * historyIndex ) );

    QOpenGLVertexArrayObject::Binder b( v.first );
    const GLenum dMode = ( view->interpolation == Dso::INTERPOLATION_OFF ) ? GL_POINTS : GL_LINE_STRIP;
    context()->functions()->glDrawArrays( dMode, 0, v.second );
}


void GlScope::drawHistogramChannelGraph( ChannelID channel, Graph &graph, int historyIndex ) {
    if ( graph.vaoHistogram.empty() || !scope->voltage[ channel ].used )
        return;

    m_program->setUniformValue( colorLocation, view->colors->voltage[ channel ].darker( 100 + 10 * historyIndex ) );
    Graph::VaoCount &h = graph.vaoHistogram[ channel ];

    QOpenGLVertexArrayObject::Binder b( h.first );
    const GLenum dMode = GL_LINES;
    context()->functions()->glDrawArrays( dMode, 0, h.second );
}


void GlScope::drawSpectrumChannelGraph( ChannelID channel, Graph &graph, int historyIndex ) {
    if ( !scope->spectrum[ channel ].used )
        return;

    m_program->setUniformValue( colorLocation, view->colors->spectrum[ channel ].darker( 100 + 10 * historyIndex ) );
    Graph::VaoCount &v = graph.vaoSpectrum[ channel ];

    QOpenGLVertexArrayObject::Binder b( v.first );
    const GLenum dMode = ( view->interpolation == Dso::INTERPOLATION_OFF ) ? GL_POINTS : GL_LINE_STRIP;
    context()->functions()->glDrawArrays( dMode, 0, v.second );
}


/// \brief Кадр камеры самым задним слоем холста (задание 3 очереди).
///
/// Слои выше обычно не заливают всё поле, поэтому кадр виден между ними; там,
/// где они всё же перекрывают, он просвечивает сквозь них благодаря
/// прозрачности. Ни измерения, ни обработки здесь нет — только вывод
/// (см. `cameralayer.h`).
void GlScope::drawCameraLayer() {
    if ( !view->cameraLayerEnabled || !m_cameraProgram || !m_cameraTexture || !m_cameraTexture->isCreated() )
        return;
    auto *gl = context()->functions();
    m_cameraProgram->bind();
    m_cameraProgram->setUniformValue( m_cameraMatrixLoc, pmvMatrix );
    m_cameraProgram->setUniformValue( m_cameraAlphaLoc, GLfloat( view->cameraLayerOpacity ) );
    // Нормализация на фон: доля холста, которую занимает кадр. Пропорции
    // кадра сохраняются, растяжения по холсту нет.
    m_cameraProgram->setUniformValue( m_cameraFitLoc, GLfloat( m_cameraFit.width() ), GLfloat( m_cameraFit.height() ) );
    // Слой кладётся ПОД остальное, поэтому здесь обычное наложение, а не
    // сложение: сложение осветляло бы кадр тем сильнее, чем больше на нём
    // нарисовано сверху, и кадр менялся бы от постороннего.
    gl->glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
    gl->glDisable( GL_DEPTH_TEST );
    m_cameraTexture->bind( 0 );
    m_cameraProgram->setUniformValue( "tex", 0 );
    {
        QOpenGLVertexArrayObject::Binder b( &m_vaoCamera );
        gl->glDrawArrays( GL_TRIANGLE_STRIP, 0, 4 );
    }
    m_cameraTexture->release();
    m_cameraProgram->release();
    gl->glEnable( GL_DEPTH_TEST );
}


void GlScope::setCameraFrame( const QImage &frame ) {
    if ( frame.isNull() ) {
        m_cameraTexture.reset(); // кадров нет — слоя нет, а не старый кадр навечно
        m_cameraFrameSize = QSize();
        m_cameraFit = QSizeF( 1.0, 1.0 );
        update();
        return;
    }
    makeCurrent();

    // Нормализация кадра на размер фона (задача автора 2026-09-02: «размер
    // изображения камеры может быть разный… от 0,31 Mp до 40 Mp и это нужно
    // нормализовать на не фиксированный размер фона»). Две части:
    //
    // 1. Уменьшение до размера холста. Кадр 40 Mp это ~7300×5500: он
    //    перекладывается в текстуру на каждом кадре, а у части драйверов ещё
    //    и превышает GL_MAX_TEXTURE_SIZE — тогда текстура не создаётся и слоя
    //    снова нет. Пикселей больше, чем есть на холсте, показать нельзя по
    //    построению, поэтому уменьшение ничего не теряет.
    // 2. Доля холста под кадр — пропорции сохраняются, растяжения нет:
    //    оператор смотрит сквозь слой на предмет, и искажать его нельзя.
    if ( m_cameraMaxTextureSize == 0 )
        context()->functions()->glGetIntegerv( GL_MAX_TEXTURE_SIZE, &m_cameraMaxTextureSize );
    const QSize canvas = canvasPixelSize();
    const QSize target = CameraShaders::normalizedFrameSize( frame.size(), canvas, m_cameraMaxTextureSize );
    const QImage fitted = target.isEmpty() ? frame : frame.scaled( target, Qt::KeepAspectRatio, Qt::SmoothTransformation );
    m_cameraFrameSize = fitted.size();
    m_cameraFit = CameraShaders::fitScale( m_cameraFrameSize, canvas );

    // Текстура пересоздаётся при смене размера кадра; иначе только данные.
    //
    // Восьмой случай двоевластия, и он давал ЗЕРКАЛО. Прежде первый кадр шёл
    // через `QOpenGLTexture( QImage )`, а последующие — через низкоуровневый
    // `setData()`. Конструктор из QImage переворачивает изображение по
    // вертикали (у QImage начало сверху, у OpenGL снизу), а `setData()` кладёт
    // строки как есть. Два пути загрузки одной текстуры дали две разные
    // ориентации, и оператор видел поток кадров, перевёрнутый относительно
    // того же кадра, сохранённого в файл.
    //
    // Теперь путь ОДИН: хранилище выделяется явно, данные всегда идут через
    // `setData()`. Строка 0 текстуры = верхняя строка QImage, и координаты
    // прямоугольника (v=0 у верхней вершины) с этим согласованы.
    const QImage rgba = fitted.convertToFormat( QImage::Format_RGBA8888 );
    if ( !m_cameraTexture || m_cameraTexture->width() != rgba.width() || m_cameraTexture->height() != rgba.height() ) {
        m_cameraTexture = std::make_unique< QOpenGLTexture >( QOpenGLTexture::Target2D );
        m_cameraTexture->setFormat( QOpenGLTexture::RGBA8_UNorm );
        m_cameraTexture->setSize( rgba.width(), rgba.height() );
        m_cameraTexture->setMipLevels( 1 );
        m_cameraTexture->setMinificationFilter( QOpenGLTexture::Linear );
        m_cameraTexture->setMagnificationFilter( QOpenGLTexture::Linear );
        m_cameraTexture->setWrapMode( QOpenGLTexture::ClampToEdge );
        m_cameraTexture->allocateStorage( QOpenGLTexture::RGBA, QOpenGLTexture::UInt8 );
    }
    m_cameraTexture->setData( QOpenGLTexture::RGBA, QOpenGLTexture::UInt8, rgba.constBits() );
    doneCurrent();
    update();
}


/// Размер холста в пикселях — то, что автор называет размером фона.
///
/// Он НЕ фиксирован: меняется вместе с окном, а на экране с масштабированием
/// ещё и отличается от логического размера виджета в `devicePixelRatio` раз.
/// Кадр нормализуется именно на него, поэтому величина берётся в одном месте.
QSize GlScope::canvasPixelSize() const {
    const qreal dpr = devicePixelRatioF();
    return QSize( int( width() * dpr ), int( height() * dpr ) );
}
