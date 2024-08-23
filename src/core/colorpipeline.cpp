/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2024 Xaver Hugl <xaver.hugl@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "colorpipeline.h"

#include <numbers>

namespace KWin
{

ValueRange ValueRange::operator*(double mult) const
{
    return ValueRange{
        .min = min * mult,
        .max = max * mult,
    };
}

static bool s_disableTonemapping = qEnvironmentVariableIntValue("KWIN_DISABLE_TONEMAPPING") == 1;

ColorPipeline ColorPipeline::create(const ColorDescription &from, const ColorDescription &to, RenderingIntent intent)
{
    const auto range1 = ValueRange(from.minLuminance(), from.maxHdrLuminance().value_or(from.referenceLuminance()));
    if (std::isnan(range1.max)) {
        qWarning() << "fuck" << from.maxHdrLuminance().value_or(-1) << from.referenceLuminance();
    }
    const double maxOutputLuminance = to.maxHdrLuminance().value_or(to.referenceLuminance());
    ColorPipeline ret(ValueRange{
        .min = from.transferFunction().nitsToEncoded(range1.min),
        .max = from.transferFunction().nitsToEncoded(range1.max),
    });
    ret.addTransferFunction(from.transferFunction());

    // FIXME this assumes that the range stays the same with matrix multiplication
    // that's not necessarily true, and figuring out the actual range could be complicated..
    ret.addMatrix(from.toOther(to, intent), ret.currentOutputRange() * (to.referenceLuminance() / from.referenceLuminance()));
    if (!s_disableTonemapping && ret.currentOutputRange().max > maxOutputLuminance * 1.01 && intent == RenderingIntent::Perceptual) {
        ret.addTonemapper(to.containerColorimetry(), to.referenceLuminance(), ret.currentOutputRange().max, maxOutputLuminance, 1.5);
    }

    ret.addInverseTransferFunction(to.transferFunction());
    return ret;
}

ColorPipeline::ColorPipeline()
    : inputRange(ValueRange{
          .min = 0,
          .max = 1,
      })
{
}

ColorPipeline::ColorPipeline(const ValueRange &inputRange)
    : inputRange(inputRange)
{
}

const ValueRange &ColorPipeline::currentOutputRange() const
{
    return ops.empty() ? inputRange : ops.back().output;
}

void ColorPipeline::addMultiplier(double factor)
{
    addMultiplier(QVector3D(factor, factor, factor));
}

void ColorPipeline::addMultiplier(const QVector3D &factors)
{
    if (factors == QVector3D(1, 1, 1)) {
        return;
    }
    const ValueRange output{
        .min = currentOutputRange().min * std::min(factors.x(), std::min(factors.y(), factors.z())),
        .max = currentOutputRange().max * std::max(factors.x(), std::max(factors.y(), factors.z())),
    };
    if (!ops.empty()) {
        auto *lastOp = &ops.back().operation;
        if (const auto mat = std::get_if<ColorMatrix>(lastOp)) {
            auto newMat = mat->mat;
            newMat.scale(factors);
            ops.erase(ops.end() - 1);
            addMatrix(newMat, output);
            return;
        } else if (const auto mult = std::get_if<ColorMultiplier>(lastOp)) {
            mult->factors *= factors;
            if ((mult->factors - QVector3D(1, 1, 1)).lengthSquared() < s_maxResolution * s_maxResolution) {
                ops.erase(ops.end() - 1);
            } else {
                ops.back().output = output;
            }
            return;
        } else if (std::abs(factors.x() - factors.y()) < s_maxResolution && std::abs(factors.x() - factors.z()) < s_maxResolution) {
            if (const auto tf = std::get_if<ColorTransferFunction>(lastOp)) {
                tf->tf.minLuminance *= factors.x();
                tf->tf.maxLuminance *= factors.x();
                ops.back().output = output;
                return;
            } else if (const auto tf = std::get_if<InverseColorTransferFunction>(lastOp)) {
                tf->tf.minLuminance /= factors.x();
                tf->tf.maxLuminance /= factors.x();
                ops.back().output = output;
                return;
            }
        }
    }
    ops.push_back(ColorOp{
        .input = currentOutputRange(),
        .operation = ColorMultiplier(factors),
        .output = output,
    });
}

void ColorPipeline::addTransferFunction(TransferFunction tf)
{
    if (!ops.empty()) {
        if (const auto invTf = std::get_if<InverseColorTransferFunction>(&ops.back().operation)) {
            if (invTf->tf == tf) {
                ops.erase(ops.end() - 1);
                return;
            }
        }
    }
    if (tf.type == TransferFunction::linear) {
        QMatrix4x4 mat;
        mat.translate(tf.minLuminance, tf.minLuminance, tf.minLuminance);
        mat.scale(tf.maxLuminance - tf.minLuminance);
        addMatrix(mat, ValueRange{
                           .min = (mat * QVector3D(currentOutputRange().min, 0, 0)).x(),
                           .max = (mat * QVector3D(currentOutputRange().max, 0, 0)).x(),
                       });
    } else {
        ops.push_back(ColorOp{
            .input = currentOutputRange(),
            .operation = ColorTransferFunction(tf),
            .output = ValueRange{
                .min = tf.encodedToNits(currentOutputRange().min),
                .max = tf.encodedToNits(currentOutputRange().max),
            },
        });
    }
}

void ColorPipeline::addInverseTransferFunction(TransferFunction tf)
{
    if (!ops.empty()) {
        if (const auto otherTf = std::get_if<ColorTransferFunction>(&ops.back().operation)) {
            if (otherTf->tf == tf) {
                ops.erase(ops.end() - 1);
                return;
            }
        }
    }
    if (tf.type == TransferFunction::linear) {
        QMatrix4x4 mat;
        mat.scale(1.0 / (tf.maxLuminance - tf.minLuminance));
        mat.translate(-tf.minLuminance, -tf.minLuminance, -tf.minLuminance);
        addMatrix(mat, ValueRange{
                           .min = (mat * QVector3D(currentOutputRange().min, 0, 0)).x(),
                           .max = (mat * QVector3D(currentOutputRange().max, 0, 0)).x(),
                       });
    } else {
        ops.push_back(ColorOp{
            .input = currentOutputRange(),
            .operation = InverseColorTransferFunction(tf),
            .output = ValueRange{
                .min = tf.nitsToEncoded(currentOutputRange().min),
                .max = tf.nitsToEncoded(currentOutputRange().max),
            },
        });
    }
}

static bool isFuzzyIdentity(const QMatrix4x4 &mat)
{
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            const float targetValue = i == j ? 1 : 0;
            if (std::abs(mat(i, j) - targetValue) > ColorPipeline::s_maxResolution) {
                return false;
            }
        }
    }
    return true;
}

static bool isFuzzyScalingOnly(const QMatrix4x4 &mat)
{
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            if (i == j) {
                continue;
            }
            if (std::abs(mat(i, j)) > ColorPipeline::s_maxResolution) {
                return false;
            }
        }
    }
    return true;
}

void ColorPipeline::addMatrix(const QMatrix4x4 &mat, const ValueRange &output)
{
    if (isFuzzyIdentity(mat)) {
        return;
    }
    if (!ops.empty()) {
        auto *lastOp = &ops.back().operation;
        if (const auto otherMat = std::get_if<ColorMatrix>(lastOp)) {
            const auto newMat = mat * otherMat->mat;
            ops.erase(ops.end() - 1);
            addMatrix(newMat, output);
            return;
        } else if (const auto mult = std::get_if<ColorMultiplier>(lastOp)) {
            QMatrix4x4 scaled = mat;
            scaled.scale(mult->factors);
            ops.erase(ops.end() - 1);
            addMatrix(scaled, output);
            return;
        }
    }
    if (isFuzzyScalingOnly(mat)) {
        // pure scaling, this can be simplified
        addMultiplier(QVector3D(mat(0, 0), mat(1, 1), mat(2, 2)));
        return;
    }
    ops.push_back(ColorOp{
        .input = currentOutputRange(),
        .operation = ColorMatrix(mat),
        .output = output,
    });
}

static const QMatrix4x4 s_toICtCp = QMatrix4x4(
    2048.0 / 4096.0,   2048.0 / 4096.0,   0.0,             0.0,
    6610.0 / 4096.0,  -13613.0 / 4096.0,  7003.0 / 4096.0, 0.0,
    17933.0 / 4096.0, -17390.0 / 4096.0, -543.0 / 4096.0,  0.0,
    0.0,               0.0,               0.0,             1.0).transposed();
static const QMatrix4x4 s_fromICtCp = s_toICtCp.inverted();

void ColorPipeline::addTonemapper(const Colorimetry &containerColorimetry, double referenceLuminance, double maxInputLuminance, double maxOutputLuminance, double maxAddedHeadroom)
{
    // convert from rgb to ICtCp
    addMatrix(containerColorimetry.toLMS(), currentOutputRange());
    const TransferFunction PQ(TransferFunction::PerceptualQuantizer);
    addInverseTransferFunction(PQ);
    addMatrix(s_toICtCp, currentOutputRange());
    // apply the tone mapping to the intensity component
    ops.push_back(ColorOp{
        .input = currentOutputRange(),
        .operation = ColorTonemapper(referenceLuminance, maxInputLuminance, maxOutputLuminance, maxAddedHeadroom),
        .output = ValueRange{
            .min = PQ.nitsToEncoded(currentOutputRange().min),
            .max = PQ.nitsToEncoded(maxOutputLuminance),
        },
    });
    // convert back to rgb
    addMatrix(s_fromICtCp, currentOutputRange());
    addTransferFunction(PQ);
    addMatrix(containerColorimetry.fromLMS(), currentOutputRange());
}

bool ColorPipeline::isIdentity() const
{
    return ops.empty();
}

void ColorPipeline::add(const ColorOp &op)
{
    if (const auto mat = std::get_if<ColorMatrix>(&op.operation)) {
        addMatrix(mat->mat, op.output);
    } else if (const auto mult = std::get_if<ColorMultiplier>(&op.operation)) {
        addMultiplier(mult->factors);
    } else if (const auto tf = std::get_if<ColorTransferFunction>(&op.operation)) {
        addTransferFunction(tf->tf);
    } else if (const auto tf = std::get_if<InverseColorTransferFunction>(&op.operation)) {
        addInverseTransferFunction(tf->tf);
    } else {
        ops.push_back(op);
    }
}

ColorPipeline ColorPipeline::merged(const ColorPipeline &onTop) const
{
    ColorPipeline ret{inputRange};
    ret.ops = ops;
    for (const auto &op : onTop.ops) {
        ret.add(op);
    }
    return ret;
}

QVector3D ColorPipeline::evaluate(const QVector3D &input) const
{
    QVector3D ret = input;
    for (const auto &op : ops) {
        ret = op.apply(ret);
    }
    return ret;
}

QVector3D ColorOp::apply(const QVector3D input) const
{
    if (const auto mat = std::get_if<ColorMatrix>(&operation)) {
        return mat->mat * input;
    } else if (const auto mult = std::get_if<ColorMultiplier>(&operation)) {
        return mult->factors * input;
    } else if (const auto tf = std::get_if<ColorTransferFunction>(&operation)) {
        return tf->tf.encodedToNits(input);
    } else if (const auto tf = std::get_if<InverseColorTransferFunction>(&operation)) {
        return tf->tf.nitsToEncoded(input);
    } else if (const auto tonemap = std::get_if<ColorTonemapper>(&operation)) {
        return QVector3D(tonemap->map(input.x()), input.y(), input.z());
    } else {
        Q_UNREACHABLE();
    }
}

ColorTransferFunction::ColorTransferFunction(TransferFunction tf)
    : tf(tf)
{
}

InverseColorTransferFunction::InverseColorTransferFunction(TransferFunction tf)
    : tf(tf)
{
}

ColorMatrix::ColorMatrix(const QMatrix4x4 &mat)
    : mat(mat)
{
}

ColorMultiplier::ColorMultiplier(const QVector3D &factors)
    : factors(factors)
{
}

ColorMultiplier::ColorMultiplier(double factor)
    : factors(factor, factor, factor)
{
}

ColorTonemapper::ColorTonemapper(double referenceLuminance, double maxInputLuminance, double maxOutputLuminance, double maxAddedHeadroom)
    : m_inputReferenceLuminance(referenceLuminance)
    , m_maxInputLuminance(maxInputLuminance)
    , m_maxOutputLuminance(maxOutputLuminance)
{
    m_inputRange = maxInputLuminance / referenceLuminance;
    const double outputRange = maxOutputLuminance / referenceLuminance;
    // = how much dynamic range this algorithm adds, by reducing the reference luminance
    m_addedRange = std::clamp(m_inputRange / outputRange, 1.0, maxAddedHeadroom);
    m_outputReferenceLuminance = referenceLuminance / m_addedRange;
}

double ColorTonemapper::map(double pqEncodedLuminance) const
{
    const double luminance = TransferFunction(TransferFunction::PerceptualQuantizer).encodedToNits(pqEncodedLuminance);
    // keep things linear up to the reference luminance
    const double low = std::min(luminance / m_addedRange, m_outputReferenceLuminance);
    // and apply a nonlinear curve above, to reduce the luminance without completely removing differences
    const double relativeHighlight = std::clamp((luminance / m_inputReferenceLuminance - 1.0) / (m_inputRange - 1.0), 0.0, 1.0);
    const double high = std::log(relativeHighlight * (std::numbers::e - 1) + 1) * (m_maxOutputLuminance - m_outputReferenceLuminance);
    return TransferFunction(TransferFunction::PerceptualQuantizer).nitsToEncoded(low + high);
}
}

QDebug operator<<(QDebug debug, const KWin::ColorPipeline &pipeline)
{
    debug << "ColorPipeline(";
    for (const auto &op : pipeline.ops) {
        if (auto tf = std::get_if<KWin::ColorTransferFunction>(&op.operation)) {
            debug << tf->tf;
        } else if (auto tf = std::get_if<KWin::InverseColorTransferFunction>(&op.operation)) {
            debug << "inverse" << tf->tf;
        } else if (auto mat = std::get_if<KWin::ColorMatrix>(&op.operation)) {
            debug << mat->mat;
        } else if (auto mult = std::get_if<KWin::ColorMultiplier>(&op.operation)) {
            debug << mult->factors;
        } else if (auto tonemap = std::get_if<KWin::ColorTonemapper>(&op.operation)) {
            debug << "tonemapper(" << tonemap->m_inputReferenceLuminance << tonemap->m_maxInputLuminance << tonemap->m_maxOutputLuminance << ")";
        }
    }
    debug << ")";
    return debug;
}

QDebug operator<<(QDebug debug, const KWin::ValueRange &range)
{
    debug << "[" << range.min << "," << range.max << "]";
    return debug;
}
