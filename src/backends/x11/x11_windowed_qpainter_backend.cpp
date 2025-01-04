/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "x11_windowed_qpainter_backend.h"
#include "core/graphicsbufferview.h"
#include "core/shmgraphicsbufferallocator.h"
#include "platformsupport/scenes/qpainter/qpainterswapchain.h"
#include "x11_windowed_backend.h"
#include "x11_windowed_output.h"

#include <cerrno>
#include <cmath>
#include <drm_fourcc.h>
#include <string.h>
#include <xcb/present.h>

namespace KWin
{

X11WindowedQPainterPrimaryLayer::X11WindowedQPainterPrimaryLayer(X11WindowedOutput *output, X11WindowedQPainterBackend *backend)
    : OutputLayer(output)
    , m_output(output)
    , m_backend(backend)
{
}

X11WindowedQPainterPrimaryLayer::~X11WindowedQPainterPrimaryLayer()
{
}

std::optional<OutputLayerBeginFrameInfo> X11WindowedQPainterPrimaryLayer::doBeginFrame()
{
    const QSize bufferSize = m_output->modeSize();
    if (!m_swapchain || m_swapchain->size() != bufferSize) {
        m_swapchain = std::make_unique<QPainterSwapchain>(m_backend->graphicsBufferAllocator(), bufferSize, m_output->backend()->driFormatForDepth(m_output->depth()));
    }

    m_current = m_swapchain->acquire();
    if (!m_current) {
        return std::nullopt;
    }

    QRegion repaint = m_output->exposedArea() + m_output->rect();
    m_output->clearExposedArea();

    m_renderTime = std::make_unique<CpuRenderTimeQuery>();
    return OutputLayerBeginFrameInfo{
        .renderTarget = RenderTarget(m_current->view()->image()),
        .repaint = repaint,
    };
}

bool X11WindowedQPainterPrimaryLayer::doEndFrame(const QRegion &renderedRegion, const QRegion &damagedRegion, OutputFrame *frame)
{
    m_renderTime->end();
    frame->addRenderTimeQuery(std::move(m_renderTime));
    return true;
}

void X11WindowedQPainterPrimaryLayer::present()
{
    xcb_pixmap_t pixmap = m_output->importBuffer(m_current->buffer());
    Q_ASSERT(pixmap != XCB_PIXMAP_NONE);

    xcb_xfixes_region_t valid = 0;
    xcb_xfixes_region_t update = 0;
    uint32_t serial = 0;
    uint32_t options = 0;
    uint64_t targetMsc = 0;

    xcb_present_pixmap(m_output->backend()->connection(),
                       m_output->window(),
                       pixmap,
                       serial,
                       valid,
                       update,
                       0,
                       0,
                       XCB_NONE,
                       XCB_NONE,
                       XCB_NONE,
                       options,
                       targetMsc,
                       0,
                       0,
                       0,
                       nullptr);
}

DrmDevice *X11WindowedQPainterPrimaryLayer::scanoutDevice() const
{
    return m_backend->drmDevice();
}

QHash<uint32_t, QList<uint64_t>> X11WindowedQPainterPrimaryLayer::supportedDrmFormats() const
{
    return m_backend->supportedFormats();
}

X11WindowedQPainterCursorLayer::X11WindowedQPainterCursorLayer(X11WindowedOutput *output)
    : OutputLayer(output)
    , m_output(output)
{
}

std::optional<OutputLayerBeginFrameInfo> X11WindowedQPainterCursorLayer::doBeginFrame()
{
    const auto tmp = targetRect().size().expandedTo(QSize(64, 64));
    const QSize bufferSize(std::ceil(tmp.width()), std::ceil(tmp.height()));
    if (m_buffer.size() != bufferSize) {
        m_buffer = QImage(bufferSize, QImage::Format_ARGB32_Premultiplied);
    }

    m_renderTime = std::make_unique<CpuRenderTimeQuery>();
    return OutputLayerBeginFrameInfo{
        .renderTarget = RenderTarget(&m_buffer),
        .repaint = infiniteRegion(),
    };
}

bool X11WindowedQPainterCursorLayer::doEndFrame(const QRegion &renderedRegion, const QRegion &damagedRegion, OutputFrame *frame)
{
    m_renderTime->end();
    if (frame) {
        frame->addRenderTimeQuery(std::move(m_renderTime));
    }
    m_output->cursor()->update(m_buffer, hotspot());
    return true;
}

DrmDevice *X11WindowedQPainterCursorLayer::scanoutDevice() const
{
    return nullptr;
}

QHash<uint32_t, QList<uint64_t>> X11WindowedQPainterCursorLayer::supportedDrmFormats() const
{
    return {{DRM_FORMAT_ARGB8888, {DRM_FORMAT_MOD_LINEAR}}};
}

X11WindowedQPainterBackend::X11WindowedQPainterBackend(X11WindowedBackend *backend)
    : QPainterBackend()
    , m_backend(backend)
    , m_allocator(std::make_unique<ShmGraphicsBufferAllocator>())
{
    const auto outputs = m_backend->outputs();
    for (Output *output : outputs) {
        addOutput(output);
    }

    connect(backend, &X11WindowedBackend::outputAdded, this, &X11WindowedQPainterBackend::addOutput);
    connect(backend, &X11WindowedBackend::outputRemoved, this, &X11WindowedQPainterBackend::removeOutput);
}

X11WindowedQPainterBackend::~X11WindowedQPainterBackend()
{
    m_outputs.clear();
}

void X11WindowedQPainterBackend::addOutput(Output *output)
{
    X11WindowedOutput *x11Output = static_cast<X11WindowedOutput *>(output);
    m_outputs[output] = Layers{
        .primaryLayer = std::make_unique<X11WindowedQPainterPrimaryLayer>(x11Output, this),
        .cursorLayer = std::make_unique<X11WindowedQPainterCursorLayer>(x11Output),
    };
}

void X11WindowedQPainterBackend::removeOutput(Output *output)
{
    m_outputs.erase(output);
}

GraphicsBufferAllocator *X11WindowedQPainterBackend::graphicsBufferAllocator() const
{
    return m_allocator.get();
}

bool X11WindowedQPainterBackend::present(Output *output, const std::shared_ptr<OutputFrame> &frame)
{
    m_outputs[output].primaryLayer->present();
    static_cast<X11WindowedOutput *>(output)->framePending(frame);
    return true;
}

OutputLayer *X11WindowedQPainterBackend::primaryLayer(Output *output)
{
    return m_outputs[output].primaryLayer.get();
}

OutputLayer *X11WindowedQPainterBackend::cursorLayer(Output *output)
{
    return m_outputs[output].cursorLayer.get();
}

}

#include "moc_x11_windowed_qpainter_backend.cpp"
