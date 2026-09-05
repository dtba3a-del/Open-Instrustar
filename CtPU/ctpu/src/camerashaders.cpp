// SPDX-License-Identifier: GPL-3.0-or-later

#include "camerashaders.h"

#include <algorithm>

namespace CameraShaders {

    // Три диалекта, как у основной программы (`glscope.cpp`, initializeGL).
    // Директива `#version` стоит В САМОМ исходнике, а не приклеивается снаружи:
    // приклеивание и было дефектом. Метка выбирает исходник, но в текст не
    // попадает.
    //
    // Различия между диалектами ровно те же, что у основной программы:
    // 1.00 ES и 1.20 — `attribute`/`varying`/`gl_FragColor`/`texture2D`;
    // 1.50 (ядро) — `in`/`out`/`texture`, свой выход вместо `gl_FragColor`.

    static const char *vertex100ES = R"(#version 100
attribute highp vec3 vertex;
attribute highp vec2 texCoord;
uniform highp mat4 matrix;
uniform highp vec2 fit;
varying highp vec2 uv;
void main() {
    uv = texCoord;
    gl_Position = matrix * vec4( vertex.xy * fit, vertex.z, 1.0 );
}
)";

    static const char *fragment100ES = R"(#version 100
uniform highp sampler2D tex;
uniform highp float alpha;
varying highp vec2 uv;
void main() {
    highp vec4 c = texture2D( tex, uv );
    gl_FragColor = vec4( c.rgb, c.a * alpha );
}
)";

    static const char *vertex120 = R"(#version 120
attribute highp vec3 vertex;
attribute highp vec2 texCoord;
uniform mat4 matrix;
uniform vec2 fit;
varying highp vec2 uv;
void main() {
    uv = texCoord;
    gl_Position = matrix * vec4( vertex.xy * fit, vertex.z, 1.0 );
}
)";

    static const char *fragment120 = R"(#version 120
uniform sampler2D tex;
uniform highp float alpha;
varying highp vec2 uv;
void main() {
    vec4 c = texture2D( tex, uv );
    gl_FragColor = vec4( c.rgb, c.a * alpha );
}
)";

    static const char *vertex150 = R"(#version 150
in highp vec3 vertex;
in highp vec2 texCoord;
uniform mat4 matrix;
uniform vec2 fit;
out highp vec2 uv;
void main() {
    uv = texCoord;
    gl_Position = matrix * vec4( vertex.xy * fit, vertex.z, 1.0 );
}
)";

    static const char *fragment150 = R"(#version 150
uniform sampler2D tex;
uniform highp float alpha;
in highp vec2 uv;
out vec4 cameraColor;
void main() {
    vec4 c = texture( tex, uv );
    cameraColor = vec4( c.rgb, c.a * alpha );
}
)";


    Sources forGlslVersion( const QString &glslVersion ) {
        if ( glslVersion == QLatin1String( GLES100 ) )
            return { QString::fromLatin1( vertex100ES ), QString::fromLatin1( fragment100ES ) };
        if ( glslVersion == QLatin1String( GLSL150 ) )
            return { QString::fromLatin1( vertex150 ), QString::fromLatin1( fragment150 ) };
        // 1.20 и всё незнакомое: самый широко поддержанный из трёх.
        return { QString::fromLatin1( vertex120 ), QString::fromLatin1( fragment120 ) };
    }


    QSizeF fitScale( const QSize &frame, const QSize &canvas ) {
        if ( frame.width() <= 0 || frame.height() <= 0 || canvas.width() <= 0 || canvas.height() <= 0 )
            return QSizeF( 1.0, 1.0 );
        const double frameAspect = double( frame.width() ) / double( frame.height() );
        const double canvasAspect = double( canvas.width() ) / double( canvas.height() );
        if ( frameAspect >= canvasAspect )
            return QSizeF( 1.0, canvasAspect / frameAspect ); // упёрся в ширину
        return QSizeF( frameAspect / canvasAspect, 1.0 );     // упёрся в высоту
    }


    QSize normalizedFrameSize( const QSize &frame, const QSize &canvas, int maxTextureSize ) {
        if ( frame.width() <= 0 || frame.height() <= 0 )
            return QSize();
        // Верхняя граница: холст, а при известном пределе драйвера — ещё и он.
        int limitW = canvas.width() > 0 ? canvas.width() : frame.width();
        int limitH = canvas.height() > 0 ? canvas.height() : frame.height();
        if ( maxTextureSize > 0 ) {
            limitW = std::min( limitW, maxTextureSize );
            limitH = std::min( limitH, maxTextureSize );
        }
        if ( frame.width() <= limitW && frame.height() <= limitH )
            return QSize(); // кадр и так не больше — уменьшать нечего
        QSize target = frame;
        target.scale( limitW, limitH, Qt::KeepAspectRatio );
        // Ни одна сторона не должна выродиться в ноль: пустая текстура — это
        // снова «слоя нет», только по другой причине.
        return QSize( std::max( 1, target.width() ), std::max( 1, target.height() ) );
    }

} // namespace CameraShaders
