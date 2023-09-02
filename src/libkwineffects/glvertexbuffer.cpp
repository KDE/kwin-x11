/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2006-2007 Rivo Laks <rivolaks@hot.ee>
    SPDX-FileCopyrightText: 2010, 2011 Martin Gräßlin <mgraesslin@kde.org>
    SPDX-FileCopyrightText: 2023 Xaver Hugl <xaver.hugl@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "glvertexbuffer.h"
#include "glframebuffer.h"
#include "glshader.h"
#include "glshadermanager.h"
#include "kwineffects.h"
#include "kwinglplatform.h"
#include "kwinglutils.h"
#include "logging_p.h"

#include <QVector4D>
#include <bitset>
#include <deque>

namespace KWin
{

// Certain GPUs, especially mobile, require the data copied to the GPU to be aligned to a
// certain amount of bytes. For example, the Mali GPU requires data to be aligned to 8 bytes.
// This function helps ensure that the data is aligned.
template<typename T>
T align(T value, int bytes)
{
    return (value + bytes - 1) & ~T(bytes - 1);
}

class IndexBuffer
{
public:
    IndexBuffer();
    ~IndexBuffer();

    void accommodate(size_t count);
    void bind();

private:
    GLuint m_buffer;
    size_t m_count = 0;
    std::vector<uint16_t> m_data;
};

IndexBuffer::IndexBuffer()
{
    // The maximum number of quads we can render with 16 bit indices is 16,384.
    // But we start with 512 and grow the buffer as needed.
    glGenBuffers(1, &m_buffer);
    accommodate(512);
}

IndexBuffer::~IndexBuffer()
{
    glDeleteBuffers(1, &m_buffer);
}

void IndexBuffer::accommodate(size_t count)
{
    // Check if we need to grow the buffer.
    if (count <= m_count) {
        return;
    }
    Q_ASSERT(m_count * 2 < std::numeric_limits<uint16_t>::max() / 4);
    const size_t oldCount = m_count;
    m_count *= 2;
    m_data.reserve(m_count * 6);
    for (size_t i = oldCount; i < m_count; i++) {
        const uint16_t offset = i * 4;
        m_data[i * 6 + 0] = offset + 1;
        m_data[i * 6 + 1] = offset + 0;
        m_data[i * 6 + 2] = offset + 3;
        m_data[i * 6 + 3] = offset + 3;
        m_data[i * 6 + 4] = offset + 2;
        m_data[i * 6 + 5] = offset + 1;
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_buffer);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_count * sizeof(uint16_t), m_data.data(), GL_STATIC_DRAW);
}

void IndexBuffer::bind()
{
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_buffer);
}

// ------------------------------------------------------------------

struct VertexAttrib
{
    int size;
    GLenum type;
    int offset;
};

// ------------------------------------------------------------------

struct BufferFence
{
    GLsync sync;
    intptr_t nextEnd;

    bool signaled() const
    {
        GLint value;
        glGetSynciv(sync, GL_SYNC_STATUS, 1, nullptr, &value);
        return value == GL_SIGNALED;
    }
};

static void deleteAll(std::deque<BufferFence> &fences)
{
    for (const BufferFence &fence : fences) {
        glDeleteSync(fence.sync);
    }

    fences.clear();
}

// ------------------------------------------------------------------

template<size_t Count>
struct FrameSizesArray
{
public:
    FrameSizesArray()
    {
        m_array.fill(0);
    }

    void push(size_t size)
    {
        m_array[m_index] = size;
        m_index = (m_index + 1) % Count;
    }

    size_t average() const
    {
        size_t sum = 0;
        for (size_t size : m_array) {
            sum += size;
        }
        return sum / Count;
    }

private:
    std::array<size_t, Count> m_array;
    int m_index = 0;
};

//*********************************
// GLVertexBufferPrivate
//*********************************
class GLVertexBufferPrivate
{
public:
    GLVertexBufferPrivate(GLVertexBuffer::UsageHint usageHint)
        : vertexCount(0)
        , persistent(false)
        , useColor(false)
        , color(0, 0, 0, 255)
        , bufferSize(0)
        , bufferEnd(0)
        , mappedSize(0)
        , frameSize(0)
        , nextOffset(0)
        , baseAddress(0)
        , map(nullptr)
    {
        glGenBuffers(1, &buffer);

        switch (usageHint) {
        case GLVertexBuffer::Dynamic:
            usage = GL_DYNAMIC_DRAW;
            break;
        case GLVertexBuffer::Static:
            usage = GL_STATIC_DRAW;
            break;
        default:
            usage = GL_STREAM_DRAW;
            break;
        }
    }

    ~GLVertexBufferPrivate()
    {
        deleteAll(fences);

        if (buffer != 0) {
            glDeleteBuffers(1, &buffer);
            map = nullptr;
        }
    }

    void interleaveArrays(float *array, int dim, const float *vertices, const float *texcoords, int count);
    void bindArrays();
    void unbindArrays();
    void reallocateBuffer(size_t size);
    GLvoid *mapNextFreeRange(size_t size);
    void reallocatePersistentBuffer(size_t size);
    bool awaitFence(intptr_t offset);
    GLvoid *getIdleRange(size_t size);

    GLuint buffer;
    GLenum usage;
    int stride;
    int vertexCount;
    static std::unique_ptr<GLVertexBuffer> streamingBuffer;
    static bool haveBufferStorage;
    static bool haveSyncFences;
    static bool hasMapBufferRange;
    static bool supportsIndexedQuads;
    QByteArray dataStore;
    bool persistent;
    bool useColor;
    QVector4D color;
    size_t bufferSize;
    intptr_t bufferEnd;
    size_t mappedSize;
    size_t frameSize;
    intptr_t nextOffset;
    intptr_t baseAddress;
    uint8_t *map;
    std::deque<BufferFence> fences;
    FrameSizesArray<4> frameSizes;
    VertexAttrib attrib[VertexAttributeCount];
    std::bitset<32> enabledArrays;
    static std::unique_ptr<IndexBuffer> s_indexBuffer;
};

bool GLVertexBufferPrivate::hasMapBufferRange = false;
bool GLVertexBufferPrivate::supportsIndexedQuads = false;
std::unique_ptr<GLVertexBuffer> GLVertexBufferPrivate::streamingBuffer;
bool GLVertexBufferPrivate::haveBufferStorage = false;
bool GLVertexBufferPrivate::haveSyncFences = false;
std::unique_ptr<IndexBuffer> GLVertexBufferPrivate::s_indexBuffer;

void GLVertexBufferPrivate::interleaveArrays(float *dst, int dim,
                                             const float *vertices, const float *texcoords,
                                             int count)
{
    if (!texcoords) {
        memcpy((void *)dst, vertices, dim * sizeof(float) * count);
        return;
    }

    switch (dim) {
    case 2:
        for (int i = 0; i < count; i++) {
            *(dst++) = *(vertices++);
            *(dst++) = *(vertices++);
            *(dst++) = *(texcoords++);
            *(dst++) = *(texcoords++);
        }
        break;

    case 3:
        for (int i = 0; i < count; i++) {
            *(dst++) = *(vertices++);
            *(dst++) = *(vertices++);
            *(dst++) = *(vertices++);
            *(dst++) = *(texcoords++);
            *(dst++) = *(texcoords++);
        }
        break;

    default:
        for (int i = 0; i < count; i++) {
            for (int j = 0; j < dim; j++) {
                *(dst++) = *(vertices++);
            }

            *(dst++) = *(texcoords++);
            *(dst++) = *(texcoords++);
        }
    }
}

void GLVertexBufferPrivate::bindArrays()
{
    if (useColor) {
        GLShader *shader = ShaderManager::instance()->getBoundShader();
        shader->setUniform(GLShader::Color, color);
    }

    glBindBuffer(GL_ARRAY_BUFFER, buffer);

    for (size_t i = 0; i < enabledArrays.size(); i++) {
        if (enabledArrays[i]) {
            glVertexAttribPointer(i, attrib[i].size, attrib[i].type, GL_FALSE, stride,
                                  (const GLvoid *)(baseAddress + attrib[i].offset));
            glEnableVertexAttribArray(i);
        }
    }
}

void GLVertexBufferPrivate::unbindArrays()
{
    for (size_t i = 0; i < enabledArrays.size(); i++) {
        if (enabledArrays[i]) {
            glDisableVertexAttribArray(i);
        }
    }
}

void GLVertexBufferPrivate::reallocatePersistentBuffer(size_t size)
{
    if (buffer != 0) {
        // This also unmaps and unbinds the buffer
        glDeleteBuffers(1, &buffer);
        buffer = 0;

        deleteAll(fences);
    }

    if (buffer == 0) {
        glGenBuffers(1, &buffer);
    }

    // Round the size up to 64 kb
    size_t minSize = std::max<size_t>(frameSizes.average() * 3, 128 * 1024);
    bufferSize = std::max(size, minSize);

    const GLbitfield storage = GL_DYNAMIC_STORAGE_BIT;
    const GLbitfield access = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;

    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferStorage(GL_ARRAY_BUFFER, bufferSize, nullptr, storage | access);

    map = (uint8_t *)glMapBufferRange(GL_ARRAY_BUFFER, 0, bufferSize, access);

    nextOffset = 0;
    bufferEnd = bufferSize;
}

bool GLVertexBufferPrivate::awaitFence(intptr_t end)
{
    // Skip fences until we reach the end offset
    while (!fences.empty() && fences.front().nextEnd < end) {
        glDeleteSync(fences.front().sync);
        fences.pop_front();
    }

    Q_ASSERT(!fences.empty());

    // Wait on the next fence
    const BufferFence &fence = fences.front();

    if (!fence.signaled()) {
        qCDebug(LIBKWINGLUTILS) << "Stalling on VBO fence";
        const GLenum ret = glClientWaitSync(fence.sync, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000);

        if (ret == GL_TIMEOUT_EXPIRED || ret == GL_WAIT_FAILED) {
            qCCritical(LIBKWINGLUTILS) << "Wait failed";
            return false;
        }
    }

    glDeleteSync(fence.sync);

    // Update the end pointer
    bufferEnd = fence.nextEnd;
    fences.pop_front();

    return true;
}

GLvoid *GLVertexBufferPrivate::getIdleRange(size_t size)
{
    if (size > bufferSize) {
        reallocatePersistentBuffer(size * 2);
    }

    // Handle wrap-around
    if ((nextOffset + size > bufferSize)) {
        nextOffset = 0;
        bufferEnd -= bufferSize;

        for (BufferFence &fence : fences) {
            fence.nextEnd -= bufferSize;
        }

        // Emit a fence now
        if (auto sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0)) {
            fences.push_back(BufferFence{
                .sync = sync,
                .nextEnd = intptr_t(bufferSize)});
        }
    }

    if (nextOffset + intptr_t(size) > bufferEnd) {
        if (!awaitFence(nextOffset + size)) {
            return nullptr;
        }
    }

    return map + nextOffset;
}

void GLVertexBufferPrivate::reallocateBuffer(size_t size)
{
    // Round the size up to 4 Kb for streaming/dynamic buffers.
    const size_t minSize = 32768; // Minimum size for streaming buffers
    const size_t alloc = usage != GL_STATIC_DRAW ? std::max(size, minSize) : size;

    glBufferData(GL_ARRAY_BUFFER, alloc, nullptr, usage);

    bufferSize = alloc;
}

GLvoid *GLVertexBufferPrivate::mapNextFreeRange(size_t size)
{
    GLbitfield access = GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_UNSYNCHRONIZED_BIT;

    if ((nextOffset + size) > bufferSize) {
        // Reallocate the data store if it's too small.
        if (size > bufferSize) {
            reallocateBuffer(size);
        } else {
            access |= GL_MAP_INVALIDATE_BUFFER_BIT;
            access ^= GL_MAP_UNSYNCHRONIZED_BIT;
        }

        nextOffset = 0;
    }

    return glMapBufferRange(GL_ARRAY_BUFFER, nextOffset, size, access);
}

const GLVertexAttrib GLVertexBuffer::GLVertex2DLayout[2] = {
    {VA_Position, 2, GL_FLOAT, offsetof(GLVertex2D, position)},
    {VA_TexCoord, 2, GL_FLOAT, offsetof(GLVertex2D, texcoord)},
};

const GLVertexAttrib GLVertexBuffer::GLVertex3DLayout[2] = {
    {VA_Position, 3, GL_FLOAT, offsetof(GLVertex3D, position)},
    {VA_TexCoord, 2, GL_FLOAT, offsetof(GLVertex3D, texcoord)},
};

GLVertexBuffer::GLVertexBuffer(UsageHint hint)
    : d(std::make_unique<GLVertexBufferPrivate>(hint))
{
}

GLVertexBuffer::~GLVertexBuffer() = default;

void GLVertexBuffer::setData(const void *data, size_t size)
{
    GLvoid *ptr = map(size);
    memcpy(ptr, data, size);
    unmap();
}

GLvoid *GLVertexBuffer::map(size_t size)
{
    d->mappedSize = size;
    d->frameSize += size;

    if (d->persistent) {
        return d->getIdleRange(size);
    }

    glBindBuffer(GL_ARRAY_BUFFER, d->buffer);

    bool preferBufferSubData = GLPlatform::instance()->preferBufferSubData();

    if (GLVertexBufferPrivate::hasMapBufferRange && !preferBufferSubData) {
        return (GLvoid *)d->mapNextFreeRange(size);
    }

    // If we can't map the buffer we allocate local memory to hold the
    // buffer data and return a pointer to it.  The data will be submitted
    // to the actual buffer object when the user calls unmap().
    if (size_t(d->dataStore.size()) < size) {
        d->dataStore.resize(size);
    }

    return (GLvoid *)d->dataStore.data();
}

void GLVertexBuffer::unmap()
{
    if (d->persistent) {
        d->baseAddress = d->nextOffset;
        d->nextOffset += align(d->mappedSize, 8);
        d->mappedSize = 0;
        return;
    }

    bool preferBufferSubData = GLPlatform::instance()->preferBufferSubData();

    if (GLVertexBufferPrivate::hasMapBufferRange && !preferBufferSubData) {
        glUnmapBuffer(GL_ARRAY_BUFFER);

        d->baseAddress = d->nextOffset;
        d->nextOffset += align(d->mappedSize, 8);
    } else {
        // Upload the data from local memory to the buffer object
        if (preferBufferSubData) {
            if ((d->nextOffset + d->mappedSize) > d->bufferSize) {
                d->reallocateBuffer(d->mappedSize);
                d->nextOffset = 0;
            }

            glBufferSubData(GL_ARRAY_BUFFER, d->nextOffset, d->mappedSize, d->dataStore.constData());

            d->baseAddress = d->nextOffset;
            d->nextOffset += align(d->mappedSize, 8);
        } else {
            glBufferData(GL_ARRAY_BUFFER, d->mappedSize, d->dataStore.data(), d->usage);
            d->baseAddress = 0;
        }

        // Free the local memory buffer if it's unlikely to be used again
        if (d->usage == GL_STATIC_DRAW) {
            d->dataStore = QByteArray();
        }
    }

    d->mappedSize = 0;
}

void GLVertexBuffer::setVertexCount(int count)
{
    d->vertexCount = count;
}

void GLVertexBuffer::setAttribLayout(const GLVertexAttrib *attribs, int count, int stride)
{
    // Start by disabling all arrays
    d->enabledArrays.reset();

    for (int i = 0; i < count; i++) {
        const int index = attribs[i].index;

        Q_ASSERT(index >= 0 && index < VertexAttributeCount);
        Q_ASSERT(!d->enabledArrays[index]);

        d->attrib[index].size = attribs[i].size;
        d->attrib[index].type = attribs[i].type;
        d->attrib[index].offset = attribs[i].relativeOffset;

        d->enabledArrays[index] = true;
    }

    d->stride = stride;
}

void GLVertexBuffer::render(GLenum primitiveMode)
{
    render(infiniteRegion(), primitiveMode, false);
}

void GLVertexBuffer::render(const QRegion &region, GLenum primitiveMode, bool hardwareClipping)
{
    d->bindArrays();
    draw(region, primitiveMode, 0, d->vertexCount, hardwareClipping);
    d->unbindArrays();
}

void GLVertexBuffer::bindArrays()
{
    d->bindArrays();
}

void GLVertexBuffer::unbindArrays()
{
    d->unbindArrays();
}

void GLVertexBuffer::draw(GLenum primitiveMode, int first, int count)
{
    draw(infiniteRegion(), primitiveMode, first, count, false);
}

void GLVertexBuffer::draw(const QRegion &region, GLenum primitiveMode, int first, int count, bool hardwareClipping)
{
    if (primitiveMode == GL_QUADS) {
        if (!GLVertexBufferPrivate::s_indexBuffer) {
            GLVertexBufferPrivate::s_indexBuffer = std::make_unique<IndexBuffer>();
        }

        GLVertexBufferPrivate::s_indexBuffer->bind();
        GLVertexBufferPrivate::s_indexBuffer->accommodate(count / 4);

        count = count * 6 / 4;

        if (!hardwareClipping) {
            glDrawElementsBaseVertex(GL_TRIANGLES, count, GL_UNSIGNED_SHORT, nullptr, first);
        } else {
            // Clip using scissoring
            const GLFramebuffer *current = GLFramebuffer::currentFramebuffer();
            for (const QRect &r : region) {
                glScissor(r.x(), current->size().height() - (r.y() + r.height()), r.width(), r.height());
                glDrawElementsBaseVertex(GL_TRIANGLES, count, GL_UNSIGNED_SHORT, nullptr, first);
            }
        }
        return;
    }

    if (!hardwareClipping) {
        glDrawArrays(primitiveMode, first, count);
    } else {
        // Clip using scissoring
        const GLFramebuffer *current = GLFramebuffer::currentFramebuffer();
        for (const QRect &r : region) {
            glScissor(r.x(), current->size().height() - (r.y() + r.height()), r.width(), r.height());
            glDrawArrays(primitiveMode, first, count);
        }
    }
}

bool GLVertexBuffer::supportsIndexedQuads()
{
    return GLVertexBufferPrivate::supportsIndexedQuads;
}

bool GLVertexBuffer::isUseColor() const
{
    return d->useColor;
}

void GLVertexBuffer::setUseColor(bool enable)
{
    d->useColor = enable;
}

void GLVertexBuffer::setColor(const QColor &color, bool enable)
{
    d->useColor = enable;
    d->color = QVector4D(color.redF(), color.greenF(), color.blueF(), color.alphaF());
}

void GLVertexBuffer::reset()
{
    d->useColor = false;
    d->color = QVector4D(0, 0, 0, 1);
    d->vertexCount = 0;
}

void GLVertexBuffer::endOfFrame()
{
    if (!d->persistent) {
        return;
    }

    // Emit a fence if we have uploaded data
    if (d->frameSize > 0) {
        d->frameSizes.push(d->frameSize);
        d->frameSize = 0;

        // Force the buffer to be reallocated at the beginning of the next frame
        // if the average frame size is greater than half the size of the buffer
        if (d->frameSizes.average() > d->bufferSize / 2) {
            deleteAll(d->fences);
            glDeleteBuffers(1, &d->buffer);

            d->buffer = 0;
            d->bufferSize = 0;
            d->nextOffset = 0;
            d->map = nullptr;
        } else {
            if (auto sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0)) {
                d->fences.push_back(BufferFence{
                    .sync = sync,
                    .nextEnd = intptr_t(d->nextOffset + d->bufferSize)});
            }
        }
    }
}

void GLVertexBuffer::beginFrame()
{
    if (!d->persistent) {
        return;
    }

    // Remove finished fences from the list and update the bufferEnd offset
    while (d->fences.size() > 1 && d->fences.front().signaled()) {
        const BufferFence &fence = d->fences.front();
        glDeleteSync(fence.sync);

        d->bufferEnd = fence.nextEnd;
        d->fences.pop_front();
    }
}

void GLVertexBuffer::initStatic()
{
    if (GLPlatform::instance()->isGLES()) {
        bool haveBaseVertex = hasGLExtension(QByteArrayLiteral("GL_OES_draw_elements_base_vertex"));
        bool haveCopyBuffer = hasGLVersion(3, 0);
        bool haveMapBufferRange = hasGLExtension(QByteArrayLiteral("GL_EXT_map_buffer_range"));

        GLVertexBufferPrivate::hasMapBufferRange = haveMapBufferRange;
        GLVertexBufferPrivate::supportsIndexedQuads = haveBaseVertex && haveCopyBuffer && haveMapBufferRange;
        GLVertexBufferPrivate::haveBufferStorage = hasGLExtension("GL_EXT_buffer_storage");
        GLVertexBufferPrivate::haveSyncFences = hasGLVersion(3, 0);
    } else {
        bool haveBaseVertex = hasGLVersion(3, 2) || hasGLExtension(QByteArrayLiteral("GL_ARB_draw_elements_base_vertex"));
        bool haveCopyBuffer = hasGLVersion(3, 1) || hasGLExtension(QByteArrayLiteral("GL_ARB_copy_buffer"));
        bool haveMapBufferRange = hasGLVersion(3, 0) || hasGLExtension(QByteArrayLiteral("GL_ARB_map_buffer_range"));

        GLVertexBufferPrivate::hasMapBufferRange = haveMapBufferRange;
        GLVertexBufferPrivate::supportsIndexedQuads = haveBaseVertex && haveCopyBuffer && haveMapBufferRange;
        GLVertexBufferPrivate::haveBufferStorage = hasGLVersion(4, 4) || hasGLExtension("GL_ARB_buffer_storage");
        GLVertexBufferPrivate::haveSyncFences = hasGLVersion(3, 2) || hasGLExtension("GL_ARB_sync");
    }
    GLVertexBufferPrivate::s_indexBuffer.reset();
    GLVertexBufferPrivate::streamingBuffer = std::make_unique<GLVertexBuffer>(GLVertexBuffer::Stream);

    if (GLVertexBufferPrivate::haveBufferStorage && GLVertexBufferPrivate::haveSyncFences) {
        if (qgetenv("KWIN_PERSISTENT_VBO") != QByteArrayLiteral("0")) {
            GLVertexBufferPrivate::streamingBuffer->d->persistent = true;
        }
    }
}

void GLVertexBuffer::cleanup()
{
    GLVertexBufferPrivate::s_indexBuffer.reset();
    GLVertexBufferPrivate::hasMapBufferRange = false;
    GLVertexBufferPrivate::supportsIndexedQuads = false;
    GLVertexBufferPrivate::streamingBuffer.reset();
}

GLVertexBuffer *GLVertexBuffer::streamingBuffer()
{
    return GLVertexBufferPrivate::streamingBuffer.get();
}

}
