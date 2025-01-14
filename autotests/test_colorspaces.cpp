/*
    SPDX-FileCopyrightText: 2023 Xaver Hugl <xaver.hugl@gmail.com>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include <QImage>
#include <QTest>

#include "core/colorpipeline.h"
#include "core/colorspace.h"
#include "core/iccprofile.h"
#include "opengl/eglcontext.h"
#include "opengl/egldisplay.h"
#include "opengl/glframebuffer.h"
#include "opengl/glshader.h"
#include "opengl/glshadermanager.h"
#include "opengl/icc_shader.h"

#include <lcms2.h>

using namespace KWin;

class TestColorspaces : public QObject
{
    Q_OBJECT

public:
    TestColorspaces() = default;

private Q_SLOTS:
    void roundtripConversion_data();
    void roundtripConversion();
    void testXYZ_XYconversions();
    void testIdentityTransformation_data();
    void testIdentityTransformation();
    void testColorPipeline_data();
    void testColorPipeline();
    void testXYZ();
    void testOpenglShader_data();
    void testOpenglShader();
    void testIccShader_data();
    void testIccShader();
    void dontCrashWithWeirdHdrMetadata();
};

static bool compareVectors(const QVector3D &one, const QVector3D &two, float maxDifference)
{
    const bool ret = std::abs(one.x() - two.x()) <= maxDifference
        && std::abs(one.y() - two.y()) <= maxDifference
        && std::abs(one.z() - two.z()) <= maxDifference;
    if (!ret) {
        qWarning() << one << "!=" << two << "within" << maxDifference;
    }
    return ret;
}

static const double s_resolution10bit = std::pow(1.0 / 2.0, 10);

void TestColorspaces::roundtripConversion_data()
{
    QTest::addColumn<NamedColorimetry>("srcColorimetry");
    QTest::addColumn<TransferFunction::Type>("srcTransferFunction");
    QTest::addColumn<NamedColorimetry>("dstColorimetry");
    QTest::addColumn<TransferFunction::Type>("dstTransferFunction");
    QTest::addColumn<double>("requiredAccuracy");

    QTest::addRow("BT709 (sRGB) <-> BT2020 (linear)") << NamedColorimetry::BT709 << TransferFunction::sRGB << NamedColorimetry::BT2020 << TransferFunction::linear << s_resolution10bit;
    QTest::addRow("BT709 (gamma 2.2) <-> BT2020 (linear)") << NamedColorimetry::BT709 << TransferFunction::gamma22 << NamedColorimetry::BT2020 << TransferFunction::linear << s_resolution10bit;
    QTest::addRow("BT709 (linear) <-> BT2020 (linear)") << NamedColorimetry::BT709 << TransferFunction::linear << NamedColorimetry::BT2020 << TransferFunction::linear << s_resolution10bit;
    QTest::addRow("BT709 (PQ) <-> BT2020 (linear)") << NamedColorimetry::BT709 << TransferFunction::PerceptualQuantizer << NamedColorimetry::BT2020 << TransferFunction::linear << 3 * s_resolution10bit;
}

void TestColorspaces::roundtripConversion()
{
    QFETCH(NamedColorimetry, srcColorimetry);
    QFETCH(TransferFunction::Type, srcTransferFunction);
    QFETCH(NamedColorimetry, dstColorimetry);
    QFETCH(TransferFunction::Type, dstTransferFunction);
    QFETCH(double, requiredAccuracy);

    const auto src = ColorDescription(srcColorimetry, TransferFunction(srcTransferFunction), 100, 0, 100, 100);
    const auto dst = ColorDescription(dstColorimetry, TransferFunction(dstTransferFunction), 100, 0, 100, 100);

    const QVector3D red(1, 0, 0);
    const QVector3D green(0, 1, 0);
    const QVector3D blue(0, 0, 1);
    const QVector3D white(1, 1, 1);
    constexpr std::array renderingIntents = {
        RenderingIntent::RelativeColorimetric,
        RenderingIntent::AbsoluteColorimetric,
    };
    for (const RenderingIntent intent : renderingIntents) {
        QVERIFY(compareVectors(dst.mapTo(src.mapTo(red, dst, intent), src, intent), red, requiredAccuracy));
        QVERIFY(compareVectors(dst.mapTo(src.mapTo(green, dst, intent), src, intent), green, requiredAccuracy));
        QVERIFY(compareVectors(dst.mapTo(src.mapTo(blue, dst, intent), src, intent), blue, requiredAccuracy));
        QVERIFY(compareVectors(dst.mapTo(src.mapTo(white, dst, intent), src, intent), white, requiredAccuracy));
    }
}

void TestColorspaces::testXYZ_XYconversions()
{
    // this test ensures that xyY<->XYZ conversions can handle weird inputs
    // and don't cause crashes
    QCOMPARE(XYZ(0, 0, 0).toxyY(), xyY(0, 0, 1));
    QCOMPARE_LE(XYZ(100, 100, 100).toxyY().y, 1);
    QCOMPARE(xyY(0, 0, 1).toXYZ(), XYZ(0, 0, 0));
    QCOMPARE(xyY(1, 0, 1).toXYZ(), XYZ(0, 0, 0));
}

void TestColorspaces::testIdentityTransformation_data()
{
    QTest::addColumn<NamedColorimetry>("colorimetry");
    QTest::addColumn<TransferFunction::Type>("transferFunction");

    QTest::addRow("BT709 (sRGB)") << NamedColorimetry::BT709 << TransferFunction::sRGB;
    QTest::addRow("BT709 (gamma22)") << NamedColorimetry::BT709 << TransferFunction::gamma22;
    QTest::addRow("BT709 (PQ)") << NamedColorimetry::BT709 << TransferFunction::PerceptualQuantizer;
    QTest::addRow("BT709 (linear)") << NamedColorimetry::BT709 << TransferFunction::linear;
    QTest::addRow("BT2020 (sRGB)") << NamedColorimetry::BT2020 << TransferFunction::sRGB;
    QTest::addRow("BT2020 (gamma22)") << NamedColorimetry::BT2020 << TransferFunction::gamma22;
    QTest::addRow("BT2020 (PQ)") << NamedColorimetry::BT2020 << TransferFunction::PerceptualQuantizer;
    QTest::addRow("BT2020 (linear)") << NamedColorimetry::BT2020 << TransferFunction::linear;
}

void TestColorspaces::testIdentityTransformation()
{
    QFETCH(NamedColorimetry, colorimetry);
    QFETCH(TransferFunction::Type, transferFunction);
    const TransferFunction tf(transferFunction);
    const ColorDescription src(colorimetry, tf, 100, tf.minLuminance, tf.maxLuminance, tf.maxLuminance);
    const TransferFunction tf2(transferFunction, tf.minLuminance * 1.1, tf.maxLuminance * 1.1);
    const ColorDescription dst(colorimetry, tf2, 110, tf2.minLuminance, tf2.maxLuminance, tf2.maxLuminance);

    constexpr std::array renderingIntents = {
        RenderingIntent::Perceptual,
        RenderingIntent::RelativeColorimetric,
        RenderingIntent::AbsoluteColorimetric,
        RenderingIntent::RelativeColorimetricWithBPC,
    };
    for (const RenderingIntent intent : renderingIntents) {
        const auto pipeline = ColorPipeline::create(src, dst, intent);
        if (!pipeline.isIdentity()) {
            qWarning() << pipeline;
        }
        QVERIFY(pipeline.isIdentity());
    }
}

void TestColorspaces::testColorPipeline_data()
{
    QTest::addColumn<ColorDescription>("srcColor");
    QTest::addColumn<ColorDescription>("dstColor");
    QTest::addColumn<QVector3D>("dstBlack");
    QTest::addColumn<QVector3D>("dstGray");
    QTest::addColumn<QVector3D>("dstWhite");
    QTest::addColumn<RenderingIntent>("intent");

    QTest::addRow("sRGB -> rec.2020 relative colorimetric")
        << ColorDescription(NamedColorimetry::BT709, TransferFunction(TransferFunction::gamma22), TransferFunction::defaultReferenceLuminanceFor(TransferFunction::gamma22), 0, std::nullopt, std::nullopt)
        << ColorDescription(NamedColorimetry::BT2020, TransferFunction(TransferFunction::PerceptualQuantizer), 500, 0, std::nullopt, std::nullopt)
        << QVector3D(0.06729, 0.06729, 0.06729)
        << QVector3D(0.51667, 0.51667, 0.51667)
        << QVector3D(0.67658, 0.67658, 0.67658)
        << RenderingIntent::RelativeColorimetric;
    QTest::addRow("sRGB -> scRGB relative colorimetric")
        << ColorDescription(NamedColorimetry::BT709, TransferFunction(TransferFunction::gamma22), TransferFunction::defaultReferenceLuminanceFor(TransferFunction::gamma22), 0, std::nullopt, std::nullopt)
        << ColorDescription(NamedColorimetry::BT709, TransferFunction(TransferFunction::linear, 0, 80), 80, 0, std::nullopt, std::nullopt)
        << QVector3D(0.00025, 0.00025, 0.00025)
        << QVector3D(0.217833, 0.217833, 0.217833)
        << QVector3D(1, 1, 1)
        << RenderingIntent::RelativeColorimetric;
    QTest::addRow("sRGB -> rec.2020 relative colorimetric with bpc")
        << ColorDescription(NamedColorimetry::BT709, TransferFunction(TransferFunction::gamma22, 0.2, 80), 80, 0.2, std::nullopt, std::nullopt)
        << ColorDescription(NamedColorimetry::BT2020, TransferFunction(TransferFunction::PerceptualQuantizer, 0.005, 10'000), 500, 0.005, std::nullopt, std::nullopt)
        << QVector3D(0, 0, 0)
        << QVector3D(0.51667, 0.51667, 0.51667)
        << QVector3D(0.67658, 0.67658, 0.67658)
        << RenderingIntent::RelativeColorimetricWithBPC;
    QTest::addRow("scRGB -> scRGB relative colorimetric with bpc")
        << ColorDescription(NamedColorimetry::BT709, TransferFunction(TransferFunction::linear, 0, 80), TransferFunction::defaultReferenceLuminanceFor(TransferFunction::gamma22), 0, std::nullopt, std::nullopt)
        << ColorDescription(NamedColorimetry::BT709, TransferFunction(TransferFunction::linear, 8, 80), 80, 8, std::nullopt, std::nullopt)
        << QVector3D(0, 0, 0)
        << QVector3D(0.5, 0.5, 0.5)
        << QVector3D(1, 1, 1)
        << RenderingIntent::RelativeColorimetricWithBPC;
}

void TestColorspaces::testColorPipeline()
{
    QFETCH(ColorDescription, srcColor);
    QFETCH(ColorDescription, dstColor);
    QFETCH(QVector3D, dstBlack);
    QFETCH(QVector3D, dstGray);
    QFETCH(QVector3D, dstWhite);
    QFETCH(RenderingIntent, intent);

    const auto pipeline = ColorPipeline::create(srcColor, dstColor, intent);
    QVERIFY(compareVectors(pipeline.evaluate(QVector3D(0, 0, 0)), dstBlack, s_resolution10bit));
    QVERIFY(compareVectors(pipeline.evaluate(QVector3D(0.5, 0.5, 0.5)), dstGray, s_resolution10bit));
    QVERIFY(compareVectors(pipeline.evaluate(QVector3D(1, 1, 1)), dstWhite, s_resolution10bit));

    const auto inversePipeline = ColorPipeline::create(dstColor, srcColor, intent);
    QVERIFY(compareVectors(inversePipeline.evaluate(dstBlack), QVector3D(0, 0, 0), s_resolution10bit));
    QVERIFY(compareVectors(inversePipeline.evaluate(dstGray), QVector3D(0.5, 0.5, 0.5), s_resolution10bit));
    QVERIFY(compareVectors(inversePipeline.evaluate(dstWhite), QVector3D(1, 1, 1), s_resolution10bit));
}

void TestColorspaces::testXYZ()
{
    Colorimetry xyz = Colorimetry::fromName(NamedColorimetry::CIEXYZ);
    QVERIFY(isFuzzyIdentity(xyz.toXYZ()));
    QVERIFY(isFuzzyIdentity(xyz.fromXYZ()));
}

void TestColorspaces::testOpenglShader_data()
{
    QTest::addColumn<RenderingIntent>("intent");
    QTest::addColumn<double>("maxError");

    // the allowed error here needs to be this high because of llvmpipe. With real GPU drivers it's lower
    QTest::addRow("Perceptual") << RenderingIntent::Perceptual << 7.0;
    QTest::addRow("RelativeColorimetric") << RenderingIntent::RelativeColorimetric << 1.5;
    QTest::addRow("AbsoluteColorimetric") << RenderingIntent::AbsoluteColorimetric << 1.5;
    QTest::addRow("RelativeColorimetricWithBPC") << RenderingIntent::RelativeColorimetricWithBPC << 1.5;
}

void TestColorspaces::testOpenglShader()
{
    QFETCH(RenderingIntent, intent);
    QFETCH(double, maxError);

    const auto display = EglDisplay::create(eglGetDisplay(EGL_DEFAULT_DISPLAY));
    const auto context = EglContext::create(display.get(), EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT);

    const ColorDescription src(NamedColorimetry::BT709, TransferFunction(TransferFunction::linear, 0, 400), 100, 0, 200, 400);
    const ColorDescription dst(NamedColorimetry::BT709, TransferFunction(TransferFunction::gamma22), 100, 0, 100, 100);

    QImage input(255, 255, QImage::Format_RGBA8888_Premultiplied);
    for (int x = 0; x < input.width(); x++) {
        for (int y = 0; y < input.height(); y++) {
            input.setPixel(x, y, qRgba(x, y, 0, 255));
        }
    }

    QImage openGlResult;
    {
        ShaderBinder binder(ShaderTrait::MapTexture | ShaderTrait::TransformColorspace);
        QMatrix4x4 proj;
        proj.ortho(QRectF(0, 0, input.width(), input.height()));
        binder.shader()->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, proj);
        binder.shader()->setColorspaceUniforms(src, dst, intent);
        const auto target = GLTexture::allocate(GL_RGBA8, input.size());
        GLFramebuffer buffer(target.get());
        context->pushFramebuffer(&buffer);

        const auto srcColor = GLTexture::upload(input);
        srcColor->render(input.size());

        context->popFramebuffer();
        openGlResult = target->toImage();
        openGlResult.mirror();
    }
    QImage pipelineResult(input.width(), input.height(), QImage::Format_RGBA8888_Premultiplied);
    {
        const auto pipeline = ColorPipeline::create(src, dst, intent);
        for (int x = 0; x < input.width(); x++) {
            for (int y = 0; y < input.height(); y++) {
                const auto pixel = input.pixel(x, y);
                const QVector3D colors = QVector3D(qRed(pixel), qGreen(pixel), qBlue(pixel)) / 255;
                const QVector3D output = pipeline.evaluate(colors) * 255;
                pipelineResult.setPixel(x, y, qRgba(std::round(output.x()), std::round(output.y()), std::round(output.z()), 255));
            }
        }
    }

    for (int x = 0; x < input.width(); x++) {
        for (int y = 0; y < input.height(); y++) {
            const auto glPixel = openGlResult.pixel(x, y);
            const QVector3D glColors = QVector3D(qRed(glPixel), qGreen(glPixel), qBlue(glPixel));
            const auto pipePixel = pipelineResult.pixel(x, y);
            const QVector3D pipeColors = QVector3D(qRed(pipePixel), qGreen(pipePixel), qBlue(pipePixel));
            const QVector3D difference = (glColors - pipeColors);
            QCOMPARE_LE(difference.length(), maxError);
        }
    }
}

void TestColorspaces::testIccShader_data()
{
    QTest::addColumn<QString>("iccProfilePath");
    QTest::addColumn<RenderingIntent>("intent");
    QTest::addColumn<uint32_t>("lcmsIntent");
    QTest::addColumn<int>("maxAllowedError");

    const auto F13 = QFINDTESTDATA("data/Framework 13.icc");
    const auto Samsung = QFINDTESTDATA("data/Samsung CRG49 Shaper Matrix.icc");
    QTest::addRow("relative colorimetric Framework 13") << F13 << RenderingIntent::RelativeColorimetric << uint32_t(INTENT_RELATIVE_COLORIMETRIC) << 6;
    QTest::addRow("absolute colorimetric Framework 13") << F13 << RenderingIntent::AbsoluteColorimetric << uint32_t(INTENT_ABSOLUTE_COLORIMETRIC) << 6;
    // FIXME why is this further away from the LCMS result? It's way simpler to calculate...
    QTest::addRow("relative colorimetric CRG49") << Samsung << RenderingIntent::RelativeColorimetric << uint32_t(INTENT_RELATIVE_COLORIMETRIC) << 13;
    QTest::addRow("absolute colorimetric CRG49") << Samsung << RenderingIntent::AbsoluteColorimetric << uint32_t(INTENT_ABSOLUTE_COLORIMETRIC) << 13;
}

void TestColorspaces::testIccShader()
{
    const auto display = EglDisplay::create(eglGetDisplay(EGL_DEFAULT_DISPLAY));
    const auto context = EglContext::create(display.get(), EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT);

    QImage input(255, 255, QImage::Format_RGBA8888_Premultiplied);
    for (int x = 0; x < input.width(); x++) {
        for (int y = 0; y < input.height(); y++) {
            input.setPixel(x, y, qRgba(x, y, (x + y) / 2, 255));
        }
    }

    QFETCH(QString, iccProfilePath);
    QFETCH(RenderingIntent, intent);
    QFETCH(uint32_t, lcmsIntent);

    const std::shared_ptr<IccProfile> profile = IccProfile::load(iccProfilePath).profile.value_or(nullptr);
    QVERIFY(profile);

    QImage pipelineResult(input.width(), input.height(), QImage::Format_RGBA8888_Premultiplied);
    {
        // by default, LCMS uses adaption state 1.0 for absolute colorimetric,
        // also known as relative colorimetric... we don't want that
        cmsSetAdaptationState(0);

        const auto toCmsxyY = [](const xyY &primary) {
            return cmsCIExyY{
                .x = primary.x,
                .y = primary.y,
                .Y = primary.Y,
            };
        };
        const cmsCIExyY sRGBWhite = toCmsxyY(Colorimetry::fromName(NamedColorimetry::BT709).white().toxyY());
        const cmsCIExyYTRIPLE sRGBPrimaries{
            .Red = toCmsxyY(Colorimetry::fromName(NamedColorimetry::BT709).red().toxyY()),
            .Green = toCmsxyY(Colorimetry::fromName(NamedColorimetry::BT709).green().toxyY()),
            .Blue = toCmsxyY(Colorimetry::fromName(NamedColorimetry::BT709).blue().toxyY()),
        };
        const std::array<double, 10> params = {2.2};
        const std::array toneCurves = {
            cmsBuildParametricToneCurve(nullptr, 1, params.data()),
            cmsBuildParametricToneCurve(nullptr, 1, params.data()),
            cmsBuildParametricToneCurve(nullptr, 1, params.data()),
        };
        // note that we can't just use cmsCreate_sRGBProfile here
        // as that uses the sRGB piece-wise transfer function, which is not correct for our use case
        cmsHPROFILE sRGBHandle = cmsCreateRGBProfile(&sRGBWhite, &sRGBPrimaries, toneCurves.data());

        cmsHPROFILE handle = cmsOpenProfileFromFile(iccProfilePath.toUtf8(), "r");
        QVERIFY(handle);

        const auto transform = cmsCreateTransform(sRGBHandle, TYPE_RGB_8, handle, TYPE_RGB_8, lcmsIntent, cmsFLAGS_NOOPTIMIZE);
        QVERIFY(transform);

        for (int x = 0; x < input.width(); x++) {
            for (int y = 0; y < input.height(); y++) {
                const auto pixel = input.pixel(x, y);
                std::array<uint8_t, 3> in = {
                    uint8_t(qRed(pixel)),
                    uint8_t(qGreen(pixel)),
                    uint8_t(qBlue(pixel)),
                };
                std::array<uint8_t, 3> out = {0, 0, 0};
                cmsDoTransform(transform, in.data(), out.data(), 1);
                pipelineResult.setPixel(x, y, qRgba(out[0], out[1], out[2], 255));
            }
        }
        cmsDeleteTransform(transform);
        cmsCloseProfile(sRGBHandle);
        cmsCloseProfile(handle);
    }

    QImage openGlResult;
    {
        IccShader shader;
        ShaderBinder binder{shader.shader()};
        shader.setUniforms(profile, ColorDescription::sRGB, intent);

        QMatrix4x4 proj;
        proj.ortho(QRectF(0, 0, input.width(), input.height()));
        binder.shader()->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, proj);
        const auto target = GLTexture::allocate(GL_RGBA8, input.size());
        GLFramebuffer buffer(target.get());
        context->pushFramebuffer(&buffer);

        const auto srcColor = GLTexture::upload(input);
        srcColor->render(input.size());

        context->popFramebuffer();
        openGlResult = target->toImage();
        openGlResult.mirror();
    }

    float maxError = 0;
    for (int x = 0; x < input.width(); x++) {
        for (int y = 0; y < input.height(); y++) {
            const auto glPixel = openGlResult.pixel(x, y);
            const QVector3D glColors = QVector3D(qRed(glPixel), qGreen(glPixel), qBlue(glPixel));
            const auto pipePixel = pipelineResult.pixel(x, y);
            const QVector3D pipeColors = QVector3D(qRed(pipePixel), qGreen(pipePixel), qBlue(pipePixel));
            const QVector3D difference = (glColors - pipeColors);

            maxError = std::max(difference.length(), maxError);
        }
    }

    qWarning() << "Max ICC shader error:" << maxError;
    QFETCH(int, maxAllowedError);
    QCOMPARE_LE(maxError, maxAllowedError);
}

void TestColorspaces::dontCrashWithWeirdHdrMetadata()
{
    // verify that weird display metadata with max. luminance < reference luminance
    // doesn't crash KWin
    ColorDescription in(NamedColorimetry::BT709, TransferFunction(TransferFunction::linear, 0, 80), 80, 0, 60, 60);
    ColorDescription out(NamedColorimetry::BT709, TransferFunction(TransferFunction::linear, 0, 80), 80, 0, 40, 40);
    const auto pipeline = ColorPipeline::create(in, out, RenderingIntent::Perceptual);
    QCOMPARE(pipeline.evaluate(QVector3D()), QVector3D());
}

QTEST_MAIN(TestColorspaces)

#include "test_colorspaces.moc"
