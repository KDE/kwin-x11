/*
    SPDX-FileCopyrightText: 2016 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/
// Qt
#include <QtTest>

// server
#include "wayland/compositor_interface.h"
#include "wayland/display.h"
#include "wayland/plasmashell_interface.h"

// client
#include "KWayland/Client/compositor.h"
#include "KWayland/Client/connection_thread.h"
#include "KWayland/Client/event_queue.h"
#include "KWayland/Client/plasmashell.h"
#include "KWayland/Client/registry.h"
#include "KWayland/Client/surface.h"

#include <wayland-client-protocol.h>

#include <cerrno> // For EPROTO

using namespace KWayland::Client;
using namespace KWaylandServer;

class ErrorTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void init();
    void cleanup();

    void testMultiplePlasmaShellSurfacesForSurface();

private:
    std::unique_ptr<KWaylandServer::Display> m_display;
    std::unique_ptr<CompositorInterface> m_ci;
    std::unique_ptr<PlasmaShellInterface> m_psi;
    std::unique_ptr<ConnectionThread> m_connection;
    std::unique_ptr<QThread> m_thread;
    std::unique_ptr<EventQueue> m_queue;
    std::unique_ptr<Compositor> m_compositor;
    std::unique_ptr<PlasmaShell> m_plasmaShell;
};

static const QString s_socketName = QStringLiteral("kwayland-test-error-0");

void ErrorTest::init()
{
    m_display = std::make_unique<KWaylandServer::Display>();
    m_display->addSocketName(s_socketName);
    m_display->start();
    QVERIFY(m_display->isRunning());
    m_display->createShm();
    m_ci = std::make_unique<CompositorInterface>(m_display.get());
    m_psi = std::make_unique<PlasmaShellInterface>(m_display.get());

    // setup connection
    m_connection = std::make_unique<KWayland::Client::ConnectionThread>();
    QSignalSpy connectedSpy(m_connection.get(), &ConnectionThread::connected);
    m_connection->setSocketName(s_socketName);

    m_thread = std::make_unique<QThread>();
    m_connection->moveToThread(m_thread.get());
    m_thread->start();

    m_connection->initConnection();
    QVERIFY(connectedSpy.wait());

    m_queue = std::make_unique<EventQueue>();
    m_queue->setup(m_connection.get());

    Registry registry;
    QSignalSpy interfacesAnnouncedSpy(&registry, &Registry::interfacesAnnounced);
    registry.setEventQueue(m_queue.get());
    registry.create(m_connection.get());
    QVERIFY(registry.isValid());
    registry.setup();
    QVERIFY(interfacesAnnouncedSpy.wait());

    m_compositor.reset(registry.createCompositor(registry.interface(Registry::Interface::Compositor).name, registry.interface(Registry::Interface::Compositor).version));
    QVERIFY(m_compositor);
    m_plasmaShell.reset(registry.createPlasmaShell(registry.interface(Registry::Interface::PlasmaShell).name,
                                                   registry.interface(Registry::Interface::PlasmaShell).version));
    QVERIFY(m_plasmaShell);
}

void ErrorTest::cleanup()
{
    m_plasmaShell.reset();
    m_compositor.reset();
    m_queue.reset();
    if (m_thread) {
        m_thread->quit();
        m_thread->wait();
        m_thread.reset();
    }
    m_connection.reset();
    m_display.reset();
}

void ErrorTest::testMultiplePlasmaShellSurfacesForSurface()
{
    // this test verifies that creating two ShellSurfaces for the same Surface triggers a protocol error
    QSignalSpy errorSpy(m_connection.get(), &ConnectionThread::errorOccurred);
    // PlasmaShell is too smart and doesn't allow us to create a second PlasmaShellSurface
    // thus we need to cheat by creating a surface manually
    auto surface = wl_compositor_create_surface(*m_compositor);
    std::unique_ptr<PlasmaShellSurface> shellSurface1(m_plasmaShell->createSurface(surface));
    std::unique_ptr<PlasmaShellSurface> shellSurface2(m_plasmaShell->createSurface(surface));
    QVERIFY(!m_connection->hasError());
    QVERIFY(errorSpy.wait());
    QVERIFY(m_connection->hasError());
    QCOMPARE(m_connection->errorCode(), EPROTO);
    wl_surface_destroy(surface);
}

QTEST_GUILESS_MAIN(ErrorTest)
#include "test_error.moc"
