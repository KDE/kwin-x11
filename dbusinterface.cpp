/********************************************************************
 KWin - the KDE window manager
 This file is part of the KDE project.

Copyright (C) 2012 Martin Gräßlin <mgraesslin@kde.org>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*********************************************************************/

// own
#include "dbusinterface.h"
#include "compositingadaptor.h"

// kwin
#include "composite.h"
#include "compositingprefs.h"
#include "main.h"
#include "placement.h"
#include "kwinadaptor.h"
#include "scene.h"
#include "workspace.h"
#include "virtualdesktops.h"
#ifdef KWIN_BUILD_ACTIVITIES
#include "activities.h"
#endif

// Qt
#include <QDBusServiceWatcher>

namespace KWin
{

DBusInterface::DBusInterface(QObject *parent)
    : QObject(parent)
{
    (void) new KWinAdaptor(this);

    QDBusConnection dbus = QDBusConnection::sessionBus();
    dbus.registerObject(QStringLiteral("/KWin"), this);
    if (!dbus.registerService(QStringLiteral("org.kde.KWin"))) {
        QDBusServiceWatcher *dog = new QDBusServiceWatcher(QStringLiteral("org.kde.KWin"), dbus, QDBusServiceWatcher::WatchForUnregistration, this);
        connect (dog, SIGNAL(serviceUnregistered(QString)), SLOT(becomeKWinService(QString)));
    }
    dbus.connect(QString(), QStringLiteral("/KWin"), QStringLiteral("org.kde.KWin"), QStringLiteral("reloadConfig"),
                 Workspace::self(), SLOT(slotReloadConfig()));
}

void DBusInterface::becomeKWinService(const QString &service)
{
    // TODO: this watchdog exists to make really safe that we at some point get the service
    // but it's probably no longer needed since we explicitly unregister the service with the deconstructor
    if (service == QStringLiteral("org.kde.KWin") && QDBusConnection::sessionBus().registerService(QStringLiteral("org.kde.KWin")) && sender()) {
        sender()->deleteLater(); // bye doggy :'(
    }
}

DBusInterface::~DBusInterface()
{
    QDBusConnection::sessionBus().unregisterService(QStringLiteral("org.kde.KWin")); // this is the long standing legal service
    // KApplication automatically also grabs org.kde.kwin, so it's often been used externally - ensure to free it as well
    QDBusConnection::sessionBus().unregisterService(QStringLiteral("org.kde.kwin"));
}

// wrap void methods with no arguments to Workspace
#define WRAP(name) \
void DBusInterface::name() \
{\
    Workspace::self()->name();\
}

WRAP(reconfigure)

#undef WRAP

void DBusInterface::killWindow()
{
    Workspace::self()->slotKillWindow();
}

#define WRAP(name) \
void DBusInterface::name() \
{\
    Placement::self()->name();\
}

WRAP(cascadeDesktop)
WRAP(unclutterDesktop)

#undef WRAP

// wrap returning methods with no arguments to Workspace
#define WRAP( rettype, name ) \
rettype DBusInterface::name( ) \
{\
    return Workspace::self()->name(); \
}

WRAP(QString, supportInformation)

#undef WRAP

bool DBusInterface::startActivity(const QString &in0)
{
#ifdef KWIN_BUILD_ACTIVITIES
    return Activities::self()->start(in0);
#else
    return false;
#endif
}

bool DBusInterface::stopActivity(const QString &in0)
{
#ifdef KWIN_BUILD_ACTIVITIES
    return Activities::self()->stop(in0);
#else
    return false;
#endif
}

int DBusInterface::currentDesktop()
{
    return VirtualDesktopManager::self()->current();
}

bool DBusInterface::setCurrentDesktop(int desktop)
{
    return VirtualDesktopManager::self()->setCurrent(desktop);
}

void DBusInterface::nextDesktop()
{
    VirtualDesktopManager::self()->moveTo<DesktopNext>();
}

void DBusInterface::previousDesktop()
{
    VirtualDesktopManager::self()->moveTo<DesktopPrevious>();
}


CompositorDBusInterface::CompositorDBusInterface(Compositor *parent)
    : QObject(parent)
    , m_compositor(parent)
{
    connect(m_compositor, &Compositor::compositingToggled, this, &CompositorDBusInterface::compositingToggled);
    new CompositingAdaptor(this);
    QDBusConnection dbus = QDBusConnection::sessionBus();
    dbus.registerObject(QStringLiteral("/Compositor"), this);
    dbus.connect(QString(), QStringLiteral("/Compositor"), QStringLiteral("org.kde.kwin.Compositing"),
                 QStringLiteral("reinit"), m_compositor, SLOT(slotReinitialize()));
}

QString CompositorDBusInterface::compositingNotPossibleReason() const
{
    return CompositingPrefs::compositingNotPossibleReason();
}

QString CompositorDBusInterface::compositingType() const
{
    if (!m_compositor->hasScene()) {
        return QStringLiteral("none");
    }
    switch (m_compositor->scene()->compositingType()) {
    case XRenderCompositing:
        return QStringLiteral("xrender");
    case OpenGL2Compositing:
#ifdef KWIN_HAVE_OPENGLES
        return QStringLiteral("gles");
#else
        return QStringLiteral("gl2");
#endif
    case QPainterCompositing:
        return "qpainter";
    case NoCompositing:
    default:
        return QStringLiteral("none");
    }
}

bool CompositorDBusInterface::isActive() const
{
    return m_compositor->isActive();
}

bool CompositorDBusInterface::isCompositingPossible() const
{
    return CompositingPrefs::compositingPossible();
}

bool CompositorDBusInterface::isOpenGLBroken() const
{
    return CompositingPrefs::openGlIsBroken();
}

void CompositorDBusInterface::resume()
{
    m_compositor->resume(Compositor::ScriptSuspend);
}

void CompositorDBusInterface::suspend()
{
    m_compositor->suspend(Compositor::ScriptSuspend);
}

QStringList CompositorDBusInterface::supportedOpenGLPlatformInterfaces() const
{
    QStringList interfaces;
    bool supportsGlx = (kwinApp()->operationMode() == Application::OperationModeX11);
#ifdef KWIN_HAVE_OPENGLES
    supportsGlx = false;
#endif
    if (supportsGlx) {
        interfaces << QStringLiteral("glx");
    }
#ifdef KWIN_HAVE_EGL
    interfaces << QStringLiteral("egl");
#endif
    return interfaces;
}

} // namespace
