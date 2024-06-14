/*
    SPDX-FileCopyrightText: 2022 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "outputlayer.h"
#include "scene/surfaceitem.h"
#include "scene/surfaceitem_wayland.h"
#include "wayland/surface.h"

namespace KWin
{

OutputLayer::OutputLayer(Output *output)
    : m_output(output)
{
}

qreal OutputLayer::scale() const
{
    return m_scale;
}

void OutputLayer::setScale(qreal scale)
{
    m_scale = scale;
}

QPointF OutputLayer::hotspot() const
{
    return m_hotspot;
}

void OutputLayer::setHotspot(const QPointF &hotspot)
{
    m_hotspot = hotspot;
}

std::optional<QSize> OutputLayer::fixedSize() const
{
    return std::nullopt;
}

QRegion OutputLayer::repaints() const
{
    return m_repaints;
}

void OutputLayer::addRepaint(const QRegion &region)
{
    m_repaints += region;
}

void OutputLayer::resetRepaints()
{
    m_repaints = QRegion();
}

bool OutputLayer::needsRepaint() const
{
    return !m_repaints.isEmpty();
}

bool OutputLayer::doAttemptScanout(GraphicsBuffer *buffer, const ColorDescription &color, const std::shared_ptr<OutputFrame> &frame)
{
    return false;
}

bool OutputLayer::attemptScanout(SurfaceItem *surfaceItem, const std::shared_ptr<OutputFrame> &frame)
{
    SurfaceItemWayland *wayland = qobject_cast<SurfaceItemWayland *>(surfaceItem);
    if (!wayland || !wayland->surface()) {
        return false;
    }
    const auto buffer = wayland->surface()->buffer();
    const auto attrs = buffer->dmabufAttributes();
    if (!attrs) {
        return false;
    }
    const bool tearing = frame->presentationMode() == PresentationMode::Async || frame->presentationMode() == PresentationMode::AdaptiveAsync;
    const auto formats = tearing ? supportedTearingFormats() : supportedDrmFormats();
    if (!formats.contains(attrs->format) || !formats[attrs->format].contains(attrs->modifier)) {
        if (m_scanoutCandidate && m_scanoutCandidate != surfaceItem) {
            m_scanoutCandidate->setScanoutHint(nullptr, {});
        }
        m_scanoutCandidate = surfaceItem;
        surfaceItem->setScanoutHint(scanoutDevice(), supportedDrmFormats());
        return false;
    }
    m_sourceRect = surfaceItem->bufferSourceBox();
    m_bufferTransform = surfaceItem->bufferTransform();
    const auto desiredTransform = m_output ? m_output->transform() : OutputTransform::Kind::Normal;
    m_offloadTransform = m_bufferTransform.combine(desiredTransform.inverted());
    const bool ret = doAttemptScanout(buffer, surfaceItem->colorDescription(), frame);
    if (ret) {
        surfaceItem->resetDamage();
        // ensure the pixmap is updated when direct scanout ends
        surfaceItem->destroyPixmap();
    }
    return ret;
}

std::optional<OutputLayerBeginFrameInfo> OutputLayer::beginFrame()
{
    m_sourceRect = QRectF(QPointF(0, 0), m_targetRect.size());
    m_bufferTransform = m_output ? m_output->transform() : OutputTransform::Kind::Normal;
    m_offloadTransform = OutputTransform::Kind::Normal;
    return doBeginFrame();
}

bool OutputLayer::endFrame(const QRegion &renderedRegion, const QRegion &damagedRegion, OutputFrame *frame)
{
    return doEndFrame(renderedRegion, damagedRegion, frame);
}

void OutputLayer::notifyNoScanoutCandidate()
{
    if (m_scanoutCandidate) {
        m_scanoutCandidate->setScanoutHint(nullptr, {});
        m_scanoutCandidate = nullptr;
    }
}

void OutputLayer::setEnabled(bool enable)
{
    m_enabled = enable;
}

bool OutputLayer::isEnabled() const
{
    return m_enabled;
}

QRectF OutputLayer::sourceRect() const
{
    return m_sourceRect;
}

void OutputLayer::setSourceRect(const QRectF &rect)
{
    m_sourceRect = rect;
}

OutputTransform OutputLayer::offloadTransform() const
{
    return m_offloadTransform;
}

OutputTransform OutputLayer::bufferTransform() const
{
    return m_bufferTransform;
}

QRect OutputLayer::targetRect() const
{
    return m_targetRect;
}

void OutputLayer::setTargetRect(const QRect &rect)
{
    m_targetRect = rect;
}

QHash<uint32_t, QList<uint64_t>> OutputLayer::supportedTearingFormats() const
{
    return supportedDrmFormats();
}

} // namespace KWin

#include "moc_outputlayer.cpp"
