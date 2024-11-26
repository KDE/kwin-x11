/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2006-2007 Rivo Laks <rivolaks@hot.ee>
    SPDX-FileCopyrightText: 2010, 2011 Martin Gräßlin <mgraesslin@kde.org>
    SPDX-FileCopyrightText: 2023 Xaver Hugl <xaver.hugl@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "glshader.h"
#include "gllut.h"
#include "gllut3D.h"
#include "glplatform.h"
#include "glutils.h"
#include "utils/common.h"

#include <QFile>

namespace KWin
{

GLShader::GLShader(unsigned int flags)
    : m_valid(false)
    , m_locationsResolved(false)
    , m_explicitLinking(flags & ExplicitLinking)
{
    m_program = glCreateProgram();
}

GLShader::GLShader(const QString &vertexfile, const QString &fragmentfile, unsigned int flags)
    : m_valid(false)
    , m_locationsResolved(false)
    , m_explicitLinking(flags & ExplicitLinking)
{
    m_program = glCreateProgram();
    loadFromFiles(vertexfile, fragmentfile);
}

GLShader::~GLShader()
{
    if (m_program) {
        glDeleteProgram(m_program);
    }
}

bool GLShader::loadFromFiles(const QString &vertexFile, const QString &fragmentFile)
{
    QFile vf(vertexFile);
    if (!vf.open(QIODevice::ReadOnly)) {
        qCCritical(KWIN_OPENGL) << "Couldn't open" << vertexFile << "for reading!";
        return false;
    }
    const QByteArray vertexSource = vf.readAll();

    QFile ff(fragmentFile);
    if (!ff.open(QIODevice::ReadOnly)) {
        qCCritical(KWIN_OPENGL) << "Couldn't open" << fragmentFile << "for reading!";
        return false;
    }
    const QByteArray fragmentSource = ff.readAll();

    return load(vertexSource, fragmentSource);
}

bool GLShader::link()
{
    // Be optimistic
    m_valid = true;

    glLinkProgram(m_program);

    // Get the program info log
    int maxLength, length;
    glGetProgramiv(m_program, GL_INFO_LOG_LENGTH, &maxLength);

    QByteArray log(maxLength, 0);
    glGetProgramInfoLog(m_program, maxLength, &length, log.data());

    // Make sure the program linked successfully
    int status;
    glGetProgramiv(m_program, GL_LINK_STATUS, &status);

    if (status == 0) {
        qCCritical(KWIN_OPENGL) << "Failed to link shader:"
                                << "\n"
                                << log;
        m_valid = false;
    } else if (length > 0) {
        qCDebug(KWIN_OPENGL) << "Shader link log:" << log;
    }

    return m_valid;
}

const QByteArray GLShader::prepareSource(GLenum shaderType, const QByteArray &source) const
{
    // Prepare the source code
    QByteArray ba;
    const auto context = OpenGlContext::currentContext();
    if (context->isOpenGLES() && context->glslVersion() < Version(3, 0)) {
        ba.append("precision highp float;\n");
    }
    ba.append(source);
    if (context->isOpenGLES() && context->glslVersion() >= Version(3, 0)) {
        ba.replace("#version 140", "#version 300 es\n\nprecision highp float;\n");
    }

    return ba;
}

bool GLShader::compile(GLuint program, GLenum shaderType, const QByteArray &source) const
{
    GLuint shader = glCreateShader(shaderType);

    QByteArray preparedSource = prepareSource(shaderType, source);
    const char *src = preparedSource.constData();
    glShaderSource(shader, 1, &src, nullptr);

    // Compile the shader
    glCompileShader(shader);

    // Get the shader info log
    int maxLength, length;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &maxLength);

    QByteArray log(maxLength, 0);
    glGetShaderInfoLog(shader, maxLength, &length, log.data());

    // Check the status
    int status;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);

    if (status == 0) {
        const char *typeName = (shaderType == GL_VERTEX_SHADER ? "vertex" : "fragment");
        qCCritical(KWIN_OPENGL) << "Failed to compile" << typeName << "shader:"
                                << "\n"
                                << log;
        size_t line = 0;
        const auto split = source.split('\n');
        for (const auto &l : split) {
            qCCritical(KWIN_OPENGL).nospace() << "line " << line++ << ":" << l;
        }
    } else if (length > 0) {
        qCDebug(KWIN_OPENGL) << "Shader compile log:" << log;
    }

    if (status != 0) {
        glAttachShader(program, shader);
    }

    glDeleteShader(shader);
    return status != 0;
}

bool GLShader::load(const QByteArray &vertexSource, const QByteArray &fragmentSource)
{
    m_valid = false;

    // Compile the vertex shader
    if (!vertexSource.isEmpty()) {
        bool success = compile(m_program, GL_VERTEX_SHADER, vertexSource);

        if (!success) {
            return false;
        }
    }

    // Compile the fragment shader
    if (!fragmentSource.isEmpty()) {
        bool success = compile(m_program, GL_FRAGMENT_SHADER, fragmentSource);

        if (!success) {
            return false;
        }
    }

    if (m_explicitLinking) {
        return true;
    }

    // link() sets mValid
    return link();
}

void GLShader::bindAttributeLocation(const char *name, int index)
{
    glBindAttribLocation(m_program, index, name);
}

void GLShader::bindFragDataLocation(const char *name, int index)
{
    const auto context = OpenGlContext::currentContext();
    if (!context->isOpenGLES() && (context->hasVersion(Version(3, 0)) || context->hasOpenglExtension(QByteArrayLiteral("GL_EXT_gpu_shader4")))) {
        glBindFragDataLocation(m_program, index, name);
    }
}

void GLShader::bind()
{
    glUseProgram(m_program);
}

void GLShader::unbind()
{
    glUseProgram(0);
}

void GLShader::resolveLocations()
{
    if (m_locationsResolved) {
        return;
    }

    m_matrix4Locations[Mat4Uniform::TextureMatrix] = uniformLocation("textureMatrix");
    m_matrix4Locations[Mat4Uniform::ProjectionMatrix] = uniformLocation("projection");
    m_matrix4Locations[Mat4Uniform::ModelViewMatrix] = uniformLocation("modelview");
    m_matrix4Locations[Mat4Uniform::ModelViewProjectionMatrix] = uniformLocation("modelViewProjectionMatrix");
    m_matrix4Locations[Mat4Uniform::WindowTransformation] = uniformLocation("windowTransformation");
    m_matrix4Locations[Mat4Uniform::ScreenTransformation] = uniformLocation("screenTransformation");
    m_matrix4Locations[Mat4Uniform::ColorimetryTransformation] = uniformLocation("colorimetryTransform");
    m_matrix4Locations[Mat4Uniform::DestinationToLMS] = uniformLocation("destinationToLMS");
    m_matrix4Locations[Mat4Uniform::LMSToDestination] = uniformLocation("lmsToDestination");

    m_vec2Locations[Vec2Uniform::Offset] = uniformLocation("offset");
    m_vec2Locations[Vec2Uniform::SourceTransferFunctionParams] = uniformLocation("sourceTransferFunctionParams");
    m_vec2Locations[Vec2Uniform::DestinationTransferFunctionParams] = uniformLocation("destinationTransferFunctionParams");

    m_vec3Locations[Vec3Uniform::PrimaryBrightness] = uniformLocation("primaryBrightness");

    m_vec4Locations[Vec4Uniform::ModulationConstant] = uniformLocation("modulation");

    m_floatLocations[FloatUniform::Saturation] = uniformLocation("saturation");
    m_floatLocations[FloatUniform::MaxDestinationLuminance] = uniformLocation("maxDestinationLuminance");
    m_floatLocations[FloatUniform::SourceReferenceLuminance] = uniformLocation("sourceReferenceLuminance");
    m_floatLocations[FloatUniform::DestinationReferenceLuminance] = uniformLocation("destinationReferenceLuminance");
    m_floatLocations[FloatUniform::MaxTonemappingLuminance] = uniformLocation("maxTonemappingLuminance");

    m_colorLocations[ColorUniform::Color] = uniformLocation("geometryColor");

    m_intLocations[IntUniform::TextureWidth] = uniformLocation("textureWidth");
    m_intLocations[IntUniform::TextureHeight] = uniformLocation("textureHeight");
    m_intLocations[IntUniform::Sampler] = uniformLocation("sampler");
    m_intLocations[IntUniform::Sampler1] = uniformLocation("sampler1");

    m_intLocations[IntUniform::SourceNamedTransferFunction] = uniformLocation("sourceNamedTransferFunction");
    m_intLocations[IntUniform::DestinationNamedTransferFunction] = uniformLocation("destinationNamedTransferFunction");

    m_locationsResolved = true;
}

int GLShader::uniformLocation(const char *name)
{
    const int location = glGetUniformLocation(m_program, name);
    return location;
}

bool GLShader::setUniform(Mat3Uniform uniform, const QMatrix3x3 &value)
{
    resolveLocations();
    return setUniform(m_matrix3Locations[uniform], value);
}

bool GLShader::setUniform(Mat4Uniform uniform, const QMatrix4x4 &matrix)
{
    resolveLocations();
    return setUniform(m_matrix4Locations[uniform], matrix);
}

bool GLShader::setUniform(Vec2Uniform uniform, const QVector2D &value)
{
    resolveLocations();
    return setUniform(m_vec2Locations[uniform], value);
}

bool GLShader::setUniform(Vec3Uniform uniform, const QVector3D &value)
{
    resolveLocations();
    return setUniform(m_vec3Locations[uniform], value);
}

bool GLShader::setUniform(Vec4Uniform uniform, const QVector4D &value)
{
    resolveLocations();
    return setUniform(m_vec4Locations[uniform], value);
}

bool GLShader::setUniform(FloatUniform uniform, float value)
{
    resolveLocations();
    return setUniform(m_floatLocations[uniform], value);
}

bool GLShader::setUniform(IntUniform uniform, int value)
{
    resolveLocations();
    return setUniform(m_intLocations[uniform], value);
}

bool GLShader::setUniform(ColorUniform uniform, const QVector4D &value)
{
    resolveLocations();
    return setUniform(m_colorLocations[uniform], value);
}

bool GLShader::setUniform(ColorUniform uniform, const QColor &value)
{
    resolveLocations();
    return setUniform(m_colorLocations[uniform], value);
}

bool GLShader::setUniform(const char *name, float value)
{
    const int location = uniformLocation(name);
    return setUniform(location, value);
}

bool GLShader::setUniform(const char *name, double value)
{
    const int location = uniformLocation(name);
    return setUniform(location, value);
}

bool GLShader::setUniform(const char *name, int value)
{
    const int location = uniformLocation(name);
    return setUniform(location, value);
}

bool GLShader::setUniform(const char *name, const QVector2D &value)
{
    const int location = uniformLocation(name);
    return setUniform(location, value);
}

bool GLShader::setUniform(const char *name, const QVector3D &value)
{
    const int location = uniformLocation(name);
    return setUniform(location, value);
}

bool GLShader::setUniform(const char *name, const QVector4D &value)
{
    const int location = uniformLocation(name);
    return setUniform(location, value);
}

bool GLShader::setUniform(const char *name, const QMatrix3x3 &value)
{
    const int location = uniformLocation(name);
    return setUniform(location, value);
}

bool GLShader::setUniform(const char *name, const QMatrix4x4 &value)
{
    const int location = uniformLocation(name);
    return setUniform(location, value);
}

bool GLShader::setUniform(const char *name, const QColor &color)
{
    const int location = uniformLocation(name);
    return setUniform(location, color);
}

bool GLShader::setUniform(int location, float value)
{
    if (location >= 0) {
        glUniform1f(location, value);
    }
    return (location >= 0);
}

bool GLShader::setUniform(int location, double value)
{
    if (location >= 0) {
        glUniform1f(location, value);
    }
    return (location >= 0);
}

bool GLShader::setUniform(int location, int value)
{
    if (location >= 0) {
        glUniform1i(location, value);
    }
    return (location >= 0);
}

bool GLShader::setUniform(int location, int xValue, int yValue, int zValue)
{
    if (location >= 0) {
        glUniform3i(location, xValue, yValue, zValue);
    }
    return location >= 0;
}

bool GLShader::setUniform(int location, const QVector2D &value)
{
    if (location >= 0) {
        glUniform2fv(location, 1, (const GLfloat *)&value);
    }
    return (location >= 0);
}

bool GLShader::setUniform(int location, const QVector3D &value)
{
    if (location >= 0) {
        glUniform3fv(location, 1, (const GLfloat *)&value);
    }
    return (location >= 0);
}

bool GLShader::setUniform(int location, const QVector4D &value)
{
    if (location >= 0) {
        glUniform4fv(location, 1, (const GLfloat *)&value);
    }
    return (location >= 0);
}

bool GLShader::setUniform(int location, const QMatrix3x3 &value)
{
    if (location >= 0) {
        glUniformMatrix3fv(location, 1, GL_FALSE, value.constData());
    }
    return location >= 0;
}

bool GLShader::setUniform(int location, const QMatrix4x4 &value)
{
    if (location >= 0) {
        glUniformMatrix4fv(location, 1, GL_FALSE, value.constData());
    }
    return (location >= 0);
}

bool GLShader::setUniform(int location, const QColor &color)
{
    if (location >= 0) {
        glUniform4f(location, color.redF(), color.greenF(), color.blueF(), color.alphaF());
    }
    return (location >= 0);
}

int GLShader::attributeLocation(const char *name)
{
    int location = glGetAttribLocation(m_program, name);
    return location;
}

bool GLShader::setAttribute(const char *name, float value)
{
    int location = attributeLocation(name);
    if (location >= 0) {
        glVertexAttrib1f(location, value);
    }
    return (location >= 0);
}

QMatrix4x4 GLShader::getUniformMatrix4x4(const char *name)
{
    int location = uniformLocation(name);
    if (location >= 0) {
        GLfloat m[16];
        OpenGlContext::currentContext()->glGetnUniformfv(m_program, location, sizeof(m), m);
        QMatrix4x4 matrix(m[0], m[4], m[8], m[12],
                          m[1], m[5], m[9], m[13],
                          m[2], m[6], m[10], m[14],
                          m[3], m[7], m[11], m[15]);
        matrix.optimize();
        return matrix;
    } else {
        return QMatrix4x4();
    }
}

void GLShader::ensureColorPipelineUniforms()
{
    if (m_colorPipelineUniforms) {
        return;
    }
    ColorPipelineUniforms uniforms;
    uniforms.operationCount = uniformLocation("pipelineOperationCount");
    for (uint32_t i = 0; i < uniforms.operationTypes.size(); i++) {
        const QByteArray varName = QByteArrayLiteral("pipelineOperations[") + QByteArray::number(i) + QByteArrayLiteral("].type");
        uniforms.operationTypes[i] = uniformLocation(varName.data());
    }
    for (uint32_t i = 0; i < uniforms.operationTypes.size(); i++) {
        const QByteArray varName = QByteArrayLiteral("pipelineOperations[") + QByteArray::number(i) + QByteArrayLiteral("].typeIndex");
        uniforms.operationIndices[i] = uniformLocation(varName.data());
    }
    for (uint32_t i = 0; i < uniforms.transferFunctionTypes.size(); i++) {
        const QByteArray varName = QByteArrayLiteral("pipelineTransferFunctions[") + QByteArray::number(i) + QByteArrayLiteral("].type");
        uniforms.transferFunctionTypes[i] = uniformLocation(varName.data());
    }
    for (uint32_t i = 0; i < uniforms.transferFunctionMinLum.size(); i++) {
        const QByteArray varName = QByteArrayLiteral("pipelineTransferFunctions[") + QByteArray::number(i) + QByteArrayLiteral("].minLuminance");
        uniforms.transferFunctionMinLum[i] = uniformLocation(varName.data());
    }
    for (uint32_t i = 0; i < uniforms.transferFunctionMaxMinusMinLum.size(); i++) {
        const QByteArray varName = QByteArrayLiteral("pipelineTransferFunctions[") + QByteArray::number(i) + QByteArrayLiteral("].maxMinusMinLuminance");
        uniforms.transferFunctionMaxMinusMinLum[i] = uniformLocation(varName.data());
    }
    for (uint32_t i = 0; i < uniforms.transferFunctionMaxMinusMinLum.size(); i++) {
        const QByteArray varName = QByteArrayLiteral("pipelineMatrices[") + QByteArray::number(i) + QByteArrayLiteral("]");
        uniforms.matrices[i] = uniformLocation(varName.data());
    }
    uniforms.tonemapper = {
        .maxDestinationLuminance = uniformLocation("pipelineToneMapper.maxDestinationLuminance"),
        .inputRange = uniformLocation("pipelineToneMapper.inputRange"),
        .referenceDimming = uniformLocation("pipelineToneMapper.referenceDimming"),
        .outputReferenceLuminance = uniformLocation("pipelineToneMapper.outputReferenceLuminance"),
    };
    for (uint32_t i = 0; i < uniforms.lut1DSamplers.size(); i++) {
        const QByteArray varName = QByteArrayLiteral("pipelineLut1Ds[") + QByteArray::number(i) + QByteArrayLiteral("].sampler");
        uniforms.lut1DSamplers[i] = uniformLocation(varName.data());
    }
    for (uint32_t i = 0; i < uniforms.lut1DSamplerSizes.size(); i++) {
        const QByteArray varName = QByteArrayLiteral("pipelineLut1Ds[") + QByteArray::number(i) + QByteArrayLiteral("].size");
        uniforms.lut1DSamplerSizes[i] = uniformLocation(varName.data());
    }
    uniforms.lut3d = {
        .sampler = uniformLocation("pipelineLut3D.sampler"),
        .size = uniformLocation("pipelineLut3D.size"),
    };
    m_colorPipelineUniforms = uniforms;
}

bool GLShader::setColorPipelineUniforms(const ColorPipeline &pipeline)
{
    ensureColorPipelineUniforms();
    if (pipeline.ops.size() > m_colorPipelineUniforms->operationTypes.size()) {
        qWarning() << "too many operations!" << pipeline.ops.size();
        return false;
    }
    m_dumbestCache.clear();
    m_dumbestCache2.reset();
    uint32_t tfIndex = 0;
    uint32_t matrixIndex = 0;
    bool toneMapperSet = false;
    uint32_t lut1DIndex = 0;
    bool lut3DSet = false;
    int textureIndex = 2;
    for (uint32_t opIndex = 0; opIndex < pipeline.ops.size(); opIndex++) {
        const auto &op = pipeline.ops[opIndex];
        if (const auto mat = std::get_if<ColorMatrix>(&op.operation)) {
            if (matrixIndex >= m_colorPipelineUniforms->matrices.size()) {
                qCWarning(KWIN_OPENGL, "Color pipeline has too many matrices / multipliers");
                return false;
            }
            setUniform(m_colorPipelineUniforms->operationTypes[opIndex], 2);
            setUniform(m_colorPipelineUniforms->operationIndices[opIndex], int(matrixIndex));
            setUniform(m_colorPipelineUniforms->matrices[matrixIndex], mat->mat);
            matrixIndex++;
        } else if (const auto mult = std::get_if<ColorMultiplier>(&op.operation)) {
            if (matrixIndex >= m_colorPipelineUniforms->matrices.size()) {
                qCWarning(KWIN_OPENGL, "Color pipeline has too many matrices / multipliers");
                return false;
            }
            QMatrix4x4 mat;
            mat.scale(mult->factors.toVector3D());
            mat(3, 3) = mult->factors.w();
            setUniform(m_colorPipelineUniforms->operationTypes[opIndex], 2);
            setUniform(m_colorPipelineUniforms->operationIndices[opIndex], int(matrixIndex));
            setUniform(m_colorPipelineUniforms->matrices[matrixIndex], mat);
            matrixIndex++;
        } else if (const auto tf = std::get_if<ColorTransferFunction>(&op.operation)) {
            if (tfIndex >= m_colorPipelineUniforms->transferFunctionTypes.size()) {
                qCWarning(KWIN_OPENGL, "Color pipeline has too many transfer functions");
                return false;
            }
            setUniform(m_colorPipelineUniforms->operationTypes[opIndex], 0);
            setUniform(m_colorPipelineUniforms->operationIndices[opIndex], int(tfIndex));
            setUniform(m_colorPipelineUniforms->transferFunctionTypes[tfIndex], tf->tf.type);
            setUniform(m_colorPipelineUniforms->transferFunctionMinLum[tfIndex], tf->tf.minLuminance);
            setUniform(m_colorPipelineUniforms->transferFunctionMaxMinusMinLum[tfIndex], tf->tf.maxLuminance - tf->tf.minLuminance);
            tfIndex++;
        } else if (const auto tf = std::get_if<InverseColorTransferFunction>(&op.operation)) {
            if (tfIndex >= m_colorPipelineUniforms->transferFunctionTypes.size()) {
                qCWarning(KWIN_OPENGL, "Color pipeline has too many transfer functions");
                return false;
            }
            setUniform(m_colorPipelineUniforms->operationTypes[opIndex], 1);
            setUniform(m_colorPipelineUniforms->operationIndices[opIndex], int(tfIndex));
            setUniform(m_colorPipelineUniforms->transferFunctionTypes[tfIndex], tf->tf.type);
            setUniform(m_colorPipelineUniforms->transferFunctionMinLum[tfIndex], tf->tf.minLuminance);
            setUniform(m_colorPipelineUniforms->transferFunctionMaxMinusMinLum[tfIndex], tf->tf.maxLuminance - tf->tf.minLuminance);
            tfIndex++;
        } else if (const auto tonemap = std::get_if<ColorTonemapper>(&op.operation)) {
            if (toneMapperSet) {
                qCWarning(KWIN_OPENGL, "Color pipeline has too many tone mappers");
                return false;
            }
            setUniform(m_colorPipelineUniforms->operationTypes[opIndex], 3);
            setUniform(m_colorPipelineUniforms->tonemapper.inputRange, tonemap->m_inputRange);
            setUniform(m_colorPipelineUniforms->tonemapper.maxDestinationLuminance, tonemap->m_maxOutputLuminance);
            setUniform(m_colorPipelineUniforms->tonemapper.outputReferenceLuminance, tonemap->m_outputReferenceLuminance);
            setUniform(m_colorPipelineUniforms->tonemapper.referenceDimming, tonemap->m_referenceDimming);
            toneMapperSet = true;
        } else if (const auto lut1d = std::get_if<std::shared_ptr<ColorTransformation>>(&op.operation)) {
            if (lut1DIndex >= m_colorPipelineUniforms->lut1DSamplers.size()) {
                qCWarning(KWIN_OPENGL, "Color pipeline has too many 1D LUTs");
                return false;
            }
            setUniform(m_colorPipelineUniforms->operationTypes[opIndex], 4);
            // TODO read the actual texture size from the ICC profile, and reduce this size significantly
            auto tex = GlLookUpTable::create([lut = *lut1d](size_t index) {
                const double relative = index / 4095.0;
                return lut->transform(QVector3D(relative, relative, relative));
            }, 4096);
            glActiveTexture(GL_TEXTURE0 + textureIndex);
            tex->bind();
            setUniform(m_colorPipelineUniforms->operationIndices[opIndex], int(lut1DIndex));
            setUniform(m_colorPipelineUniforms->lut1DSamplers[lut1DIndex], textureIndex);
            setUniform(m_colorPipelineUniforms->lut1DSamplerSizes[lut1DIndex], 4096);
            textureIndex++;
            lut1DIndex++;
            m_dumbestCache.push_back(std::move(tex));
        } else if (const auto lut3d = std::get_if<std::shared_ptr<ColorLUT3D>>(&op.operation)) {
            if (lut3DSet) {
                qCWarning(KWIN_OPENGL, "Color pipeline has too many 3D LUTs");
                return false;
            }
            setUniform(m_colorPipelineUniforms->operationTypes[opIndex], 5);
            auto tex = GlLookUpTable3D::create([lut = *lut3d](size_t x, size_t y, size_t z) {
                return lut->sample(x, y, z);
            }, (*lut3d)->xSize(), (*lut3d)->ySize(), (*lut3d)->zSize());
            glActiveTexture(GL_TEXTURE0 + textureIndex);
            tex->bind();
            setUniform(m_colorPipelineUniforms->lut3d.sampler, textureIndex);
            setUniform(m_colorPipelineUniforms->lut3d.size, (*lut3d)->xSize(), (*lut3d)->ySize(), (*lut3d)->zSize());
            textureIndex++;
            lut3DSet = true;
            m_dumbestCache2 = std::move(tex);
        }
    }
    for (uint32_t unused1d = lut1DIndex; unused1d < m_colorPipelineUniforms->lut1DSamplers.size(); unused1d++) {
        glActiveTexture(GL_TEXTURE0 + textureIndex);
        glBindTexture(GL_TEXTURE_2D, 0);
        setUniform(m_colorPipelineUniforms->lut1DSamplers[unused1d], textureIndex);
        setUniform(m_colorPipelineUniforms->lut1DSamplerSizes[unused1d], 4096);
        textureIndex++;
    }
    if (!lut3DSet) {
        glActiveTexture(GL_TEXTURE0 + textureIndex);
        glBindTexture(GL_TEXTURE_3D, 0);
        setUniform(m_colorPipelineUniforms->lut3d.sampler, textureIndex);
        setUniform(m_colorPipelineUniforms->lut3d.size, 32, 32, 32);
        textureIndex++;
    }
    setUniform(m_colorPipelineUniforms->operationCount, int(pipeline.ops.size()));
    glActiveTexture(GL_TEXTURE0);
    m_currentPipeline = pipeline;
    return true;
}

bool GLShader::setColorPipelineUniforms(const ColorDescription &src, const ColorDescription &dst, RenderingIntent intent)
{
    return setColorPipelineUniforms(ColorPipeline::create(src, dst, intent));
}

static bool s_disableTonemapping = qEnvironmentVariableIntValue("KWIN_DISABLE_TONEMAPPING") == 1;

void GLShader::setLegacyColorspaceUniforms(const ColorDescription &src, const ColorDescription &dst, RenderingIntent intent)
{
    resolveLocations();
    setUniform(Mat4Uniform::ColorimetryTransformation, src.toOther(dst, intent));
    setUniform(IntUniform::SourceNamedTransferFunction, src.transferFunction().type);
    setUniform(Vec2Uniform::SourceTransferFunctionParams, QVector2D(src.transferFunction().minLuminance, src.transferFunction().maxLuminance - src.transferFunction().minLuminance));
    setUniform(FloatUniform::SourceReferenceLuminance, src.referenceLuminance());
    setUniform(IntUniform::DestinationNamedTransferFunction, dst.transferFunction().type);
    setUniform(Vec2Uniform::DestinationTransferFunctionParams, QVector2D(dst.transferFunction().minLuminance, dst.transferFunction().maxLuminance - dst.transferFunction().minLuminance));
    setUniform(FloatUniform::DestinationReferenceLuminance, dst.referenceLuminance());
    setUniform(FloatUniform::MaxDestinationLuminance, dst.maxHdrLuminance().value_or(10'000));
    if (!s_disableTonemapping && intent == RenderingIntent::Perceptual) {
        setUniform(FloatUniform::MaxTonemappingLuminance, src.maxHdrLuminance().value_or(src.referenceLuminance()) * dst.referenceLuminance() / src.referenceLuminance());
    } else {
        setUniform(FloatUniform::MaxTonemappingLuminance, dst.referenceLuminance());
    }
    setUniform(Mat4Uniform::DestinationToLMS, dst.containerColorimetry().toLMS());
    setUniform(Mat4Uniform::LMSToDestination, dst.containerColorimetry().fromLMS());
}
}
