/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "kwin_wayland_test.h"

#include "core/output.h"
#include "cursor.h"
#include "decorations/decorationbridge.h"
#include "decorations/settings.h"
#include "pointer_input.h"
#include "scripting/scripting.h"
#include "tiles/tilemanager.h"
#include "utils/common.h"
#include "wayland_server.h"
#include "window.h"
#include "workspace.h"
#include "x11window.h"

#include <KDecoration2/DecoratedClient>
#include <KDecoration2/Decoration>
#include <KDecoration2/DecorationSettings>

#include <KWayland/Client/compositor.h>
#include <KWayland/Client/connection_thread.h>
#include <KWayland/Client/surface.h>

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QTemporaryFile>
#include <QTextStream>

#include <netwm.h>
#include <xcb/xcb_icccm.h>

#include <linux/input.h>

Q_DECLARE_METATYPE(KWin::QuickTileMode)
Q_DECLARE_METATYPE(KWin::MaximizeMode)

namespace KWin
{

static const QString s_socketName = QStringLiteral("wayland_test_kwin_quick_tiling-0");

class QuickTilingTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();
    void testQuickTiling_data();
    void testQuickTiling();
    void testQuickMaximizing_data();
    void testQuickMaximizing();
    void testQuickTilingKeyboardMove_data();
    void testQuickTilingKeyboardMove();
    void testQuickTilingPointerMove_data();
    void testQuickTilingPointerMove();
    void testQuickTilingTouchMove_data();
    void testQuickTilingTouchMove();
    void testX11QuickTiling_data();
    void testX11QuickTiling();
    void testX11QuickTilingAfterVertMaximize_data();
    void testX11QuickTilingAfterVertMaximize();
    void testShortcut_data();
    void testShortcut();
    void testScript_data();
    void testScript();
    void testPerDesktopTiles_data();
    void testPerDesktopTiles();

private:
    KWayland::Client::ConnectionThread *m_connection = nullptr;
    KWayland::Client::Compositor *m_compositor = nullptr;
};

void QuickTilingTest::initTestCase()
{
    qRegisterMetaType<KWin::Window *>();
    qRegisterMetaType<KWin::MaximizeMode>("MaximizeMode");
    QSignalSpy applicationStartedSpy(kwinApp(), &Application::started);
    QVERIFY(waylandServer()->init(s_socketName));
    Test::setOutputConfig({
        QRect(0, 0, 1280, 1024),
        QRect(1280, 0, 1280, 1024),
    });

    // set custom config which disables the Outline
    KSharedConfig::Ptr config = KSharedConfig::openConfig(QString(), KConfig::SimpleConfig);
    KConfigGroup group = config->group(QStringLiteral("Outline"));
    group.writeEntry(QStringLiteral("QmlPath"), QString("/does/not/exist.qml"));
    group.sync();

    kwinApp()->setConfig(config);

    qputenv("XKB_DEFAULT_RULES", "evdev");

    kwinApp()->start();
    QVERIFY(applicationStartedSpy.wait());

    const auto outputs = workspace()->outputs();
    QCOMPARE(outputs.count(), 2);
    QCOMPARE(outputs[0]->geometry(), QRect(0, 0, 1280, 1024));
    QCOMPARE(outputs[1]->geometry(), QRect(1280, 0, 1280, 1024));
}

void QuickTilingTest::init()
{
    QVERIFY(Test::setupWaylandConnection(Test::AdditionalWaylandInterface::XdgDecorationV1));
    m_connection = Test::waylandConnection();
    m_compositor = Test::waylandCompositor();

    workspace()->setActiveOutput(QPoint(640, 512));
    input()->pointer()->warp(QPoint(640, 512));
}

void QuickTilingTest::cleanup()
{
    Test::destroyWaylandConnection();
}

void QuickTilingTest::testQuickTiling_data()
{
    QTest::addColumn<QuickTileMode>("mode");
    QTest::addColumn<QRectF>("expectedGeometry");
    QTest::addColumn<QRectF>("secondScreen");
    QTest::addColumn<QuickTileMode>("expectedModeAfterToggle");

#define FLAG(name) QuickTileMode(QuickTileFlag::name)

    QTest::newRow("left") << FLAG(Left) << QRectF(0, 0, 640, 1024) << QRectF(1280, 0, 640, 1024) << FLAG(Right);
    QTest::newRow("top") << FLAG(Top) << QRectF(0, 0, 1280, 512) << QRectF(1280, 0, 1280, 512) << QuickTileMode();
    QTest::newRow("right") << FLAG(Right) << QRectF(640, 0, 640, 1024) << QRectF(1920, 0, 640, 1024) << QuickTileMode();
    QTest::newRow("bottom") << FLAG(Bottom) << QRectF(0, 512, 1280, 512) << QRectF(1280, 512, 1280, 512) << QuickTileMode();

    QTest::newRow("top left") << (FLAG(Left) | FLAG(Top)) << QRectF(0, 0, 640, 512) << QRectF(1280, 0, 640, 512) << (FLAG(Right) | FLAG(Top));
    QTest::newRow("top right") << (FLAG(Right) | FLAG(Top)) << QRectF(640, 0, 640, 512) << QRectF(1920, 0, 640, 512) << QuickTileMode();
    QTest::newRow("bottom left") << (FLAG(Left) | FLAG(Bottom)) << QRectF(0, 512, 640, 512) << QRectF(1280, 512, 640, 512) << (FLAG(Right) | FLAG(Bottom));
    QTest::newRow("bottom right") << (FLAG(Right) | FLAG(Bottom)) << QRectF(640, 512, 640, 512) << QRectF(1920, 512, 640, 512) << QuickTileMode();

    QTest::newRow("maximize") << FLAG(Maximize) << QRectF(0, 0, 1280, 1024) << QRectF(1280, 0, 1280, 1024) << QuickTileMode();

#undef FLAG
}

void QuickTilingTest::testQuickTiling()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    QVERIFY(surface != nullptr);
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    QVERIFY(shellSurface != nullptr);

    // Map the window.
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(window);
    QCOMPARE(workspace()->activeWindow(), window);
    QCOMPARE(window->frameGeometry(), QRect(0, 0, 100, 50));
    QCOMPARE(window->quickTileMode(), QuickTileMode(QuickTileFlag::None));

    // We have to receive a configure event when the window becomes active.
    QSignalSpy toplevelConfigureRequestedSpy(shellSurface.get(), &Test::XdgToplevel::configureRequested);
    QSignalSpy surfaceConfigureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 1);

    QSignalSpy quickTileChangedSpy(window, &Window::quickTileModeChanged);
    QSignalSpy frameGeometryChangedSpy(window, &Window::frameGeometryChanged);

    QFETCH(QuickTileMode, mode);
    QFETCH(QRectF, expectedGeometry);
    const QuickTileMode oldQuickTileMode = window->quickTileMode();
    window->handleQuickTileShortcut(mode);

    // at this point the geometry did not yet change
    QCOMPARE(window->frameGeometry(), QRect(0, 0, 100, 50));
    // Manually maximized window is always without a tile
    if (mode == QuickTileFlag::Maximize) {
        QCOMPARE(window->requestedQuickTileMode(), QuickTileFlag::None);
    } else {
        QCOMPARE(window->requestedQuickTileMode(), mode);
    }
    // Actual quickTileMOde didn't change yet
    QCOMPARE(window->quickTileMode(), oldQuickTileMode);

    // but we got requested a new geometry
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 2);
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).toSize(), expectedGeometry.size());

    // attach a new image
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), expectedGeometry.size().toSize(), Qt::red);

    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(frameGeometryChangedSpy.count(), 1);
    QCOMPARE(window->frameGeometry(), expectedGeometry);
    if (mode == QuickTileFlag::Maximize) {
        QCOMPARE(quickTileChangedSpy.count(), 0);
        QCOMPARE(window->quickTileMode(), QuickTileFlag::None);
    } else {
        QCOMPARE(quickTileChangedSpy.count(), 1);
        QCOMPARE(window->quickTileMode(), mode);
    }

    // send window to other screen
    QList<Output *> outputs = workspace()->outputs();
    QCOMPARE(window->output(), outputs[0]);
    window->sendToOutput(outputs[1]);
    QCOMPARE(window->output(), outputs[1]);
    if (mode == QuickTileFlag::Maximize) {
        QCOMPARE(window->quickTileMode(), QuickTileFlag::None);
    } else {
        // quick tile should not be changed
        QCOMPARE(window->quickTileMode(), mode);
    }
    QTEST(window->frameGeometry(), "secondScreen");
    Tile *tile = workspace()->tileManager(outputs[1])->quickTile(mode);
    QCOMPARE(window->tile(), mode == QuickTileFlag::Maximize ? nullptr : tile);

    // now try to toggle again
    window->handleQuickTileShortcut(mode);
    QTEST(window->requestedQuickTileMode(), "expectedModeAfterToggle");
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 3);

    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), toplevelConfigureRequestedSpy.last().at(0).toSize(), Qt::red);
    if (mode == QuickTileFlag::Maximize) {
        QVERIFY(!quickTileChangedSpy.wait());
        QCOMPARE(quickTileChangedSpy.count(), 0);
    } else {
        QVERIFY(quickTileChangedSpy.wait());
        QCOMPARE(quickTileChangedSpy.count(), 2);
    }
    QTEST(window->quickTileMode(), "expectedModeAfterToggle");
}

void QuickTilingTest::testQuickMaximizing_data()
{
    QTest::addColumn<QuickTileMode>("mode");

#define FLAG(name) QuickTileMode(QuickTileFlag::name)

    QTest::newRow("maximize") << FLAG(Maximize);
    QTest::newRow("none") << FLAG(None);

#undef FLAG
}

void QuickTilingTest::testQuickMaximizing()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    QVERIFY(surface != nullptr);
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    QVERIFY(shellSurface != nullptr);

    // Map the window.
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(window);
    QCOMPARE(workspace()->activeWindow(), window);
    QCOMPARE(window->frameGeometry(), QRect(0, 0, 100, 50));
    QCOMPARE(window->quickTileMode(), QuickTileMode(QuickTileFlag::None));
    QCOMPARE(window->maximizeMode(), MaximizeRestore);

    // We have to receive a configure event upon becoming active.
    QSignalSpy toplevelConfigureRequestedSpy(shellSurface.get(), &Test::XdgToplevel::configureRequested);
    QSignalSpy surfaceConfigureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 1);

    QSignalSpy quickTileChangedSpy(window, &Window::quickTileModeChanged);
    QSignalSpy frameGeometryChangedSpy(window, &Window::frameGeometryChanged);
    QSignalSpy maximizeChangedSpy(window, &Window::maximizedChanged);

    const QuickTileMode oldQuickTileMode = window->quickTileMode();
    window->setQuickTileModeAtCurrentPosition(QuickTileFlag::Maximize);

    // at this point the geometry did not yet change
    QCOMPARE(window->frameGeometry(), QRect(0, 0, 100, 50));
    // Manually maximized window is always without a tile
    QCOMPARE(window->requestedQuickTileMode(), QuickTileFlag::None);
    QCOMPARE(window->quickTileMode(), oldQuickTileMode);
    QCOMPARE(window->geometryRestore(), QRect(0, 0, 100, 50));

    // but we got requested a new geometry
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 2);
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).toSize(), QSize(1280, 1024));

    // attach a new image
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(1280, 1024), Qt::red);

    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(frameGeometryChangedSpy.count(), 1);
    QCOMPARE(quickTileChangedSpy.count(), 0);
    QCOMPARE(window->quickTileMode(), QuickTileFlag::None);
    QCOMPARE(window->frameGeometry(), QRect(0, 0, 1280, 1024));
    QCOMPARE(window->geometryRestore(), QRect(0, 0, 100, 50));

    // window is now set to maximised
    QCOMPARE(maximizeChangedSpy.count(), 1);
    QCOMPARE(window->maximizeMode(), MaximizeFull);
    QCOMPARE(window->tile(), nullptr);

    // go back to quick tile none
    QFETCH(QuickTileMode, mode);
    window->setQuickTileModeAtCurrentPosition(mode);
    QCOMPARE(window->requestedQuickTileMode(), QuickTileMode(QuickTileFlag::None));
    // geometry not yet changed
    QCOMPARE(window->frameGeometry(), QRect(0, 0, 1280, 1024));
    QCOMPARE(window->geometryRestore(), QRect(0, 0, 100, 50));
    // we got requested a new geometry
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 3);
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).toSize(), QSize(100, 50));

    // render again
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), toplevelConfigureRequestedSpy.last().at(0).toSize(), Qt::yellow);

    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(frameGeometryChangedSpy.count(), 2);
    QCOMPARE(quickTileChangedSpy.count(), 0);
    QCOMPARE(window->quickTileMode(), QuickTileMode(QuickTileFlag::None));
    QCOMPARE(window->frameGeometry(), QRect(0, 0, 100, 50));
    QCOMPARE(window->geometryRestore(), QRect(0, 0, 100, 50));
    QCOMPARE(maximizeChangedSpy.count(), 2);
}

void QuickTilingTest::testQuickTilingKeyboardMove_data()
{
    QTest::addColumn<QPoint>("targetPos");
    QTest::addColumn<QuickTileMode>("expectedMode");

    QTest::newRow("topRight") << QPoint(2559, 24) << QuickTileMode(QuickTileFlag::Top | QuickTileFlag::Right);
    QTest::newRow("right") << QPoint(2559, 512) << QuickTileMode(QuickTileFlag::Right);
    QTest::newRow("bottomRight") << QPoint(2559, 1023) << QuickTileMode(QuickTileFlag::Bottom | QuickTileFlag::Right);
    QTest::newRow("bottomLeft") << QPoint(0, 1023) << QuickTileMode(QuickTileFlag::Bottom | QuickTileFlag::Left);
    QTest::newRow("Left") << QPoint(0, 512) << QuickTileMode(QuickTileFlag::Left);
    QTest::newRow("topLeft") << QPoint(0, 24) << QuickTileMode(QuickTileFlag::Top | QuickTileFlag::Left);
}

void QuickTilingTest::testQuickTilingKeyboardMove()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    QVERIFY(surface != nullptr);

    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    QVERIFY(shellSurface != nullptr);
    // let's render
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);

    QVERIFY(window);
    QCOMPARE(workspace()->activeWindow(), window);
    QCOMPARE(window->frameGeometry(), QRect(0, 0, 100, 50));
    QCOMPARE(window->quickTileMode(), QuickTileMode(QuickTileFlag::None));
    QCOMPARE(window->maximizeMode(), MaximizeRestore);

    // We have to receive a configure event when the window becomes active.
    QSignalSpy toplevelConfigureRequestedSpy(shellSurface.get(), &Test::XdgToplevel::configureRequested);
    QSignalSpy surfaceConfigureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 1);

    QSignalSpy quickTileChangedSpy(window, &Window::quickTileModeChanged);

    workspace()->performWindowOperation(window, Options::UnrestrictedMoveOp);
    QCOMPARE(window, workspace()->moveResizeWindow());
    QCOMPARE(Cursors::self()->mouse()->pos(), QPoint(50, 25));

    QFETCH(QPoint, targetPos);
    quint32 timestamp = 1;
    Test::keyboardKeyPressed(KEY_LEFTCTRL, timestamp++);
    while (Cursors::self()->mouse()->pos().x() > targetPos.x()) {
        Test::keyboardKeyPressed(KEY_LEFT, timestamp++);
        Test::keyboardKeyReleased(KEY_LEFT, timestamp++);
    }
    while (Cursors::self()->mouse()->pos().x() < targetPos.x()) {
        Test::keyboardKeyPressed(KEY_RIGHT, timestamp++);
        Test::keyboardKeyReleased(KEY_RIGHT, timestamp++);
    }
    while (Cursors::self()->mouse()->pos().y() < targetPos.y()) {
        Test::keyboardKeyPressed(KEY_DOWN, timestamp++);
        Test::keyboardKeyReleased(KEY_DOWN, timestamp++);
    }
    while (Cursors::self()->mouse()->pos().y() > targetPos.y()) {
        Test::keyboardKeyPressed(KEY_UP, timestamp++);
        Test::keyboardKeyReleased(KEY_UP, timestamp++);
    }
    Test::keyboardKeyReleased(KEY_LEFTCTRL, timestamp++);
    Test::keyboardKeyPressed(KEY_ENTER, timestamp++);
    Test::keyboardKeyReleased(KEY_ENTER, timestamp++);
    QCOMPARE(Cursors::self()->mouse()->pos(), targetPos);
    QVERIFY(!workspace()->moveResizeWindow());

    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 2);
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), toplevelConfigureRequestedSpy.last().at(0).toSize(), Qt::red);
    QVERIFY(quickTileChangedSpy.wait());
    QCOMPARE(quickTileChangedSpy.count(), 1);
    QTEST(window->quickTileMode(), "expectedMode");
}

void QuickTilingTest::testQuickTilingPointerMove_data()
{
    QTest::addColumn<QPoint>("pointerPos");
    QTest::addColumn<QSize>("tileSize");
    QTest::addColumn<QuickTileMode>("expectedMode");

    QTest::newRow("topRight") << QPoint(2559, 24) << QSize(640, 512) << QuickTileMode(QuickTileFlag::Top | QuickTileFlag::Right);
    QTest::newRow("right") << QPoint(2559, 512) << QSize(640, 1024) << QuickTileMode(QuickTileFlag::Right);
    QTest::newRow("bottomRight") << QPoint(2559, 1023) << QSize(640, 512) << QuickTileMode(QuickTileFlag::Bottom | QuickTileFlag::Right);
    QTest::newRow("bottomLeft") << QPoint(0, 1023) << QSize(640, 512) << QuickTileMode(QuickTileFlag::Bottom | QuickTileFlag::Left);
    QTest::newRow("Left") << QPoint(0, 512) << QSize(640, 1024) << QuickTileMode(QuickTileFlag::Left);
    QTest::newRow("topLeft") << QPoint(0, 24) << QSize(640, 512) << QuickTileMode(QuickTileFlag::Top | QuickTileFlag::Left);
}

void QuickTilingTest::testQuickTilingPointerMove()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));

    // let's render
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(window);
    QCOMPARE(workspace()->activeWindow(), window);
    QCOMPARE(window->frameGeometry(), QRect(0, 0, 100, 50));
    QCOMPARE(window->quickTileMode(), QuickTileMode(QuickTileFlag::None));
    QCOMPARE(window->maximizeMode(), MaximizeRestore);

    // we have to receive a configure event when the window becomes active
    QSignalSpy toplevelConfigureRequestedSpy(shellSurface.get(), &Test::XdgToplevel::configureRequested);
    QSignalSpy surfaceConfigureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);
    QSignalSpy frameGeometryChangedSpy(window, &Window::frameGeometryChanged);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 1);

    // verify that basic quick tile mode works as expected, i.e. the window is going to be
    // tiled if the user drags it to a screen edge or a corner
    QSignalSpy quickTileChangedSpy(window, &Window::quickTileModeChanged);
    workspace()->performWindowOperation(window, Options::UnrestrictedMoveOp);
    QCOMPARE(window, workspace()->moveResizeWindow());
    QCOMPARE(Cursors::self()->mouse()->pos(), QPoint(50, 25));

    QFETCH(QPoint, pointerPos);
    QFETCH(QSize, tileSize);
    quint32 timestamp = 1;
    Test::pointerButtonPressed(BTN_LEFT, timestamp++);
    Test::pointerMotion(pointerPos, timestamp++);
    Test::pointerButtonReleased(BTN_LEFT, timestamp++);
    QTEST(window->requestedQuickTileMode(), "expectedMode");
    QCOMPARE(window->geometryRestore(), QRect(0, 0, 100, 50));
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 2);
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).toSize(), tileSize);

    // attach a new image
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), tileSize, Qt::red);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(window->frameGeometry().size(), tileSize);
    QCOMPARE(quickTileChangedSpy.count(), 1);
    QTEST(window->quickTileMode(), "expectedMode");

    // verify that geometry restore is correct after user untiles the window, but changes
    // their mind and tiles the window again while still holding left button
    workspace()->performWindowOperation(window, Options::UnrestrictedMoveOp);
    QCOMPARE(window, workspace()->moveResizeWindow());

    Test::pointerButtonPressed(BTN_LEFT, timestamp++); // untile the window
    Test::pointerMotion(QPoint(1280, 1024) / 2, timestamp++);
    QCOMPARE(window->requestedQuickTileMode(), QuickTileMode(QuickTileFlag::None));
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 3);
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).toSize(), QSize(100, 50));

    // attach a new image
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(100, 50), Qt::red);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(window->frameGeometry().size(), QSize(100, 50));
    QCOMPARE(quickTileChangedSpy.count(), 2);
    QCOMPARE(window->quickTileMode(), QuickTileMode(QuickTileFlag::None));

    Test::pointerMotion(pointerPos, timestamp++); // tile the window again
    Test::pointerButtonReleased(BTN_LEFT, timestamp++);
    QTEST(window->requestedQuickTileMode(), "expectedMode");
    QCOMPARE(window->geometryRestore(), QRect(0, 0, 100, 50));
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 4);
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).toSize(), tileSize);

    // attach a new image
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(100, 50), Qt::red);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(window->frameGeometry().size(), QSize(100, 50));
    QCOMPARE(quickTileChangedSpy.count(), 3);
    QTEST(window->quickTileMode(), "expectedMode");
}

void QuickTilingTest::testQuickTilingTouchMove_data()
{
    QTest::addColumn<QPoint>("targetPos");
    QTest::addColumn<QuickTileMode>("expectedMode");

    QTest::newRow("topRight") << QPoint(2559, 24) << QuickTileMode(QuickTileFlag::Top | QuickTileFlag::Right);
    QTest::newRow("right") << QPoint(2559, 512) << QuickTileMode(QuickTileFlag::Right);
    QTest::newRow("bottomRight") << QPoint(2559, 1023) << QuickTileMode(QuickTileFlag::Bottom | QuickTileFlag::Right);
    QTest::newRow("bottomLeft") << QPoint(0, 1023) << QuickTileMode(QuickTileFlag::Bottom | QuickTileFlag::Left);
    QTest::newRow("Left") << QPoint(0, 512) << QuickTileMode(QuickTileFlag::Left);
    QTest::newRow("topLeft") << QPoint(0, 24) << QuickTileMode(QuickTileFlag::Top | QuickTileFlag::Left);
}

void QuickTilingTest::testQuickTilingTouchMove()
{
    // test verifies that touch on decoration also allows quick tiling
    // see BUG: 390113
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get(), Test::CreationSetup::CreateOnly));
    std::unique_ptr<Test::XdgToplevelDecorationV1> deco(Test::createXdgToplevelDecorationV1(shellSurface.get()));

    QSignalSpy decorationConfigureRequestedSpy(deco.get(), &Test::XdgToplevelDecorationV1::configureRequested);
    QSignalSpy surfaceConfigureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);
    QSignalSpy toplevelConfigureRequestedSpy(shellSurface.get(), &Test::XdgToplevel::configureRequested);

    // wait for the initial configure event
    surface->commit(KWayland::Client::Surface::CommitFlag::None);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 1);

    // let's render
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(1000, 50), Qt::blue);

    QVERIFY(window);
    QVERIFY(window->isDecorated());
    const auto decoration = window->decoration();
    QCOMPARE(workspace()->activeWindow(), window);
    QCOMPARE(window->frameGeometry(), QRect(-decoration->borderLeft(), 0, 1000 + decoration->borderLeft() + decoration->borderRight(), 50 + decoration->borderTop() + decoration->borderBottom()));
    QCOMPARE(window->quickTileMode(), QuickTileMode(QuickTileFlag::None));
    QCOMPARE(window->maximizeMode(), MaximizeRestore);

    // we have to receive a configure event when the window becomes active
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QTRY_COMPARE(surfaceConfigureRequestedSpy.count(), 2);

    QSignalSpy quickTileChangedSpy(window, &Window::quickTileModeChanged);

    // Note that interactive move will be started with a delay.
    quint32 timestamp = 1;
    QSignalSpy interactiveMoveResizeStartedSpy(window, &Window::interactiveMoveResizeStarted);
    Test::touchDown(0, QPointF(window->frameGeometry().center().x(), window->frameGeometry().y() + decoration->borderTop() / 2), timestamp++);
    QVERIFY(interactiveMoveResizeStartedSpy.wait());
    QCOMPARE(window, workspace()->moveResizeWindow());

    QFETCH(QPoint, targetPos);
    Test::touchMotion(0, targetPos, timestamp++);
    Test::touchUp(0, timestamp++);
    QVERIFY(!workspace()->moveResizeWindow());

    // When there are no borders, there is no change to them when quick-tiling.
    // TODO: we should test both cases with fixed fake decoration for autotests.
    const bool hasBorders = Workspace::self()->decorationBridge()->settings()->borderSize() != KDecoration2::BorderSize::None;

    QTEST(window->requestedQuickTileMode(), "expectedMode");
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QTRY_COMPARE(surfaceConfigureRequestedSpy.count(), hasBorders ? 4 : 3);
    QCOMPARE(false, toplevelConfigureRequestedSpy.last().first().toSize().isEmpty());

    // attach a new image
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), toplevelConfigureRequestedSpy.last().at(0).toSize(), Qt::red);
    QVERIFY(quickTileChangedSpy.wait());
    QCOMPARE(quickTileChangedSpy.count(), 1);
    QTEST(window->quickTileMode(), "expectedMode");
}

void QuickTilingTest::testX11QuickTiling_data()
{
    QTest::addColumn<QuickTileMode>("mode");
    QTest::addColumn<QRectF>("expectedGeometry");
    QTest::addColumn<int>("screenId");
    QTest::addColumn<QuickTileMode>("modeAfterToggle");

#define FLAG(name) QuickTileMode(QuickTileFlag::name)

    QTest::newRow("left") << FLAG(Left) << QRectF(0, 0, 640, 1024) << 0 << QuickTileMode();
    QTest::newRow("top") << FLAG(Top) << QRectF(0, 0, 1280, 512) << 0 << QuickTileMode();
    QTest::newRow("right") << FLAG(Right) << QRectF(640, 0, 640, 1024) << 1 << FLAG(Left);
    QTest::newRow("bottom") << FLAG(Bottom) << QRectF(0, 512, 1280, 512) << 0 << QuickTileMode();

    QTest::newRow("top left") << (FLAG(Left) | FLAG(Top)) << QRectF(0, 0, 640, 512) << 0 << QuickTileMode();
    QTest::newRow("top right") << (FLAG(Right) | FLAG(Top)) << QRectF(640, 0, 640, 512) << 1 << (FLAG(Left) | FLAG(Top));
    QTest::newRow("bottom left") << (FLAG(Left) | FLAG(Bottom)) << QRectF(0, 512, 640, 512) << 0 << QuickTileMode();
    QTest::newRow("bottom right") << (FLAG(Right) | FLAG(Bottom)) << QRectF(640, 512, 640, 512) << 1 << (FLAG(Left) | FLAG(Bottom));

    QTest::newRow("maximize") << FLAG(Maximize) << QRectF(0, 0, 1280, 1024) << 0 << QuickTileMode();

#undef FLAG
}
void QuickTilingTest::testX11QuickTiling()
{
    Test::XcbConnectionPtr c = Test::createX11Connection();
    QVERIFY(!xcb_connection_has_error(c.get()));
    const QRect windowGeometry(0, 0, 100, 200);
    xcb_window_t windowId = xcb_generate_id(c.get());
    xcb_create_window(c.get(), XCB_COPY_FROM_PARENT, windowId, rootWindow(),
                      windowGeometry.x(),
                      windowGeometry.y(),
                      windowGeometry.width(),
                      windowGeometry.height(),
                      0, XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, 0, nullptr);
    xcb_size_hints_t hints;
    memset(&hints, 0, sizeof(hints));
    xcb_icccm_size_hints_set_position(&hints, 1, windowGeometry.x(), windowGeometry.y());
    xcb_icccm_size_hints_set_size(&hints, 1, windowGeometry.width(), windowGeometry.height());
    xcb_icccm_set_wm_normal_hints(c.get(), windowId, &hints);
    xcb_map_window(c.get(), windowId);
    xcb_flush(c.get());

    // we should get a window for it
    QSignalSpy windowCreatedSpy(workspace(), &Workspace::windowAdded);
    QVERIFY(windowCreatedSpy.wait());
    X11Window *window = windowCreatedSpy.first().first().value<X11Window *>();
    QVERIFY(window);
    QCOMPARE(window->window(), windowId);

    // now quick tile
    QSignalSpy quickTileChangedSpy(window, &Window::quickTileModeChanged);
    const QRectF origGeo = window->frameGeometry();
    QFETCH(QuickTileMode, mode);
    window->handleQuickTileShortcut(mode);
    if (mode == QuickTileFlag::Maximize) {
        QCOMPARE(quickTileChangedSpy.count(), 0);
        QCOMPARE(window->quickTileMode(), QuickTileFlag::None);
    } else {
        QCOMPARE(quickTileChangedSpy.count(), 1);
        QCOMPARE(window->quickTileMode(), mode);
    }
    QTEST(window->frameGeometry(), "expectedGeometry");
    QCOMPARE(window->geometryRestore(), origGeo);

    // quick tile to same edge again should also act like send to screen
    // if screen is on the same edge
    const auto outputs = workspace()->outputs();
    QCOMPARE(window->output(), outputs[0]);
    window->handleQuickTileShortcut(mode);
    QFETCH(int, screenId);
    QCOMPARE(window->output(), outputs[screenId]);
    QTEST(window->quickTileMode(), "modeAfterToggle");
    QCOMPARE(window->geometryRestore(), origGeo);

    // and destroy the window again
    xcb_unmap_window(c.get(), windowId);
    xcb_destroy_window(c.get(), windowId);
    xcb_flush(c.get());
    c.reset();

    QSignalSpy windowClosedSpy(window, &X11Window::closed);
    QVERIFY(windowClosedSpy.wait());
}

void QuickTilingTest::testX11QuickTilingAfterVertMaximize_data()
{
    QTest::addColumn<QuickTileMode>("mode");
    QTest::addColumn<QRectF>("expectedGeometry");

#define FLAG(name) QuickTileMode(QuickTileFlag::name)

    QTest::newRow("left") << FLAG(Left) << QRectF(0, 0, 640, 1024);
    QTest::newRow("top") << FLAG(Top) << QRectF(0, 0, 1280, 512);
    QTest::newRow("right") << FLAG(Right) << QRectF(640, 0, 640, 1024);
    QTest::newRow("bottom") << FLAG(Bottom) << QRectF(0, 512, 1280, 512);

    QTest::newRow("top left") << (FLAG(Left) | FLAG(Top)) << QRectF(0, 0, 640, 512);
    QTest::newRow("top right") << (FLAG(Right) | FLAG(Top)) << QRectF(640, 0, 640, 512);
    QTest::newRow("bottom left") << (FLAG(Left) | FLAG(Bottom)) << QRectF(0, 512, 640, 512);
    QTest::newRow("bottom right") << (FLAG(Right) | FLAG(Bottom)) << QRectF(640, 512, 640, 512);

    QTest::newRow("maximize") << FLAG(Maximize) << QRectF(0, 0, 1280, 1024);

#undef FLAG
}

void QuickTilingTest::testX11QuickTilingAfterVertMaximize()
{
    Test::XcbConnectionPtr c = Test::createX11Connection();
    QVERIFY(!xcb_connection_has_error(c.get()));
    const QRect windowGeometry(0, 0, 100, 200);
    xcb_window_t windowId = xcb_generate_id(c.get());
    xcb_create_window(c.get(), XCB_COPY_FROM_PARENT, windowId, rootWindow(),
                      windowGeometry.x(),
                      windowGeometry.y(),
                      windowGeometry.width(),
                      windowGeometry.height(),
                      0, XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, 0, nullptr);
    xcb_size_hints_t hints;
    memset(&hints, 0, sizeof(hints));
    xcb_icccm_size_hints_set_position(&hints, 1, windowGeometry.x(), windowGeometry.y());
    xcb_icccm_size_hints_set_size(&hints, 1, windowGeometry.width(), windowGeometry.height());
    xcb_icccm_set_wm_normal_hints(c.get(), windowId, &hints);
    xcb_map_window(c.get(), windowId);
    xcb_flush(c.get());

    // we should get a window for it
    QSignalSpy windowCreatedSpy(workspace(), &Workspace::windowAdded);
    QVERIFY(windowCreatedSpy.wait());
    X11Window *window = windowCreatedSpy.first().first().value<X11Window *>();
    QVERIFY(window);
    QCOMPARE(window->window(), windowId);

    const QRectF origGeo = window->frameGeometry();
    QCOMPARE(window->maximizeMode(), MaximizeRestore);
    // vertically maximize the window
    window->maximize(window->maximizeMode() ^ MaximizeVertical);
    QCOMPARE(window->frameGeometry().width(), origGeo.width());
    QCOMPARE(window->height(), window->output()->geometry().height());
    QCOMPARE(window->geometryRestore(), origGeo);

    // now quick tile
    QSignalSpy quickTileChangedSpy(window, &Window::quickTileModeChanged);
    QFETCH(QuickTileMode, mode);
    window->setQuickTileModeAtCurrentPosition(mode);
    if (mode == QuickTileFlag::Maximize) {
        QCOMPARE(window->quickTileMode(), QuickTileFlag::None);
        QCOMPARE(quickTileChangedSpy.count(), 0);
    } else {
        QCOMPARE(window->quickTileMode(), mode);
        QCOMPARE(quickTileChangedSpy.count(), 1);
    }
    QTEST(window->frameGeometry(), "expectedGeometry");

    // and destroy the window again
    xcb_unmap_window(c.get(), windowId);
    xcb_destroy_window(c.get(), windowId);
    xcb_flush(c.get());
    c.reset();

    QSignalSpy windowClosedSpy(window, &X11Window::closed);
    QVERIFY(windowClosedSpy.wait());
}

void QuickTilingTest::testShortcut_data()
{
    QTest::addColumn<QStringList>("shortcutList");
    QTest::addColumn<QuickTileMode>("expectedMode");
    QTest::addColumn<QRect>("expectedGeometry");

#define FLAG(name) QuickTileMode(QuickTileFlag::name)
    QTest::newRow("top") << QStringList{QStringLiteral("Window Quick Tile Top")} << FLAG(Top) << QRect(0, 0, 1280, 512);
    QTest::newRow("bottom") << QStringList{QStringLiteral("Window Quick Tile Bottom")} << FLAG(Bottom) << QRect(0, 512, 1280, 512);
    QTest::newRow("top right") << QStringList{QStringLiteral("Window Quick Tile Top Right")} << (FLAG(Top) | FLAG(Right)) << QRect(640, 0, 640, 512);
    QTest::newRow("top left") << QStringList{QStringLiteral("Window Quick Tile Top Left")} << (FLAG(Top) | FLAG(Left)) << QRect(0, 0, 640, 512);
    QTest::newRow("bottom right") << QStringList{QStringLiteral("Window Quick Tile Bottom Right")} << (FLAG(Bottom) | FLAG(Right)) << QRect(640, 512, 640, 512);
    QTest::newRow("bottom left") << QStringList{QStringLiteral("Window Quick Tile Bottom Left")} << (FLAG(Bottom) | FLAG(Left)) << QRect(0, 512, 640, 512);
    QTest::newRow("left") << QStringList{QStringLiteral("Window Quick Tile Left")} << FLAG(Left) << QRect(0, 0, 640, 1024);
    QTest::newRow("right") << QStringList{QStringLiteral("Window Quick Tile Right")} << FLAG(Right) << QRect(640, 0, 640, 1024);

    // Test combined actions for corner tiling
    QTest::newRow("top left combined") << QStringList{QStringLiteral("Window Quick Tile Left"), QStringLiteral("Window Quick Tile Top")} << (FLAG(Top) | FLAG(Left)) << QRect(0, 0, 640, 512);
    QTest::newRow("top right combined") << QStringList{QStringLiteral("Window Quick Tile Right"), QStringLiteral("Window Quick Tile Top")} << (FLAG(Top) | FLAG(Right)) << QRect(640, 0, 640, 512);
    QTest::newRow("bottom left combined") << QStringList{QStringLiteral("Window Quick Tile Left"), QStringLiteral("Window Quick Tile Bottom")} << (FLAG(Bottom) | FLAG(Left)) << QRect(0, 512, 640, 512);
    QTest::newRow("bottom right combined") << QStringList{QStringLiteral("Window Quick Tile Right"), QStringLiteral("Window Quick Tile Bottom")} << (FLAG(Bottom) | FLAG(Right)) << QRect(640, 512, 640, 512);
#undef FLAG
}

void QuickTilingTest::testShortcut()
{
#if !KWIN_BUILD_GLOBALSHORTCUTS
    QSKIP("Can't test shortcuts without shortcuts");
    return;
#endif

    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    QVERIFY(surface != nullptr);
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    QVERIFY(shellSurface != nullptr);

    // Map the window.
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(window);
    QCOMPARE(workspace()->activeWindow(), window);
    QCOMPARE(window->frameGeometry(), QRect(0, 0, 100, 50));
    QCOMPARE(window->quickTileMode(), QuickTileMode(QuickTileFlag::None));

    // We have to receive a configure event when the window becomes active.
    QSignalSpy toplevelConfigureRequestedSpy(shellSurface.get(), &Test::XdgToplevel::configureRequested);
    QSignalSpy surfaceConfigureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 1);

    QFETCH(QStringList, shortcutList);
    QFETCH(QRect, expectedGeometry);

    const int numberOfQuickTileActions = shortcutList.count();

    QSignalSpy quickTileChangedSpy(window, &Window::quickTileModeChanged);
    QSignalSpy frameGeometryChangedSpy(window, &Window::frameGeometryChanged);

    for (QString shortcut : shortcutList) {
        // invoke global shortcut through dbus
        auto msg = QDBusMessage::createMethodCall(
            QStringLiteral("org.kde.kglobalaccel"),
            QStringLiteral("/component/kwin"),
            QStringLiteral("org.kde.kglobalaccel.Component"),
            QStringLiteral("invokeShortcut"));
        msg.setArguments(QList<QVariant>{shortcut});
        QDBusConnection::sessionBus().asyncCall(msg);

        QVERIFY(surfaceConfigureRequestedSpy.wait());
        shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
        Test::render(surface.get(), toplevelConfigureRequestedSpy.last().at(0).toSize(), Qt::red);
        QVERIFY(quickTileChangedSpy.wait());
    }

    QCOMPARE(surfaceConfigureRequestedSpy.count(), numberOfQuickTileActions + 1);
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).toSize(), expectedGeometry.size());
    QCOMPARE(frameGeometryChangedSpy.count(), numberOfQuickTileActions);

    QTRY_COMPARE(quickTileChangedSpy.count(), numberOfQuickTileActions);
    // geometry already changed
    QCOMPARE(window->frameGeometry(), expectedGeometry);
    // quick tile mode already changed
    QTEST(window->quickTileMode(), "expectedMode");

    QEXPECT_FAIL("maximize", "Geometry changed called twice for maximize", Continue);
    QCOMPARE(window->frameGeometry(), expectedGeometry);
}

void QuickTilingTest::testScript_data()
{
    QTest::addColumn<QString>("action");
    QTest::addColumn<QuickTileMode>("expectedMode");
    QTest::addColumn<QRect>("expectedGeometry");

#define FLAG(name) QuickTileMode(QuickTileFlag::name)
    QTest::newRow("top") << QStringLiteral("Top") << FLAG(Top) << QRect(0, 0, 1280, 512);
    QTest::newRow("bottom") << QStringLiteral("Bottom") << FLAG(Bottom) << QRect(0, 512, 1280, 512);
    QTest::newRow("top right") << QStringLiteral("TopRight") << (FLAG(Top) | FLAG(Right)) << QRect(640, 0, 640, 512);
    QTest::newRow("top left") << QStringLiteral("TopLeft") << (FLAG(Top) | FLAG(Left)) << QRect(0, 0, 640, 512);
    QTest::newRow("bottom right") << QStringLiteral("BottomRight") << (FLAG(Bottom) | FLAG(Right)) << QRect(640, 512, 640, 512);
    QTest::newRow("bottom left") << QStringLiteral("BottomLeft") << (FLAG(Bottom) | FLAG(Left)) << QRect(0, 512, 640, 512);
    QTest::newRow("left") << QStringLiteral("Left") << FLAG(Left) << QRect(0, 0, 640, 1024);
    QTest::newRow("right") << QStringLiteral("Right") << FLAG(Right) << QRect(640, 0, 640, 1024);
#undef FLAG
}

void QuickTilingTest::testScript()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    QVERIFY(surface != nullptr);
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    QVERIFY(shellSurface != nullptr);

    // Map the window.
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(window);
    QCOMPARE(workspace()->activeWindow(), window);
    QCOMPARE(window->frameGeometry(), QRect(0, 0, 100, 50));
    QCOMPARE(window->quickTileMode(), QuickTileMode(QuickTileFlag::None));

    // We have to receive a configure event upon the window becoming active.
    QSignalSpy toplevelConfigureRequestedSpy(shellSurface.get(), &Test::XdgToplevel::configureRequested);
    QSignalSpy surfaceConfigureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 1);

    QSignalSpy quickTileChangedSpy(window, &Window::quickTileModeChanged);
    QSignalSpy frameGeometryChangedSpy(window, &Window::frameGeometryChanged);

    QVERIFY(Scripting::self());
    QTemporaryFile tmpFile;
    QVERIFY(tmpFile.open());
    QTextStream out(&tmpFile);

    QFETCH(QString, action);
    out << "workspace.slotWindowQuickTile" << action << "()";
    out.flush();

    QFETCH(QuickTileMode, expectedMode);
    QFETCH(QRect, expectedGeometry);

    const int id = Scripting::self()->loadScript(tmpFile.fileName());
    QVERIFY(id != -1);
    QVERIFY(Scripting::self()->isScriptLoaded(tmpFile.fileName()));
    auto s = Scripting::self()->findScript(tmpFile.fileName());
    QVERIFY(s);
    QSignalSpy runningChangedSpy(s, &AbstractScript::runningChanged);
    s->run();

    QVERIFY(runningChangedSpy.wait());
    QCOMPARE(runningChangedSpy.count(), 1);
    QCOMPARE(runningChangedSpy.first().first().toBool(), true);

    // at this point the geometry did not yet change
    QCOMPARE(window->frameGeometry(), QRect(0, 0, 100, 50));
    // but requested quick tile mode already changed
    QCOMPARE(window->requestedQuickTileMode(), expectedMode);

    // but we got requested a new geometry
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 2);
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).toSize(), expectedGeometry.size());

    // attach a new image
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), expectedGeometry.size(), Qt::red);

    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(quickTileChangedSpy.count(), 1);
    QCOMPARE(window->quickTileMode(), expectedMode);
    QEXPECT_FAIL("maximize", "Geometry changed called twice for maximize", Continue);
    QCOMPARE(frameGeometryChangedSpy.count(), 1);
    QCOMPARE(window->frameGeometry(), expectedGeometry);
}

void QuickTilingTest::testPerDesktopTiles_data()
{
    QTest::addColumn<QuickTileMode>("modeDesk1");
    QTest::addColumn<QRectF>("expectedGeometryDesk1");
    QTest::addColumn<QuickTileMode>("modeDesk2");
    QTest::addColumn<QRectF>("expectedGeometryDesk2");

#define FLAG(name) QuickTileMode(QuickTileFlag::name)
    QTest::newRow("topToTop") << FLAG(Top) << QRectF(0, 0, 1280, 512) << FLAG(Top) << QRectF(0, 0, 1280, 500);
    QTest::newRow("leftToLeft") << FLAG(Left) << QRectF(0, 0, 640, 1024) << FLAG(Left) << QRectF(0, 0, 600, 1024);
    QTest::newRow("leftToTop") << FLAG(Left) << QRectF(0, 0, 640, 1024) << FLAG(Top) << QRectF(0, 0, 1280, 500);
    QTest::newRow("leftToRight") << FLAG(Left) << QRectF(0, 0, 640, 1024) << FLAG(Right) << QRectF(640, 0, 640, 1024);
    QTest::newRow("rightToBottom") << FLAG(Right) << QRectF(640, 0, 640, 1024) << FLAG(Bottom) << QRectF(0, 512, 1280, 600);
#undef FLAG
}

void QuickTilingTest::testPerDesktopTiles()
{
    // Add a desktop
    VirtualDesktopManager::self()->setCount(2);
    VirtualDesktopManager::self()->setCurrent(1);

    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    QVERIFY(surface != nullptr);
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    QVERIFY(shellSurface != nullptr);

    // Map the window.
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(window);
    // Window is on all desktops
    window->setDesktops({});
    QCOMPARE(workspace()->activeWindow(), window);
    QCOMPARE(window->frameGeometry(), QRect(0, 0, 100, 50));
    QCOMPARE(window->quickTileMode(), QuickTileMode(QuickTileFlag::None));

    // We have to receive a configure event when the window becomes active.
    QSignalSpy toplevelConfigureRequestedSpy(shellSurface.get(), &Test::XdgToplevel::configureRequested);
    QSignalSpy surfaceConfigureRequestedSpy(shellSurface->xdgSurface(), &Test::XdgSurface::configureRequested);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 1);

    QSignalSpy quickTileChangedSpy(window, &Window::quickTileModeChanged);
    QSignalSpy frameGeometryChangedSpy(window, &Window::frameGeometryChanged);

    QFETCH(QuickTileMode, modeDesk1);
    QFETCH(QRectF, expectedGeometryDesk1);
    QFETCH(QuickTileMode, modeDesk2);
    QFETCH(QRectF, expectedGeometryDesk2);
    window->setQuickTileMode(modeDesk1, true);

    // new geometry
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 2);
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).toSize(), expectedGeometryDesk1.size());

    // render
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), expectedGeometryDesk1.size().toSize(), Qt::red);

    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(frameGeometryChangedSpy.count(), 1);
    QCOMPARE(window->frameGeometry(), expectedGeometryDesk1);
    QCOMPARE(quickTileChangedSpy.count(), 1);
    QCOMPARE(window->quickTileMode(), modeDesk1);

    // Switch current virtual desktop
    VirtualDesktopManager::self()->setCurrent(2);

    // Resize the quick tile to the new expected geometry
    TileManager *manager = workspace()->tileManager(window->output());
    manager->quickTile(modeDesk2)->setGeometryFromWindow(expectedGeometryDesk2);

    // new geometry
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 3);
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).toSize(), QSize(100, 50));
    // render
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), QSize(100, 50), Qt::red);

    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(frameGeometryChangedSpy.count(), 2);
    QCOMPARE(window->frameGeometry(), QRect(0, 0, 100, 50));
    QCOMPARE(quickTileChangedSpy.count(), 2);
    QCOMPARE(window->quickTileMode(), QuickTileMode(QuickTileFlag::None));
    // Assign a new tile for desktop 2
    window->setQuickTileMode(modeDesk2, true);
    // new geometry
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 4);

    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).toSize(), expectedGeometryDesk2.size());
    shellSurface->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(surface.get(), expectedGeometryDesk2.size().toSize(), Qt::red);

    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(frameGeometryChangedSpy.count(), 3);
    QCOMPARE(window->frameGeometry(), expectedGeometryDesk2);
    QCOMPARE(quickTileChangedSpy.count(), 3);
    QCOMPARE(window->quickTileMode(), modeDesk2);
}
}

WAYLANDTEST_MAIN(KWin::QuickTilingTest)
#include "quick_tiling_test.moc"
