/********************************************************************
 KWin - the KDE window manager
 This file is part of the KDE project.

Copyright (C) 2006 Lubos Lunak <l.lunak@kde.org>

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

#include "toplevel.h"

#ifdef KWIN_BUILD_ACTIVITIES
#include "activities.h"
#endif
#include "atoms.h"
#include "client.h"
#include "client_machine.h"
#include "effects.h"
#include "screens.h"
#include "shadow.h"
#include "xcbutils.h"

#include <QDebug>

namespace KWin
{

Toplevel::Toplevel()
    : m_visual(XCB_NONE)
    , info(NULL)
    , ready_for_painting(true)
    , m_isDamaged(false)
    , m_client()
    , damage_handle(None)
    , is_shape(false)
    , effect_window(NULL)
    , m_clientMachine(new ClientMachine(this))
    , wmClientLeaderWin(0)
    , unredirect(false)
    , unredirectSuspend(false)
    , m_damageReplyPending(false)
    , m_screen(0)
    , m_skipCloseAnimation(false)
{
    connect(this, SIGNAL(damaged(KWin::Toplevel*,QRect)), SIGNAL(needsRepaint()));
    connect(screens(), SIGNAL(changed()), SLOT(checkScreen()));
    connect(screens(), SIGNAL(countChanged(int,int)), SLOT(checkScreen()));
    setupCheckScreenConnection();
}

Toplevel::~Toplevel()
{
    assert(damage_handle == None);
    delete info;
}

QDebug& operator<<(QDebug& stream, const Toplevel* cl)
{
    if (cl == NULL)
        return stream << "\'NULL\'";
    cl->debug(stream);
    return stream;
}

QDebug& operator<<(QDebug& stream, const ToplevelList& list)
{
    stream << "LIST:(";
    bool first = true;
    for (ToplevelList::ConstIterator it = list.begin();
            it != list.end();
            ++it) {
        if (!first)
            stream << ":";
        first = false;
        stream << *it;
    }
    stream << ")";
    return stream;
}

QRect Toplevel::decorationRect() const
{
    return rect();
}

void Toplevel::detectShape(Window id)
{
    const bool wasShape = is_shape;
    is_shape = Xcb::Extensions::self()->hasShape(id);
    if (wasShape != is_shape) {
        emit shapedChanged();
    }
}

// used only by Deleted::copy()
void Toplevel::copyToDeleted(Toplevel* c)
{
    geom = c->geom;
    m_visual = c->m_visual;
    bit_depth = c->bit_depth;
    info = c->info;
    m_client.reset(c->m_client, false);
    ready_for_painting = c->ready_for_painting;
    damage_handle = None;
    damage_region = c->damage_region;
    repaints_region = c->repaints_region;
    is_shape = c->is_shape;
    effect_window = c->effect_window;
    if (effect_window != NULL)
        effect_window->setWindow(this);
    resource_name = c->resourceName();
    resource_class = c->resourceClass();
    m_clientMachine = c->m_clientMachine;
    m_clientMachine->setParent(this);
    wmClientLeaderWin = c->wmClientLeader();
    opaque_region = c->opaqueRegion();
    m_screen = c->m_screen;
    m_skipCloseAnimation = c->m_skipCloseAnimation;
}

// before being deleted, remove references to everything that's now
// owner by Deleted
void Toplevel::disownDataPassedToDeleted()
{
    info = NULL;
}

QRect Toplevel::visibleRect() const
{
    QRect r = decorationRect();
    if (hasShadow() && !shadow()->shadowRegion().isEmpty()) {
        r |= shadow()->shadowRegion().boundingRect();
    }
    return r.translated(geometry().topLeft());
}

void Toplevel::getWmClientLeader()
{
    Xcb::Property prop(false, window(), atoms->wm_client_leader, XCB_ATOM_WINDOW, 0, 10000);
    wmClientLeaderWin = prop.value<xcb_window_t>(window());
}

/*!
  Returns sessionId for this client,
  taken either from its window or from the leader window.
 */
QByteArray Toplevel::sessionId() const
{
    QByteArray result = Xcb::StringProperty(window(), atoms->sm_client_id);
    if (result.isEmpty() && wmClientLeaderWin && wmClientLeaderWin != window())
        result = Xcb::StringProperty(wmClientLeaderWin, atoms->sm_client_id);
    return result;
}

/*!
  Returns command property for this client,
  taken either from its window or from the leader window.
 */
QByteArray Toplevel::wmCommand()
{
    QByteArray result = Xcb::StringProperty(window(), XCB_ATOM_WM_COMMAND);
    if (result.isEmpty() && wmClientLeaderWin && wmClientLeaderWin != window())
        result = Xcb::StringProperty(wmClientLeaderWin, XCB_ATOM_WM_COMMAND);
    result.replace(0, ' ');
    return result;
}

void Toplevel::getWmClientMachine()
{
    m_clientMachine->resolve(window(), wmClientLeader());
}

/*!
  Returns client machine for this client,
  taken either from its window or from the leader window.
*/
QByteArray Toplevel::wmClientMachine(bool use_localhost) const
{
    if (!m_clientMachine) {
        // this should never happen
        return QByteArray();
    }
    if (use_localhost && m_clientMachine->isLocal()) {
        // special name for the local machine (localhost)
        return ClientMachine::localhost();
    }
    return m_clientMachine->hostName();
}

/*!
  Returns client leader window for this client.
  Returns the client window itself if no leader window is defined.
*/
Window Toplevel::wmClientLeader() const
{
    if (wmClientLeaderWin)
        return wmClientLeaderWin;
    return window();
}

void Toplevel::getResourceClass()
{
    resource_name  = QByteArray(info->windowClassName()).toLower();
    resource_class = QByteArray(info->windowClassClass()).toLower();
    emit windowClassChanged();
}

double Toplevel::opacity() const
{
    if (info->opacity() == 0xffffffff)
        return 1.0;
    return info->opacity() * 1.0 / 0xffffffff;
}

void Toplevel::setOpacity(double new_opacity)
{
    double old_opacity = opacity();
    new_opacity = qBound(0.0, new_opacity, 1.0);
    if (old_opacity == new_opacity)
        return;
    info->setOpacity(static_cast< unsigned long >(new_opacity * 0xffffffff));
    if (compositing()) {
        addRepaintFull();
        emit opacityChanged(this, old_opacity);
    }
}

void Toplevel::setReadyForPainting()
{
    if (!ready_for_painting) {
        ready_for_painting = true;
        if (compositing()) {
            addRepaintFull();
            emit windowShown(this);
            if (Client *cl = dynamic_cast<Client*>(this)) {
                if (cl->tabGroup() && cl->tabGroup()->current() == cl)
                    cl->tabGroup()->setCurrent(cl, true);
            }
        }
    }
}

void Toplevel::deleteEffectWindow()
{
    delete effect_window;
    effect_window = NULL;
}

void Toplevel::checkScreen()
{
    if (screens()->count() == 1) {
        if (m_screen != 0) {
            m_screen = 0;
            emit screenChanged();
        }
        return;
    }
    const int s = screens()->number(geometry().center());
    if (s != m_screen) {
        m_screen = s;
        emit screenChanged();
    }
}

void Toplevel::setupCheckScreenConnection()
{
    connect(this, SIGNAL(geometryShapeChanged(KWin::Toplevel*,QRect)), SLOT(checkScreen()));
    connect(this, SIGNAL(geometryChanged()), SLOT(checkScreen()));
    checkScreen();
}

void Toplevel::removeCheckScreenConnection()
{
    disconnect(this, SIGNAL(geometryShapeChanged(KWin::Toplevel*,QRect)), this, SLOT(checkScreen()));
    disconnect(this, SIGNAL(geometryChanged()), this, SLOT(checkScreen()));
}

int Toplevel::screen() const
{
    return m_screen;
}

bool Toplevel::isOnScreen(int screen) const
{
    return screens()->geometry(screen).intersects(geometry());
}

bool Toplevel::isOnActiveScreen() const
{
    return isOnScreen(screens()->current());
}

void Toplevel::getShadow()
{
    QRect dirtyRect;  // old & new shadow region
    const QRect oldVisibleRect = visibleRect();
    if (hasShadow()) {
        dirtyRect = shadow()->shadowRegion().boundingRect();
        effectWindow()->sceneWindow()->shadow()->updateShadow();
    } else {
        Shadow::createShadow(this);
    }
    if (hasShadow())
        dirtyRect |= shadow()->shadowRegion().boundingRect();
    if (oldVisibleRect != visibleRect())
        emit paddingChanged(this, oldVisibleRect);
    if (dirtyRect.isValid()) {
        dirtyRect.translate(pos());
        addLayerRepaint(dirtyRect);
    }
}

bool Toplevel::hasShadow() const
{
    if (effectWindow() && effectWindow()->sceneWindow()) {
        return effectWindow()->sceneWindow()->shadow() != NULL;
    }
    return false;
}

Shadow *Toplevel::shadow()
{
    if (effectWindow() && effectWindow()->sceneWindow()) {
        return effectWindow()->sceneWindow()->shadow();
    } else {
        return NULL;
    }
}

const Shadow *Toplevel::shadow() const
{
    if (effectWindow() && effectWindow()->sceneWindow()) {
        return effectWindow()->sceneWindow()->shadow();
    } else {
        return NULL;
    }
}

void Toplevel::getWmOpaqueRegion()
{
    const int length=32768;
    unsigned long bytes_after_return=0;
    QRegion new_opaque_region;
    do {
        unsigned long* data;
        Atom type;
        int rformat;
        unsigned long nitems;
        if (XGetWindowProperty(display(), m_client,
                               atoms->net_wm_opaque_region, 0, length, false, XCB_ATOM_CARDINAL,
                               &type, &rformat, &nitems, &bytes_after_return,
                               reinterpret_cast< unsigned char** >(&data)) == Success) {
            if (type != XCB_ATOM_CARDINAL || rformat != 32 || nitems%4) {
                // it can happen, that the window does not provide this property
                XFree(data);
                break;
            }

            for (unsigned int i = 0; i < nitems;) {
                const int x = data[i++];
                const int y = data[i++];
                const int w = data[i++];
                const int h = data[i++];

                new_opaque_region += QRect(x,y,w,h);
            }
            XFree(data);
        } else {
            qWarning() << "XGetWindowProperty failed";
            break;
        }
    } while (bytes_after_return > 0);

    opaque_region = new_opaque_region;
}

bool Toplevel::isClient() const
{
    return false;
}

bool Toplevel::isDeleted() const
{
    return false;
}

bool Toplevel::isOnCurrentActivity() const
{
#ifdef KWIN_BUILD_ACTIVITIES
    return isOnActivity(Activities::self()->current());
#else
    return true;
#endif
}

void Toplevel::elevate(bool elevate)
{
    if (!effectWindow()) {
        return;
    }
    effectWindow()->elevate(elevate);
    addWorkspaceRepaint(visibleRect());
}

pid_t Toplevel::pid() const
{
    return info->pid();
}

xcb_window_t Toplevel::frameId() const
{
    return m_client;
}

void Toplevel::getSkipCloseAnimation()
{
    Xcb::Property property(false, window(), atoms->kde_skip_close_animation, XCB_ATOM_CARDINAL, 0, 1);
    setSkipCloseAnimation(property.toBool());
}

bool Toplevel::skipsCloseAnimation() const
{
    return m_skipCloseAnimation;
}

void Toplevel::setSkipCloseAnimation(bool set)
{
    if (set == m_skipCloseAnimation) {
        return;
    }
    m_skipCloseAnimation = set;
    emit skipCloseAnimationChanged();
}

void Toplevel::sendPointerEnterEvent(const QPointF &globalPos)
{
    Q_UNUSED(globalPos)
}

void Toplevel::sendPointerLeaveEvent(const QPointF &globalPos)
{
    Q_UNUSED(globalPos)
}

void Toplevel::sendPointerMoveEvent(const QPointF &globalPos)
{
    Q_UNUSED(globalPos)
}

void Toplevel::sendPointerButtonEvent(uint32_t button, InputRedirection::PointerButtonState state)
{
    Q_UNUSED(button)
    Q_UNUSED(state)
}

void Toplevel::sendPointerAxisEvent(InputRedirection::PointerAxis axis, qreal delta)
{
    Q_UNUSED(axis)
    Q_UNUSED(delta)
}

void Toplevel::sendKeybordKeyEvent(uint32_t key, InputRedirection::KeyboardKeyState state)
{
    Q_UNUSED(key)
    Q_UNUSED(state)
}

} // namespace

#include "toplevel.moc"
