/********************************************************************
 KWin - the KDE window manager
 This file is part of the KDE project.

Copyright (C) 1999, 2000 Matthias Ettrich <ettrich@kde.org>
Copyright (C) 2003 Lubos Lunak <l.lunak@kde.org>

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
#include "client.h"
// kwin
#ifdef KWIN_BUILD_ACTIVITIES
#include "activities.h"
#endif
#ifdef KWIN_BUILD_KAPPMENU
#include "appmenu.h"
#endif
#include "atoms.h"
#include "bridge.h"
#include "client_machine.h"
#include "composite.h"
#include "cursor.h"
#include "decorations.h"
#include "deleted.h"
#include "focuschain.h"
#include "group.h"
#include "paintredirector.h"
#include "shadow.h"
#ifdef KWIN_BUILD_TABBOX
#include "tabbox.h"
#endif
#include "workspace.h"
#include "screenedge.h"
// KDE
#include <KWindowSystem>
#include <KColorScheme>
// Qt
#include <QApplication>
#include <QDebug>
#include <QProcess>
#include <QStandardPaths>
#include <QScriptEngine>
#include <QScriptProgram>
#include <QWhatsThis>
// XLib
#include <X11/extensions/sync.h>
#include <xcb/xtest.h>
// system
#include <unistd.h>
#include <signal.h>

// Put all externs before the namespace statement to allow the linker
// to resolve them properly

namespace KWin
{

bool Client::s_haveResizeEffect = false;

const long ClientWinMask = XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE |
                           XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
                           XCB_EVENT_MASK_KEYMAP_STATE |
                           XCB_EVENT_MASK_BUTTON_MOTION |
                           XCB_EVENT_MASK_POINTER_MOTION | // need this, too!
                           XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW |
                           XCB_EVENT_MASK_FOCUS_CHANGE |
                           XCB_EVENT_MASK_EXPOSURE |
                           XCB_EVENT_MASK_STRUCTURE_NOTIFY |
                           XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT;

//************************************
// Motif
//************************************
class Motif
{
public:
    // Read a window's current settings from its _MOTIF_WM_HINTS
    // property.  If it explicitly requests that decorations be shown
    // or hidden, 'got_noborder' is set to true and 'noborder' is set
    // appropriately.
    static void readFlags(xcb_window_t w, bool& got_noborder, bool& noborder,
                          bool& resize, bool& move, bool& minimize, bool& maximize,
                          bool& close);
    struct MwmHints {
        ulong flags;
        ulong functions;
        ulong decorations;
        long input_mode;
        ulong status;
    };
    enum {
        MWM_HINTS_FUNCTIONS = (1L << 0),
        MWM_HINTS_DECORATIONS = (1L << 1),

        MWM_FUNC_ALL = (1L << 0),
        MWM_FUNC_RESIZE = (1L << 1),
        MWM_FUNC_MOVE = (1L << 2),
        MWM_FUNC_MINIMIZE = (1L << 3),
        MWM_FUNC_MAXIMIZE = (1L << 4),
        MWM_FUNC_CLOSE = (1L << 5)
    };
};

void Motif::readFlags(xcb_window_t w, bool& got_noborder, bool& noborder,
                      bool& resize, bool& move, bool& minimize, bool& maximize, bool& close)
{
    Atom type;
    int format;
    unsigned long length, after;
    unsigned char* data;
    MwmHints* hints = 0;
    if (XGetWindowProperty(display(), w, atoms->motif_wm_hints, 0, 5,
                          false, atoms->motif_wm_hints, &type, &format,
                          &length, &after, &data) == Success) {
        if (data)
            hints = (MwmHints*) data;
    }
    got_noborder = false;
    noborder = false;
    resize = true;
    move = true;
    minimize = true;
    maximize = true;
    close = true;
    if (hints) {
        // To quote from Metacity 'We support those MWM hints deemed non-stupid'
        if (hints->flags & MWM_HINTS_FUNCTIONS) {
            // if MWM_FUNC_ALL is set, other flags say what to turn _off_
            bool set_value = ((hints->functions & MWM_FUNC_ALL) == 0);
            resize = move = minimize = maximize = close = !set_value;
            if (hints->functions & MWM_FUNC_RESIZE)
                resize = set_value;
            if (hints->functions & MWM_FUNC_MOVE)
                move = set_value;
            if (hints->functions & MWM_FUNC_MINIMIZE)
                minimize = set_value;
            if (hints->functions & MWM_FUNC_MAXIMIZE)
                maximize = set_value;
            if (hints->functions & MWM_FUNC_CLOSE)
                close = set_value;
        }
        if (hints->flags & MWM_HINTS_DECORATIONS) {
            got_noborder = true;
            noborder = !hints->decorations;
        }
        XFree(data);
    }
}

// Creating a client:
//  - only by calling Workspace::createClient()
//      - it creates a new client and calls manage() for it
//
// Destroying a client:
//  - destroyClient() - only when the window itself has been destroyed
//      - releaseWindow() - the window is kept, only the client itself is destroyed

/**
 * \class Client client.h
 * \brief The Client class encapsulates a window decoration frame.
 */

/**
 * This ctor is "dumb" - it only initializes data. All the real initialization
 * is done in manage().
 */
Client::Client()
    : Toplevel()
    , m_client()
    , m_wrapper()
    , m_frame()
    , decoration(NULL)
    , bridge(new Bridge(this))
    , m_activityUpdatesBlocked(false)
    , m_blockedActivityUpdatesRequireTransients(false)
    , m_moveResizeGrabWindow()
    , move_resize_has_keyboard_grab(false)
    , m_managed(false)
    , transient_for (NULL)
    , m_transientForId(XCB_WINDOW_NONE)
    , m_originalTransientForId(XCB_WINDOW_NONE)
    , shade_below(NULL)
    , skip_switcher(false)
    , blocks_compositing(false)
    , m_cursor(Qt::ArrowCursor)
    , autoRaiseTimer(NULL)
    , shadeHoverTimer(NULL)
    , delayedMoveResizeTimer(NULL)
    , m_colormap(XCB_COLORMAP_NONE)
    , in_group(NULL)
    , m_windowGroup(XCB_WINDOW_NONE)
    , tab_group(NULL)
    , in_layer(UnknownLayer)
    , ping_timer(NULL)
    , m_killHelperPID(0)
    , m_pingTimestamp(XCB_TIME_CURRENT_TIME)
    , m_userTime(XCB_TIME_CURRENT_TIME)   // Not known yet
    , allowed_actions(0)
    , block_geometry_updates(0)
    , pending_geometry_update(PendingGeometryNone)
    , shade_geometry_change(false)
    , border_left(0)
    , border_right(0)
    , border_top(0)
    , border_bottom(0)
    , padding_left(0)
    , padding_right(0)
    , padding_top(0)
    , padding_bottom(0)
    , sm_stacking_order(-1)
    , paintRedirector(0)
    , m_firstInTabBox(false)
    , electricMaximizing(false)
    , activitiesDefined(false)
    , needsSessionInteract(false)
    , needsXWindowMove(false)
#ifdef KWIN_BUILD_KAPPMENU
    , m_menuAvailable(false)
#endif
    , m_decoInputExtent()
    , m_focusOutTimer(nullptr)
    , m_palette(QApplication::palette())
    , m_clientSideDecorated(false)
{
    // TODO: Do all as initialization
    syncRequest.counter = syncRequest.alarm = XCB_NONE;
    syncRequest.timeout = syncRequest.failsafeTimeout = NULL;
    syncRequest.lastTimestamp = xTime();
    syncRequest.isPending = false;

    // Set the initial mapping state
    mapping_state = Withdrawn;
    quick_tile_mode = QuickTileNone;
    desk = 0; // No desktop yet

    mode = PositionCenter;
    buttonDown = false;
    moveResizeMode = false;

    info = NULL;

    shade_mode = ShadeNone;
    active = false;
    deleting = false;
    keep_above = false;
    keep_below = false;
    motif_may_move = true;
    motif_may_resize = true;
    motif_may_close = true;
    fullscreen_mode = FullScreenNone;
    skip_taskbar = false;
    original_skip_taskbar = false;
    minimized = false;
    hidden = false;
    modal = false;
    noborder = false;
    app_noborder = false;
    motif_noborder = false;
    urgency = false;
    ignore_focus_stealing = false;
    demands_attention = false;
    check_active_modal = false;

    Pdeletewindow = 0;
    Ptakefocus = 0;
    Pcontexthelp = 0;
    Pping = 0;
    input = false;
    skip_pager = false;

    max_mode = MaximizeRestore;

    //Client to workspace connections require that each
    //client constructed be connected to the workspace wrapper

#ifdef KWIN_BUILD_TABBOX
    // TabBoxClient
    m_tabBoxClient = QSharedPointer<TabBox::TabBoxClientImpl>(new TabBox::TabBoxClientImpl(this));
#endif

    geom = QRect(0, 0, 100, 100);   // So that decorations don't start with size being (0,0)
    client_size = QSize(100, 100);
    ready_for_painting = false; // wait for first damage or sync reply

    connect(this, &Client::geometryShapeChanged, this, &Client::geometryChanged);
    auto signalMaximizeChanged = static_cast<void (Client::*)(KWin::Client*, KDecorationDefines::MaximizeMode)>(&Client::clientMaximizedStateChanged);
    connect(this, signalMaximizeChanged, this, &Client::geometryChanged);
    connect(this, &Client::clientStepUserMovedResized,   this, &Client::geometryChanged);
    connect(this, &Client::clientStartUserMovedResized,  this, &Client::moveResizedChanged);
    connect(this, &Client::clientFinishUserMovedResized, this, &Client::moveResizedChanged);
    connect(this, &Client::clientStartUserMovedResized,  this, &Client::removeCheckScreenConnection);
    connect(this, &Client::clientFinishUserMovedResized, this, &Client::setupCheckScreenConnection);

    connect(clientMachine(), &ClientMachine::localhostChanged, this, &Client::updateCaption);
    connect(options, &Options::condensedTitleChanged, this, &Client::updateCaption);

    m_connections << connect(VirtualDesktopManager::self(), &VirtualDesktopManager::countChanged, [this]() {
        if (decoration) {
            decoration->onAllDesktopsAvailableChanged();
        }
    });

    // SELI TODO: Initialize xsizehints??
}

/**
 * "Dumb" destructor.
 */
Client::~Client()
{
    if (m_killHelperPID && !::kill(m_killHelperPID, 0)) { // means the process is alive
        ::kill(m_killHelperPID, SIGTERM);
        m_killHelperPID = 0;
    }
    //SWrapper::Client::clientRelease(this);
    if (syncRequest.alarm != XCB_NONE)
        xcb_sync_destroy_alarm(connection(), syncRequest.alarm);
    assert(!moveResizeMode);
    assert(m_client == XCB_WINDOW_NONE);
    assert(m_wrapper == XCB_WINDOW_NONE);
    //assert( frameId() == None );
    assert(decoration == NULL);
    assert(block_geometry_updates == 0);
    assert(!check_active_modal);
    delete bridge;
    for (auto it = m_connections.constBegin(); it != m_connections.constEnd(); ++it) {
        disconnect(*it);
    }
}

// Use destroyClient() or releaseWindow(), Client instances cannot be deleted directly
void Client::deleteClient(Client* c)
{
    delete c;
}

/**
 * Releases the window. The client has done its job and the window is still existing.
 */
void Client::releaseWindow(bool on_shutdown)
{
    assert(!deleting);
    deleting = true;
    Deleted* del = NULL;
    if (!on_shutdown) {
        del = Deleted::create(this);
    }
    if (moveResizeMode)
        emit clientFinishUserMovedResized(this);
    emit windowClosed(this, del);
    finishCompositing();
    RuleBook::self()->discardUsed(this, true);   // Remove ForceTemporarily rules
    StackingUpdatesBlocker blocker(workspace());
    if (moveResizeMode)
        leaveMoveResize();
    finishWindowRules();
    ++block_geometry_updates;
    if (isOnCurrentDesktop() && isShown(true))
        addWorkspaceRepaint(visibleRect());
    // Grab X during the release to make removing of properties, setting to withdrawn state
    // and repareting to root an atomic operation (http://lists.kde.org/?l=kde-devel&m=116448102901184&w=2)
    grabXServer();
    exportMappingState(WithdrawnState);
    setModal(false);   // Otherwise its mainwindow wouldn't get focus
    hidden = true; // So that it's not considered visible anymore (can't use hideClient(), it would set flags)
    if (!on_shutdown)
        workspace()->clientHidden(this);
    m_frame.unmap();  // Destroying decoration would cause ugly visual effect
    destroyDecoration();
    cleanGrouping();
    if (!on_shutdown) {
        workspace()->removeClient(this);
        // Only when the window is being unmapped, not when closing down KWin (NETWM sections 5.5,5.7)
        info->setDesktop(0);
        desk = 0;
        info->setState(0, info->state());  // Reset all state flags
    } else
        untab();
    xcb_connection_t *c = connection();
    m_client.deleteProperty(atoms->kde_net_wm_user_creation_time);
    m_client.deleteProperty(atoms->net_frame_extents);
    m_client.deleteProperty(atoms->kde_net_wm_frame_strut);
    m_client.reparent(rootWindow(), x(), y());
    xcb_change_save_set(c, XCB_SET_MODE_DELETE, m_client);
    m_client.selectInput(XCB_EVENT_MASK_NO_EVENT);
    if (on_shutdown)
        // Map the window, so it can be found after another WM is started
        m_client.map();
    // TODO: Preserve minimized, shaded etc. state?
    else // Make sure it's not mapped if the app unmapped it (#65279). The app
        // may do map+unmap before we initially map the window by calling rawShow() from manage().
        m_client.unmap();
    m_client.reset();
    m_wrapper.reset();
    m_frame.reset();
    //frame = None;
    --block_geometry_updates; // Don't use GeometryUpdatesBlocker, it would now set the geometry
    if (!on_shutdown) {
        disownDataPassedToDeleted();
        del->unrefWindow();
    }
    checkNonExistentClients();
    deleteClient(this);
    ungrabXServer();
}

/**
 * Like releaseWindow(), but this one is called when the window has been already destroyed
 * (E.g. The application closed it)
 */
void Client::destroyClient()
{
    assert(!deleting);
    deleting = true;
    Deleted* del = Deleted::create(this);
    if (moveResizeMode)
        emit clientFinishUserMovedResized(this);
    emit windowClosed(this, del);
    finishCompositing(ReleaseReason::Destroyed);
    RuleBook::self()->discardUsed(this, true);   // Remove ForceTemporarily rules
    StackingUpdatesBlocker blocker(workspace());
    if (moveResizeMode)
        leaveMoveResize();
    finishWindowRules();
    ++block_geometry_updates;
    if (isOnCurrentDesktop() && isShown(true))
        addWorkspaceRepaint(visibleRect());
    setModal(false);
    hidden = true; // So that it's not considered visible anymore
    workspace()->clientHidden(this);
    destroyDecoration();
    cleanGrouping();
    workspace()->removeClient(this);
    m_client.reset(); // invalidate
    m_wrapper.reset();
    m_frame.reset();
    //frame = None;
    --block_geometry_updates; // Don't use GeometryUpdatesBlocker, it would now set the geometry
    disownDataPassedToDeleted();
    del->unrefWindow();
    checkNonExistentClients();
    deleteClient(this);
}

void Client::updateInputWindow()
{
    if (!Xcb::Extensions::self()->isShapeInputAvailable())
        return;

    QRegion region;

    if (!noBorder()) {
        // This function is implemented as a slot to avoid breaking binary
        // compatibility
        QMetaObject::invokeMethod(decoration, "region", Qt::DirectConnection,
                Q_RETURN_ARG(QRegion, region),
                Q_ARG(KDecorationDefines::Region, KDecorationDefines::ExtendedBorderRegion));
    }

    if (region.isEmpty()) {
        m_decoInputExtent.reset();
        return;
    }

    QRect bounds = region.boundingRect();
    input_offset = bounds.topLeft();

    // Move the bounding rect to screen coordinates
    bounds.translate(geometry().topLeft());

    // Move the region to input window coordinates
    region.translate(-input_offset);

    if (!m_decoInputExtent.isValid()) {
        const uint32_t mask = XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
        const uint32_t values[] = {true,
            XCB_EVENT_MASK_ENTER_WINDOW   |
            XCB_EVENT_MASK_LEAVE_WINDOW   |
            XCB_EVENT_MASK_BUTTON_PRESS   |
            XCB_EVENT_MASK_BUTTON_RELEASE |
            XCB_EVENT_MASK_POINTER_MOTION
        };
        m_decoInputExtent.create(bounds, XCB_WINDOW_CLASS_INPUT_ONLY, mask, values);
        if (mapping_state == Mapped)
            m_decoInputExtent.map();
    } else {
        m_decoInputExtent.setGeometry(bounds);
    }

    const QVector<xcb_rectangle_t> rects = Xcb::regionToRects(region);
    xcb_shape_rectangles(connection(), XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT, XCB_CLIP_ORDERING_UNSORTED,
                         m_decoInputExtent, 0, 0, rects.count(), rects.constData());
}

void Client::updateDecoration(bool check_workspace_pos, bool force)
{
    if (!force &&
            ((decoration == NULL && noBorder()) || (decoration != NULL && !noBorder())))
        return;
    QRect oldgeom = geometry();
    blockGeometryUpdates(true);
    if (force)
        destroyDecoration();
    if (!noBorder()) {
        createDecoration(oldgeom);
    } else
        destroyDecoration();
    if (check_workspace_pos)
        checkWorkspacePosition(oldgeom);
    updateInputWindow();
    blockGeometryUpdates(false);
    if (!noBorder())
        decoration->show();
    updateFrameExtents();
}

void Client::createDecoration(const QRect& oldgeom)
{
    setMask(QRegion());  // Reset shape mask
    if (decorationPlugin()->isDisabled()) {
        decoration = NULL;
        return;
    } else {
        decoration = decorationPlugin()->createDecoration(bridge);
    }
    connect(this, &Client::iconChanged, decoration, &KDecoration::iconChanged);
    connect(this, &Client::shadeChanged, decoration, &KDecoration::shadeChanged);
    connect(this, &Client::desktopChanged, decoration, &KDecoration::desktopChanged);
    connect(this, &Client::captionChanged, decoration, &KDecoration::captionChanged);
    connect(this, &Client::activeChanged, decoration, &KDecoration::activeChanged);
    auto signalMaximizeChanged = static_cast<void (Client::*)(Client*, KDecorationDefines::MaximizeMode)>(&Client::clientMaximizedStateChanged);
    connect(this, signalMaximizeChanged, decoration, &KDecoration::maximizeChanged);
    auto slotKeepAbove = static_cast<void(KDecoration::*)(bool)>(&KDecoration::keepAboveChanged);
    connect(this, &Client::keepAboveChanged, decoration, slotKeepAbove);
    auto slotKeepBelow = static_cast<void(KDecoration::*)(bool)>(&KDecoration::keepBelowChanged);
    connect(this, &Client::keepBelowChanged, decoration, slotKeepBelow);
#ifdef KWIN_BUILD_KAPPMENU
    connect(this, SIGNAL(showRequest()), decoration, SIGNAL(showRequest()));
    connect(this, SIGNAL(appMenuAvailable()), decoration, SIGNAL(appMenuAvailable()));
    connect(this, SIGNAL(appMenuUnavailable()), decoration, SIGNAL(appMenuUnavailable()));
    connect(this, SIGNAL(menuHidden()), decoration, SIGNAL(menuHidden()));
#endif
    // TODO: Check decoration's minimum size?
    decoration->init();
    if (decoration->widget()) {
        decoration->widget()->installEventFilter(this);
        decoration->window()->setParent(m_frameWrapper.data());
    } else if (decoration->window()) {
        decoration->window()->installEventFilter(this);
        xcb_reparent_window(connection(), decoration->window()->winId(), frameId(), 0, 0);
    }
    decoration->window()->lower();
    decoration->borders(border_left, border_right, border_top, border_bottom);
    padding_left = padding_right = padding_top = padding_bottom = 0;
    decoration->padding(padding_left, padding_right, padding_top, padding_bottom);
    Xcb::moveWindow(decoration->window()->winId(), -padding_left, -padding_top);
    move(calculateGravitation(false));
    plainResize(sizeForClientSize(clientSize()), ForceGeometrySet);
    if (Compositor::compositing()) {
        paintRedirector = PaintRedirector::create(this, decoration);
        discardWindowPixmap();
    }
    emit geometryShapeChanged(this, oldgeom);
}

void Client::destroyDecoration()
{
    QRect oldgeom = geometry();
    if (decoration != NULL) {
        delete decoration;
        decoration = NULL;
        paintRedirector = NULL;
        QPoint grav = calculateGravitation(true);
        border_left = border_right = border_top = border_bottom = 0;
        setMask(QRegion());  // Reset shape mask
        plainResize(sizeForClientSize(clientSize()), ForceGeometrySet);
        move(grav);
        if (compositing())
            discardWindowPixmap();
        if (!deleting) {
            emit geometryShapeChanged(this, oldgeom);
        }
    }
    m_decoInputExtent.reset();
}

bool Client::checkBorderSizes(bool also_resize)
{
    if (decoration == NULL)
        return false;

    int new_left = 0, new_right = 0, new_top = 0, new_bottom = 0;
    decoration->padding(new_left, new_right, new_top, new_bottom);
    if (padding_left != new_left || padding_top != new_top)
        Xcb::moveWindow(decoration->window()->winId(), -new_left, -new_top);
    padding_left = new_left;
    padding_right = new_right;
    padding_top = new_top;
    padding_bottom = new_bottom;
    decoration->borders(new_left, new_right, new_top, new_bottom);
    if (new_left == border_left && new_right == border_right &&
            new_top == border_top && new_bottom == border_bottom)
        return false;
    if (!also_resize) {
        border_left = new_left;
        border_right = new_right;
        border_top = new_top;
        border_bottom = new_bottom;
        return true;
    }
    GeometryUpdatesBlocker blocker(this);
    move(calculateGravitation(true));
    border_left = new_left;
    border_right = new_right;
    border_top = new_top;
    border_bottom = new_bottom;
    move(calculateGravitation(false));
    QRect oldgeom = geometry();
    plainResize(sizeForClientSize(clientSize()), ForceGeometrySet);
    checkWorkspacePosition(oldgeom);
    return true;
}

void Client::triggerDecorationRepaint()
{
    if (decoration && decoration->widget())
        decoration->widget()->update();
}

void Client::layoutDecorationRects(QRect &left, QRect &top, QRect &right, QRect &bottom, Client::CoordinateMode mode) const
{
    QRect r = decoration->rect();
    if (mode == WindowRelative)
        r.translate(-padding_left, -padding_top);

    NETStrut strut = info->frameOverlap();

    // Ignore the overlap strut when compositing is disabled
    if (!compositing() || !decorationPlugin()->supportsFrameOverlap())
        strut.left = strut.top = strut.right = strut.bottom = 0;
    else if (strut.left == -1 && strut.top == -1 && strut.right == -1 && strut.bottom == -1) {
        top = QRect(r.x(), r.y(), r.width(), r.height() / 3);
        left = QRect(r.x(), r.y() + top.height(), width() / 2, r.height() / 3);
        right = QRect(r.x() + left.width(), r.y() + top.height(), r.width() - left.width(), left.height());
        bottom = QRect(r.x(), r.y() + top.height() + left.height(), r.width(), r.height() - left.height() - top.height());
        return;
    }

    top = QRect(r.x(), r.y(), r.width(), padding_top + border_top + strut.top);
    bottom = QRect(r.x(), r.y() + r.height() - padding_bottom - border_bottom - strut.bottom,
                   r.width(), padding_bottom + border_bottom + strut.bottom);
    left = QRect(r.x(), r.y() + top.height(),
                 padding_left + border_left + strut.left, r.height() - top.height() - bottom.height());
    right = QRect(r.x() + r.width() - padding_right - border_right - strut.right, r.y() + top.height(),
                  padding_right + border_right + strut.right, r.height() - top.height() - bottom.height());
}

QRegion Client::decorationPendingRegion() const
{
    if (!paintRedirector)
        return QRegion();
    return paintRedirector->scheduledRepaintRegion().translated(x() - padding_left, y() - padding_top);
}

QRect Client::transparentRect() const
{
    if (isShade())
        return QRect();

    NETStrut strut = info->frameOverlap();
    // Ignore the strut when compositing is disabled or the decoration doesn't support it
    if (!compositing() || !decorationPlugin()->supportsFrameOverlap())
        strut.left = strut.top = strut.right = strut.bottom = 0;
    else if (strut.left == -1 && strut.top == -1 && strut.right == -1 && strut.bottom == -1)
        return QRect();

    const QRect r = QRect(clientPos(), clientSize())
                    .adjusted(strut.left, strut.top, -strut.right, -strut.bottom);
    if (r.isValid())
        return r;

    return QRect();
}

void Client::detectNoBorder()
{
    if (shape()) {
        noborder = true;
        app_noborder = true;
        return;
    }
    switch(windowType()) {
    case NET::Desktop :
    case NET::Dock :
    case NET::TopMenu :
    case NET::Splash :
    case NET::Notification :
        noborder = true;
        app_noborder = true;
        break;
    case NET::Unknown :
    case NET::Normal :
    case NET::Toolbar :
    case NET::Menu :
    case NET::Dialog :
    case NET::Utility :
        noborder = false;
        break;
    default:
        abort();
    }
    // NET::Override is some strange beast without clear definition, usually
    // just meaning "noborder", so let's treat it only as such flag, and ignore it as
    // a window type otherwise (SUPPORTED_WINDOW_TYPES_MASK doesn't include it)
    if (info->windowType(SUPPORTED_MANAGED_WINDOW_TYPES_MASK | NET::OverrideMask) == NET::Override) {
        noborder = true;
        app_noborder = true;
    }
}

void Client::updateFrameExtents()
{
    NETStrut strut;
    strut.left = border_left;
    strut.right = border_right;
    strut.top = border_top;
    strut.bottom = border_bottom;
    info->setFrameExtents(strut);
}

void Client::detectGtkFrameExtents()
{
    Xcb::Property prop(false, m_client, atoms->gtk_frame_extents, XCB_ATOM_CARDINAL, 0, 4);
    m_clientSideDecorated = !prop.isNull() && prop->type != 0;
    emit clientSideDecoratedChanged();
}

/**
 * Resizes the decoration, and makes sure the decoration widget gets resize event
 * even if the size hasn't changed. This is needed to make sure the decoration
 * re-layouts (e.g. when maximization state changes,
 * the decoration may alter some borders, but the actual size
 * of the decoration stays the same).
 */
void Client::resizeDecoration(const QSize& s)
{
    if (decoration == NULL)
        return;
    QSize newSize = s + QSize(padding_left + padding_right, padding_top + padding_bottom);
    QSize oldSize = decoration->window()->size();
    decoration->resize(newSize);
    if (oldSize == newSize) {
        QResizeEvent e(newSize, oldSize);
        QObject *receiver = nullptr;
        if (decoration->widget()) {
            receiver = decoration->widget();
        } else {
            receiver = decoration->window();
        }
        QApplication::sendEvent(receiver, &e);
    } else if (paintRedirector) { // oldSize != newSize
        paintRedirector->resizePixmaps();
    } else {
        triggerDecorationRepaint();
    }
    Xcb::moveWindow(decoration->window()->winId(), -padding_left, -padding_top);
    updateInputWindow();
}

bool Client::noBorder() const
{
    return decorationPlugin()->isDisabled() || noborder || isFullScreen();
}

bool Client::userCanSetNoBorder() const
{
    return !isFullScreen() && !isShade() && !tabGroup();
}

void Client::setNoBorder(bool set)
{
    if (!userCanSetNoBorder())
        return;
    set = rules()->checkNoBorder(set);
    if (noborder == set)
        return;
    noborder = set;
    updateDecoration(true, false);
    updateWindowRules(Rules::NoBorder);
}

void Client::checkNoBorder()
{
    setNoBorder(app_noborder);
}

void Client::updateShape()
{
    if (shape()) {
        // Workaround for #19644 - Shaped windows shouldn't have decoration
        if (!app_noborder) {
            // Only when shape is detected for the first time, still let the user to override
            app_noborder = true;
            noborder = rules()->checkNoBorder(true);
            updateDecoration(true);
        }
        if (noBorder()) {
            xcb_shape_combine(connection(), XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, XCB_SHAPE_SK_BOUNDING,
                              frameId(), clientPos().x(), clientPos().y(), window());
        }
    } else if (app_noborder) {
        xcb_shape_mask(connection(), XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, frameId(), 0, 0, XCB_PIXMAP_NONE);
        detectNoBorder();
        app_noborder = noborder;
        noborder = rules()->checkNoBorder(noborder || motif_noborder);
        updateDecoration(true);
    }

    // Decoration mask (i.e. 'else' here) setting is done in setMask()
    // when the decoration calls it or when the decoration is created/destroyed
    updateInputShape();
    if (compositing()) {
        addRepaintFull();
        addWorkspaceRepaint(visibleRect());   // In case shape change removes part of this window
    }
    emit geometryShapeChanged(this, geometry());
}

static Xcb::Window shape_helper_window(XCB_WINDOW_NONE);

void Client::updateInputShape()
{
    if (hiddenPreview())   // Sets it to none, don't change
        return;
    if (Xcb::Extensions::self()->isShapeInputAvailable()) {
        // There appears to be no way to find out if a window has input
        // shape set or not, so always propagate the input shape
        // (it's the same like the bounding shape by default).
        // Also, build the shape using a helper window, not directly
        // in the frame window, because the sequence set-shape-to-frame,
        // remove-shape-of-client, add-input-shape-of-client has the problem
        // that after the second step there's a hole in the input shape
        // until the real shape of the client is added and that can make
        // the window lose focus (which is a problem with mouse focus policies)
        // TODO: It seems there is, after all - XShapeGetRectangles() - but maybe this is better
        if (!shape_helper_window.isValid())
            shape_helper_window.create(QRect(0, 0, 1, 1));
        shape_helper_window.resize(width(), height());
        xcb_connection_t *c = connection();
        xcb_shape_combine(c, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT, XCB_SHAPE_SK_BOUNDING,
                          shape_helper_window, 0, 0, frameId());
        xcb_shape_combine(c, XCB_SHAPE_SO_SUBTRACT, XCB_SHAPE_SK_INPUT, XCB_SHAPE_SK_BOUNDING,
                          shape_helper_window, clientPos().x(), clientPos().y(), window());
        xcb_shape_combine(c, XCB_SHAPE_SO_UNION, XCB_SHAPE_SK_INPUT, XCB_SHAPE_SK_INPUT,
                          shape_helper_window, clientPos().x(), clientPos().y(), window());
        xcb_shape_combine(c, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT, XCB_SHAPE_SK_INPUT,
                          frameId(), 0, 0, shape_helper_window);
    }
}

void Client::setMask(const QRegion& reg, int mode)
{
    QRegion r = reg.translated(-padding_left, -padding_right) & QRect(0, 0, width(), height());
    if (_mask == r)
        return;
    _mask = r;
    xcb_connection_t *c = connection();
    xcb_window_t shape_window = frameId();
    if (shape()) {
        // The same way of applying a shape without strange intermediate states like above
        if (!shape_helper_window.isValid())
            shape_helper_window.create(QRect(0, 0, 1, 1));
        shape_window = shape_helper_window;
    }
    if (_mask.isEmpty()) {
        xcb_shape_mask(c, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, shape_window, 0, 0, XCB_PIXMAP_NONE);
    } else {
        const QVector< QRect > rects = _mask.rects();
        QVector< xcb_rectangle_t > xrects(rects.count());
        for (int i = 0; i < rects.count(); ++i) {
            const QRect &rect = rects.at(i);
            xcb_rectangle_t xrect;
            xrect.x = rect.x();
            xrect.y = rect.y();
            xrect.width = rect.width();
            xrect.height = rect.height();
            xrects[i] = xrect;
        }
        xcb_shape_rectangles(c, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, mode, shape_window,
                             0, 0, xrects.count(), xrects.constData());
    }
    if (shape()) {
        // The rest of the applying using a temporary window
        xcb_rectangle_t rec = { 0, 0, static_cast<uint16_t>(clientSize().width()),
                           static_cast<uint16_t>(clientSize().height()) };
        xcb_shape_rectangles(c, XCB_SHAPE_SO_SUBTRACT, XCB_SHAPE_SK_BOUNDING, XCB_CLIP_ORDERING_UNSORTED,
                             shape_helper_window, clientPos().x(), clientPos().y(), 1, &rec);
        xcb_shape_combine(c, XCB_SHAPE_SO_UNION, XCB_SHAPE_SK_BOUNDING, XCB_SHAPE_SK_BOUNDING,
                          shape_helper_window, clientPos().x(), clientPos().y(), window());
        xcb_shape_combine(c, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, XCB_SHAPE_SK_BOUNDING,
                          frameId(), 0, 0, shape_helper_window);
    }
    emit geometryShapeChanged(this, geometry());
    updateShape();
}

QRegion Client::mask() const
{
    if (_mask.isEmpty())
        return QRegion(0, 0, width(), height());
    return _mask;
}

void Client::hideClient(bool hide)
{
    if (hidden == hide)
        return;
    hidden = hide;
    updateVisibility();
}

/**
 * Returns whether the window is minimizable or not
 */
bool Client::isMinimizable() const
{
    if (isSpecialWindow() && !isTransient())
        return false;
    if (!rules()->checkMinimize(true))
        return false;

    if (isTransient()) {
        // #66868 - Let other xmms windows be minimized when the mainwindow is minimized
        bool shown_mainwindow = false;
        ClientList mainclients = mainClients();
        for (ClientList::ConstIterator it = mainclients.constBegin();
                it != mainclients.constEnd();
                ++it)
            if ((*it)->isShown(true))
                shown_mainwindow = true;
        if (!shown_mainwindow)
            return true;
    }
#if 0
    // This is here because kicker's taskbar doesn't provide separate entries
    // for windows with an explicitly given parent
    // TODO: perhaps this should be redone
    // Disabled for now, since at least modal dialogs should be minimizable
    // (resulting in the mainwindow being minimized too).
    if (transientFor() != NULL)
        return false;
#endif
    if (!wantsTabFocus())   // SELI, TODO: - NET::Utility? why wantsTabFocus() - skiptaskbar? ?
        return false;
    return true;
}

void Client::setMinimized(bool set)
{
    set ? minimize() : unminimize();
}

/**
 * Minimizes this client plus its transients
 */
void Client::minimize(bool avoid_animation)
{
    if (!isMinimizable() || isMinimized())
        return;

    if (isShade()) // NETWM restriction - KWindowInfo::isMinimized() == Hidden && !Shaded
        info->setState(0, NET::Shaded);

    minimized = true;

    updateVisibility();
    updateAllowedActions();
    workspace()->updateMinimizedOfTransients(this);
    updateWindowRules(Rules::Minimize);
    FocusChain::self()->update(this, FocusChain::MakeFirstMinimized);
    // TODO: merge signal with s_minimized
    emit clientMinimized(this, !avoid_animation);

    // Update states of all other windows in this group
    if (tabGroup())
        tabGroup()->updateStates(this, TabGroup::Minimized);
    emit minimizedChanged();
}

void Client::unminimize(bool avoid_animation)
{
    if (!isMinimized())
        return;

    if (rules()->checkMinimize(false)) {
        return;
    }

    if (isShade()) // NETWM restriction - KWindowInfo::isMinimized() == Hidden && !Shaded
        info->setState(NET::Shaded, NET::Shaded);

    minimized = false;
    updateVisibility();
    updateAllowedActions();
    workspace()->updateMinimizedOfTransients(this);
    updateWindowRules(Rules::Minimize);
    emit clientUnminimized(this, !avoid_animation);

    // Update states of all other windows in this group
    if (tabGroup())
        tabGroup()->updateStates(this, TabGroup::Minimized);
    emit minimizedChanged();
}

QRect Client::iconGeometry() const
{
    NETRect r = info->iconGeometry();
    QRect geom(r.pos.x, r.pos.y, r.size.width, r.size.height);
    if (geom.isValid())
        return geom;
    else {
        // Check all mainwindows of this window (recursively)
        foreach (Client * mainwin, mainClients()) {
            geom = mainwin->iconGeometry();
            if (geom.isValid())
                return geom;
        }
        // No mainwindow (or their parents) with icon geometry was found
        return QRect();
    }
}

bool Client::isShadeable() const
{
    return !isSpecialWindow() && !noBorder() && (rules()->checkShade(ShadeNormal) != rules()->checkShade(ShadeNone));
}

void Client::setShade(bool set) {
    set ? setShade(ShadeNormal) : setShade(ShadeNone);
}

void Client::setShade(ShadeMode mode)
{
    if (mode == ShadeHover && isMove())
        return; // causes geometry breaks and is probably nasty
    if (isSpecialWindow() || noBorder())
        mode = ShadeNone;
    mode = rules()->checkShade(mode);
    if (shade_mode == mode)
        return;
    bool was_shade = isShade();
    ShadeMode was_shade_mode = shade_mode;
    shade_mode = mode;

    // Decorations may turn off some borders when shaded
    // this has to happen _before_ the tab alignment since it will restrict the minimum geometry
    if (decoration)
        decoration->borders(border_left, border_right, border_top, border_bottom);

    // Update states of all other windows in this group
    if (tabGroup())
        tabGroup()->updateStates(this, TabGroup::Shaded);

    if (was_shade == isShade()) {
        // Decoration may want to update after e.g. hover-shade changes
        emit shadeChanged();
        return; // No real change in shaded state
    }

    assert(decoration != NULL);   // noborder windows can't be shaded
    GeometryUpdatesBlocker blocker(this);

    // TODO: All this unmapping, resizing etc. feels too much duplicated from elsewhere
    if (isShade()) {
        // shade_mode == ShadeNormal
        addWorkspaceRepaint(visibleRect());
        // Shade
        shade_geometry_change = true;
        QSize s(sizeForClientSize(QSize(clientSize())));
        s.setHeight(border_top + border_bottom);
        m_wrapper.selectInput(ClientWinMask);   // Avoid getting UnmapNotify
        m_wrapper.unmap();
        m_client.unmap();
        m_wrapper.selectInput(ClientWinMask | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY);
        exportMappingState(IconicState);
        plainResize(s);
        shade_geometry_change = false;
        if (was_shade_mode == ShadeHover) {
            if (shade_below && workspace()->stackingOrder().indexOf(shade_below) > -1)
                    workspace()->restack(this, shade_below);
            if (isActive())
                workspace()->activateNextClient(this);
        } else if (isActive()) {
            workspace()->focusToNull();
        }
    } else {
        shade_geometry_change = true;
        QSize s(sizeForClientSize(clientSize()));
        shade_geometry_change = false;
        plainResize(s);
        if ((shade_mode == ShadeHover || shade_mode == ShadeActivated) && rules()->checkAcceptFocus(input))
            setActive(true);
        if (shade_mode == ShadeHover) {
            ToplevelList order = workspace()->stackingOrder();
            // invalidate, since "this" could be the topmost toplevel and shade_below dangeling
            shade_below = NULL;
            // this is likely related to the index parameter?!
            for (int idx = order.indexOf(this) + 1; idx < order.count(); ++idx) {
                shade_below = qobject_cast<Client*>(order.at(idx));
                if (shade_below) {
                    break;
                }
            }
            if (shade_below && shade_below->isNormalWindow())
                workspace()->raiseClient(this);
            else
                shade_below = NULL;
        }
        m_wrapper.map();
        m_client.map();
        exportMappingState(NormalState);
        if (isActive())
            workspace()->requestFocus(this);
    }
    info->setState(isShade() ? NET::Shaded : NET::States(0), NET::Shaded);
    info->setState(isShown(false) ? NET::States(0) : NET::Hidden, NET::Hidden);
    discardWindowPixmap();
    updateVisibility();
    updateAllowedActions();
    updateWindowRules(Rules::Shade);

    emit shadeChanged();
}

void Client::shadeHover()
{
    setShade(ShadeHover);
    cancelShadeHoverTimer();
}

void Client::shadeUnhover()
{
    if (!tabGroup() || tabGroup()->current() == this ||
        tabGroup()->current()->shadeMode() == ShadeNormal)
        setShade(ShadeNormal);
    cancelShadeHoverTimer();
}

void Client::cancelShadeHoverTimer()
{
    delete shadeHoverTimer;
    shadeHoverTimer = 0;
}

void Client::toggleShade()
{
    // If the mode is ShadeHover or ShadeActive, cancel shade too
    setShade(shade_mode == ShadeNone ? ShadeNormal : ShadeNone);
}

void Client::updateVisibility()
{
    if (deleting)
        return;
    if (hidden && isCurrentTab()) {
        info->setState(NET::Hidden, NET::Hidden);
        setSkipTaskbar(true, false);   // Also hide from taskbar
        if (compositing() && options->hiddenPreviews() == HiddenPreviewsAlways)
            internalKeep();
        else
            internalHide();
        return;
    }
    if (isCurrentTab())
        setSkipTaskbar(original_skip_taskbar, false);   // Reset from 'hidden'
    if (minimized) {
        info->setState(NET::Hidden, NET::Hidden);
        if (compositing() && options->hiddenPreviews() == HiddenPreviewsAlways)
            internalKeep();
        else
            internalHide();
        return;
    }
    info->setState(0, NET::Hidden);
    if (!isOnCurrentDesktop()) {
        if (compositing() && options->hiddenPreviews() != HiddenPreviewsNever)
            internalKeep();
        else
            internalHide();
        return;
    }
    if (!isOnCurrentActivity()) {
        if (compositing() && options->hiddenPreviews() != HiddenPreviewsNever)
            internalKeep();
        else
            internalHide();
        return;
    }
    if (isManaged())
        resetShowingDesktop(true);
    internalShow();
}


void Client::resetShowingDesktop(bool keep_hidden)
{
    if (isDock() || !workspace()->showingDesktop())
        return;
    bool belongs_to_desktop = false;
    for (ClientList::ConstIterator it = group()->members().constBegin(),
                                    end = group()->members().constEnd(); it != end; ++it)
        if ((belongs_to_desktop = (*it)->isDesktop()))
            break;

    if (!belongs_to_desktop)
        workspace()->resetShowingDesktop(keep_hidden);
}

/**
 * Sets the client window's mapping state. Possible values are
 * WithdrawnState, IconicState, NormalState.
 */
void Client::exportMappingState(int s)
{
    assert(m_client != XCB_WINDOW_NONE);
    assert(!deleting || s == WithdrawnState);
    if (s == WithdrawnState) {
        m_client.deleteProperty(atoms->wm_state);
        return;
    }
    assert(s == NormalState || s == IconicState);

    int32_t data[2];
    data[0] = s;
    data[1] = XCB_NONE;
    m_client.changeProperty(atoms->wm_state, atoms->wm_state, 32, 2, data);
}

void Client::internalShow()
{
    if (mapping_state == Mapped)
        return;
    MappingState old = mapping_state;
    mapping_state = Mapped;
    if (old == Unmapped || old == Withdrawn)
        map();
    if (old == Kept) {
        m_decoInputExtent.map();
        updateHiddenPreview();
    }
    if (Compositor::isCreated()) {
        Compositor::self()->checkUnredirect();
    }
}

void Client::internalHide()
{
    if (mapping_state == Unmapped)
        return;
    MappingState old = mapping_state;
    mapping_state = Unmapped;
    if (old == Mapped || old == Kept)
        unmap();
    if (old == Kept)
        updateHiddenPreview();
    addWorkspaceRepaint(visibleRect());
    workspace()->clientHidden(this);
    if (Compositor::isCreated()) {
        Compositor::self()->checkUnredirect();
    }
}

void Client::internalKeep()
{
    assert(compositing());
    if (mapping_state == Kept)
        return;
    MappingState old = mapping_state;
    mapping_state = Kept;
    if (old == Unmapped || old == Withdrawn)
        map();
    m_decoInputExtent.unmap();
    if (isActive())
        workspace()->focusToNull(); // get rid of input focus, bug #317484
    updateHiddenPreview();
    addWorkspaceRepaint(visibleRect());
    workspace()->clientHidden(this);
    if (Compositor::isCreated()) {
        Compositor::self()->checkUnredirect();
    }
}

/**
 * Maps (shows) the client. Note that it is mapping state of the frame,
 * not necessarily the client window itself (i.e. a shaded window is here
 * considered mapped, even though it is in IconicState).
 */
void Client::map()
{
    // XComposite invalidates backing pixmaps on unmap (minimize, different
    // virtual desktop, etc.).  We kept the last known good pixmap around
    // for use in effects, but now we want to have access to the new pixmap
    if (compositing())
        discardWindowPixmap();
    if (decoration != NULL)
        decoration->show(); // Not really necessary, but let it know the state
    m_frame.map();
    if (!isShade()) {
        m_wrapper.map();
        m_client.map();
        m_decoInputExtent.map();
        exportMappingState(NormalState);
    } else
        exportMappingState(IconicState);
}

/**
 * Unmaps the client. Again, this is about the frame.
 */
void Client::unmap()
{
    // Here it may look like a race condition, as some other client might try to unmap
    // the window between these two XSelectInput() calls. However, they're supposed to
    // use XWithdrawWindow(), which also sends a synthetic event to the root window,
    // which won't be missed, so this shouldn't be a problem. The chance the real UnmapNotify
    // will be missed is also very minimal, so I don't think it's needed to grab the server
    // here.
    m_wrapper.selectInput(ClientWinMask);   // Avoid getting UnmapNotify
    m_frame.unmap();
    m_wrapper.unmap();
    m_client.unmap();
    m_decoInputExtent.unmap();
    m_wrapper.selectInput(ClientWinMask | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY);
    if (decoration != NULL)
        decoration->hide(); // Not really necessary, but let it know the state
    exportMappingState(IconicState);
}

/**
 * XComposite doesn't keep window pixmaps of unmapped windows, which means
 * there wouldn't be any previews of windows that are minimized or on another
 * virtual desktop. Therefore rawHide() actually keeps such windows mapped.
 * However special care needs to be taken so that such windows don't interfere.
 * Therefore they're put very low in the stacking order and they have input shape
 * set to none, which hopefully is enough. If there's no input shape available,
 * then it's hoped that there will be some other desktop above it *shrug*.
 * Using normal shape would be better, but that'd affect other things, e.g. painting
 * of the actual preview.
 */
void Client::updateHiddenPreview()
{
    if (hiddenPreview()) {
        workspace()->forceRestacking();
        if (Xcb::Extensions::self()->isShapeInputAvailable()) {
            xcb_shape_rectangles(connection(), XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT,
                                 XCB_CLIP_ORDERING_UNSORTED, frameId(), 0, 0, 0, NULL);
        }
    } else {
        workspace()->forceRestacking();
        updateInputShape();
    }
}

void Client::sendClientMessage(xcb_window_t w, xcb_atom_t a, xcb_atom_t protocol, uint32_t data1, uint32_t data2, uint32_t data3)
{
    xcb_client_message_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.window = w;
    ev.type = a;
    ev.format = 32;
    ev.data.data32[0] = protocol;
    ev.data.data32[1] = xTime();
    ev.data.data32[2] = data1;
    ev.data.data32[3] = data2;
    ev.data.data32[4] = data3;
    uint32_t eventMask = 0;
    if (w == rootWindow()) {
        eventMask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT; // Magic!
    }
    xcb_send_event(connection(), false, w, eventMask, reinterpret_cast<const char*>(&ev));
    xcb_flush(connection());
}

/**
 * Returns whether the window may be closed (have a close button)
 */
bool Client::isCloseable() const
{
    return rules()->checkCloseable(motif_may_close && !isSpecialWindow());
}

/**
 * Closes the window by either sending a delete_window message or using XKill.
 */
void Client::closeWindow()
{
    if (!isCloseable())
        return;

    // Update user time, because the window may create a confirming dialog.
    updateUserTime();

    if (Pdeletewindow) {
        sendClientMessage(window(), atoms->wm_protocols, atoms->wm_delete_window);
        pingWindow();
    } else // Client will not react on wm_delete_window. We have not choice
        // but destroy his connection to the XServer.
        killWindow();
}


/**
 * Kills the window via XKill
 */
void Client::killWindow()
{
    qDebug() << "Client::killWindow():" << caption();
    killProcess(false);
    m_client.kill();  // Always kill this client at the server
    destroyClient();
}

/**
 * Send a ping to the window using _NET_WM_PING if possible if it
 * doesn't respond within a reasonable time, it will be killed.
 */
void Client::pingWindow()
{
    if (!Pping)
        return; // Can't ping :(
    if (options->killPingTimeout() == 0)
        return; // Turned off
    if (ping_timer != NULL)
        return; // Pinging already
    ping_timer = new QTimer(this);
    connect(ping_timer, &QTimer::timeout, this,
        [this]() {
            qDebug() << "Ping timeout:" << caption();
            ping_timer->deleteLater();
            ping_timer = nullptr;
            killProcess(true, m_pingTimestamp);
        }
    );
    ping_timer->setSingleShot(true);
    ping_timer->start(options->killPingTimeout());
    m_pingTimestamp = xTime();
    workspace()->sendPingToWindow(window(), m_pingTimestamp);
}

void Client::gotPing(xcb_timestamp_t timestamp)
{
    // Just plain compare is not good enough because of 64bit and truncating and whatnot
    if (NET::timestampCompare(timestamp, m_pingTimestamp) != 0)
        return;
    delete ping_timer;
    ping_timer = NULL;
    if (m_killHelperPID && !::kill(m_killHelperPID, 0)) { // means the process is alive
        ::kill(m_killHelperPID, SIGTERM);
        m_killHelperPID = 0;
    }
}

void Client::killProcess(bool ask, xcb_timestamp_t timestamp)
{
    if (m_killHelperPID && !::kill(m_killHelperPID, 0)) // means the process is alive
        return;
    Q_ASSERT(!ask || timestamp != XCB_TIME_CURRENT_TIME);
    pid_t pid = info->pid();
    if (pid <= 0 || clientMachine()->hostName().isEmpty())  // Needed properties missing
        return;
    qDebug() << "Kill process:" << pid << "(" << clientMachine()->hostName() << ")";
    if (!ask) {
        if (!clientMachine()->isLocal()) {
            QStringList lst;
            lst << QString::fromUtf8(clientMachine()->hostName()) << QStringLiteral("kill") << QString::number(pid);
            QProcess::startDetached(QStringLiteral("xon"), lst);
        } else
            ::kill(pid, SIGTERM);
    } else {
        QString hostname = clientMachine()->isLocal() ? QStringLiteral("localhost") : QString::fromUtf8(clientMachine()->hostName());
        QProcess::startDetached(QStringLiteral(KWIN_KILLER_BIN),
                                QStringList() << QStringLiteral("--pid") << QString::number(unsigned(pid)) << QStringLiteral("--hostname") << hostname
                                << QStringLiteral("--windowname") << caption()
                                << QStringLiteral("--applicationname") << QString::fromUtf8(resourceClass())
                                << QStringLiteral("--wid") << QString::number(window())
                                << QStringLiteral("--timestamp") << QString::number(timestamp),
                                QString(), &m_killHelperPID);
    }
}

void Client::setSkipTaskbar(bool b, bool from_outside)
{
    int was_wants_tab_focus = wantsTabFocus();
    if (from_outside) {
        b = rules()->checkSkipTaskbar(b);
        original_skip_taskbar = b;
    }
    if (b == skipTaskbar())
        return;
    skip_taskbar = b;
    info->setState(b ? NET::SkipTaskbar : NET::States(0), NET::SkipTaskbar);
    updateWindowRules(Rules::SkipTaskbar);
    if (was_wants_tab_focus != wantsTabFocus())
        FocusChain::self()->update(this,
                                          isActive() ? FocusChain::MakeFirst : FocusChain::Update);
    emit skipTaskbarChanged();
}

void Client::setSkipPager(bool b)
{
    b = rules()->checkSkipPager(b);
    if (b == skipPager())
        return;
    skip_pager = b;
    info->setState(b ? NET::SkipPager : NET::States(0), NET::SkipPager);
    updateWindowRules(Rules::SkipPager);
    emit skipPagerChanged();
}

void Client::setSkipSwitcher(bool set)
{
    set = rules()->checkSkipSwitcher(set);
    if (set == skipSwitcher())
        return;
    skip_switcher = set;
    updateWindowRules(Rules::SkipSwitcher);
    emit skipSwitcherChanged();
}

void Client::setModal(bool m)
{
    // Qt-3.2 can have even modal normal windows :(
    if (modal == m)
        return;
    modal = m;
    emit modalChanged();
    // Changing modality for a mapped window is weird (?)
    // _NET_WM_STATE_MODAL should possibly rather be _NET_WM_WINDOW_TYPE_MODAL_DIALOG
}

void Client::setDesktop(int desktop)
{
    const int numberOfDesktops = VirtualDesktopManager::self()->count();
    if (desktop != NET::OnAllDesktops)   // Do range check
        desktop = qMax(1, qMin(numberOfDesktops, desktop));
    desktop = qMin(numberOfDesktops, rules()->checkDesktop(desktop));
    if (desk == desktop)
        return;

    int was_desk = desk;
    const bool wasOnCurrentDesktop = isOnCurrentDesktop();
    desk = desktop;
    info->setDesktop(desktop);
    if ((was_desk == NET::OnAllDesktops) != (desktop == NET::OnAllDesktops)) {
        // onAllDesktops changed
        workspace()->updateOnAllDesktopsOfTransients(this);
    }

    ClientList transients_stacking_order = workspace()->ensureStackingOrder(transients());
    for (ClientList::ConstIterator it = transients_stacking_order.constBegin();
            it != transients_stacking_order.constEnd();
            ++it)
        (*it)->setDesktop(desktop);

    if (isModal())  // if a modal dialog is moved, move the mainwindow with it as otherwise
        // the (just moved) modal dialog will confusingly return to the mainwindow with
        // the next desktop change
    {
        foreach (Client * c2, mainClients())
        c2->setDesktop(desktop);
    }

    FocusChain::self()->update(this, FocusChain::MakeFirst);
    updateVisibility();
    updateWindowRules(Rules::Desktop);

    // Update states of all other windows in this group
    if (tabGroup())
        tabGroup()->updateStates(this, TabGroup::Desktop);
    emit desktopChanged();
    if (wasOnCurrentDesktop != isOnCurrentDesktop())
        emit desktopPresenceChanged(this, was_desk);
}

/**
 * Sets whether the client is on @p activity.
 * If you remove it from its last activity, then it's on all activities.
 *
 * Note: If it was on all activities and you try to remove it from one, nothing will happen;
 * I don't think that's an important enough use case to handle here.
 */
void Client::setOnActivity(const QString &activity, bool enable)
{
#ifdef KWIN_BUILD_ACTIVITIES
    QStringList newActivitiesList = activities();
    if (newActivitiesList.contains(activity) == enable)   //nothing to do
        return;
    if (enable) {
        QStringList allActivities = Activities::self()->all();
        if (!allActivities.contains(activity))   //bogus ID
            return;
        newActivitiesList.append(activity);
    } else
        newActivitiesList.removeOne(activity);
    setOnActivities(newActivitiesList);
#else
    Q_UNUSED(activity)
    Q_UNUSED(enable)
#endif
}

/**
 * set exactly which activities this client is on
 */
void Client::setOnActivities(QStringList newActivitiesList)
{
#ifdef KWIN_BUILD_ACTIVITIES
    QString joinedActivitiesList = newActivitiesList.join(QStringLiteral(","));
    joinedActivitiesList = rules()->checkActivity(joinedActivitiesList, false);
    newActivitiesList = joinedActivitiesList.split(QStringLiteral(","), QString::SkipEmptyParts);

    QStringList allActivities = Activities::self()->all();
    if ( newActivitiesList.isEmpty() ||
        (newActivitiesList.count() > 1 && newActivitiesList.count() == allActivities.count()) ||
        (newActivitiesList.count() == 1 && newActivitiesList.at(0) == Activities::nullUuid())) {
        activityList.clear();
        const QByteArray nullUuid = Activities::nullUuid().toUtf8();
        m_client.changeProperty(atoms->activities, XCB_ATOM_STRING, 8, nullUuid.length(), nullUuid.constData());

    } else {
        QByteArray joined = joinedActivitiesList.toAscii();
        activityList = newActivitiesList;
        m_client.changeProperty(atoms->activities, XCB_ATOM_STRING, 8, joined.length(), joined.constData());
    }

    updateActivities(false);
#else
    Q_UNUSED(newActivitiesList)
#endif
}

void Client::blockActivityUpdates(bool b)
{
    if (b) {
        ++m_activityUpdatesBlocked;
    } else {
        Q_ASSERT(m_activityUpdatesBlocked);
        --m_activityUpdatesBlocked;
        if (!m_activityUpdatesBlocked)
            updateActivities(m_blockedActivityUpdatesRequireTransients);
    }
}

/**
 * update after activities changed
 */
void Client::updateActivities(bool includeTransients)
{
    if (m_activityUpdatesBlocked) {
        m_blockedActivityUpdatesRequireTransients |= includeTransients;
        return;
    }
    emit activitiesChanged(this);
    m_blockedActivityUpdatesRequireTransients = false; // reset
    FocusChain::self()->update(this, FocusChain::MakeFirst);
    updateVisibility();
    updateWindowRules(Rules::Activity);

    // Update states of all other windows in this group
    if (tabGroup())
        tabGroup()->updateStates(this, TabGroup::Activity);
}

/**
 * Returns the virtual desktop within the workspace() the client window
 * is located in, 0 if it isn't located on any special desktop (not mapped yet),
 * or NET::OnAllDesktops. Do not use desktop() directly, use
 * isOnDesktop() instead.
 */
int Client::desktop() const
{
    if (needsSessionInteract) {
        return NET::OnAllDesktops;
    }
    return desk;
}

/**
 * Returns the list of activities the client window is on.
 * if it's on all activities, the list will be empty.
 * Don't use this, use isOnActivity() and friends (from class Toplevel)
 */
QStringList Client::activities() const
{
    if (needsSessionInteract) {
        return QStringList();
    }
    return activityList;
}

void Client::setOnAllDesktops(bool b)
{
    if ((b && isOnAllDesktops()) ||
            (!b && !isOnAllDesktops()))
        return;
    if (b)
        setDesktop(NET::OnAllDesktops);
    else
        setDesktop(VirtualDesktopManager::self()->current());

    // Update states of all other windows in this group
    if (tabGroup())
        tabGroup()->updateStates(this, TabGroup::Desktop);
}

/**
 * if @p on is true, sets on all activities.
 * if it's false, sets it to only be on the current activity
 */
void Client::setOnAllActivities(bool on)
{
#ifdef KWIN_BUILD_ACTIVITIES
    if (on == isOnAllActivities())
        return;
    if (on) {
        setOnActivities(QStringList());

    } else {
        setOnActivity(Activities::self()->current(), true);
    }
#endif
}

/**
 * Performs the actual focusing of the window using XSetInputFocus and WM_TAKE_FOCUS
 */
void Client::takeFocus()
{
#ifndef NDEBUG
    static Time previous_focus_timestamp;
    static Client* previous_client;

    //if ( previous_focus_timestamp == xTime() && previous_client != this )
    //    {
    //    qDebug() << "Repeated use of the same X timestamp for focus";
    //    qDebug() << kBacktrace();
    //    }

    previous_focus_timestamp = xTime();
    previous_client = this;
#endif
    if (rules()->checkAcceptFocus(input))
        m_client.focus();
    else
        demandAttention(false); // window cannot take input, at least withdraw urgency
    if (Ptakefocus)
        sendClientMessage(window(), atoms->wm_protocols, atoms->wm_take_focus);
    workspace()->setShouldGetFocus(this);
}

/**
 * Returns whether the window provides context help or not. If it does,
 * you should show a help menu item or a help button like '?' and call
 * contextHelp() if this is invoked.
 *
 * \sa contextHelp()
 */
bool Client::providesContextHelp() const
{
    return Pcontexthelp;
}

/**
 * Invokes context help on the window. Only works if the window
 * actually provides context help.
 *
 * \sa providesContextHelp()
 */
void Client::showContextHelp()
{
    if (Pcontexthelp) {
        sendClientMessage(window(), atoms->wm_protocols, atoms->net_wm_context_help);
        QWhatsThis::enterWhatsThisMode(); // SELI TODO: ?
    }
}

/**
 * Fetches the window's caption (WM_NAME property). It will be
 * stored in the client's caption().
 */
void Client::fetchName()
{
    setCaption(readName());
}

QString Client::readName() const
{
    if (info->name() && info->name()[0] != '\0')
        return QString::fromUtf8(info->name());
    else
        return KWindowSystem::readNameProperty(window(), XCB_ATOM_WM_NAME);
}

// The list is taken from http://www.unicode.org/reports/tr9/ (#154840)
QChar LRM(0x200E);
QChar RLM(0x200F);
QChar LRE(0x202A);
QChar RLE(0x202B);
QChar LRO(0x202D);
QChar RLO(0x202E);
QChar PDF(0x202C);

void Client::setCaption(const QString& _s, bool force)
{
    if (!force && _s == cap_normal)
        return;
    QString s(_s);
    for (int i = 0; i < s.length(); ++i)
        if (!s[i].isPrint())
            s[i] = QChar(u' ');
    cap_normal = s;
    if (options->condensedTitle()) {
        static QScriptEngine engine;
        static QScriptProgram stripTitle;
        static QScriptValue script;
        if (stripTitle.isNull()) {
            const QString scriptFile = QStandardPaths::locate(QStandardPaths::GenericDataLocation, QStringLiteral(KWIN_NAME) + QStringLiteral("/stripTitle.js"));
            if (!scriptFile.isEmpty()) {
                QFile f(scriptFile);
                if (f.open(QIODevice::ReadOnly|QIODevice::Text)) {
                    f.reset();
                    stripTitle = QScriptProgram(QString::fromLocal8Bit(f.readAll()), QStringLiteral("stripTitle.js"));
                    f.close();
                }
            }
            if (stripTitle.isNull())
                stripTitle = QScriptProgram(QStringLiteral("(function(title, wm_name, wm_class){ return title ; })"), QStringLiteral("stripTitle.js"));
            script = engine.evaluate(stripTitle);
        }
        QScriptValueList args;
        args << _s << QString::fromUtf8(resourceName()) << QString::fromUtf8(resourceClass());
        s = script.call(QScriptValue(), args).toString();
    }
    if (!force && s == cap_deco)
        return;
    cap_deco = s;

    bool reset_name = force;
    bool was_suffix = (!cap_suffix.isEmpty());
    cap_suffix.clear();
    QString machine_suffix;
    if (!options->condensedTitle()) { // machine doesn't qualify for "clean"
        if (clientMachine()->hostName() != ClientMachine::localhost() && !clientMachine()->isLocal())
            machine_suffix = QStringLiteral(" <@") + QString::fromUtf8(clientMachine()->hostName()) + QStringLiteral(">") + LRM;
    }
    QString shortcut_suffix = !shortcut().isEmpty() ? (QStringLiteral(" {") + shortcut().toString() + QStringLiteral("}")) : QString();
    cap_suffix = machine_suffix + shortcut_suffix;
    auto fetchNameInternalPredicate = [this](const Client *cl) {
        return (!cl->isSpecialWindow() || cl->isToolbar()) &&
                cl != this && cl->caption() == caption();
    };
    if ((!isSpecialWindow() || isToolbar()) && workspace()->findClient(fetchNameInternalPredicate)) {
        int i = 2;
        do {
            cap_suffix = machine_suffix + QStringLiteral(" <") + QString::number(i) + QStringLiteral(">") + LRM;
            i++;
        } while (workspace()->findClient(fetchNameInternalPredicate));
        info->setVisibleName(caption().toUtf8().constData());
        reset_name = false;
    }
    if ((was_suffix && cap_suffix.isEmpty()) || reset_name) {
        // If it was new window, it may have old value still set, if the window is reused
        info->setVisibleName("");
        info->setVisibleIconName("");
    } else if (!cap_suffix.isEmpty() && !cap_iconic.isEmpty())
        // Keep the same suffix in iconic name if it's set
        info->setVisibleIconName(QString(cap_iconic + cap_suffix).toUtf8().constData());

    emit captionChanged();
}

void Client::updateCaption()
{
    setCaption(cap_normal, true);
}

void Client::fetchIconicName()
{
    QString s;
    if (info->iconName() && info->iconName()[0] != '\0')
        s = QString::fromUtf8(info->iconName());
    else
        s = KWindowSystem::readNameProperty(window(), XCB_ATOM_WM_ICON_NAME);
    if (s != cap_iconic) {
        bool was_set = !cap_iconic.isEmpty();
        cap_iconic = s;
        if (!cap_suffix.isEmpty()) {
            if (!cap_iconic.isEmpty())  // Keep the same suffix in iconic name if it's set
                info->setVisibleIconName(QString(s + cap_suffix).toUtf8().constData());
            else if (was_set)
                info->setVisibleIconName("");
        }
    }
}

/**
 * \reimp
 */
QString Client::caption(bool full, bool stripped) const
{
    QString cap = stripped ? cap_deco : cap_normal;
    if (full)
        cap += cap_suffix;
    return cap;
}

bool Client::tabTo(Client *other, bool behind, bool activate)
{
    Q_ASSERT(other && other != this);

    if (tab_group && tab_group == other->tabGroup()) { // special case: move inside group
        tab_group->move(this, other, behind);
        return true;
    }

    GeometryUpdatesBlocker blocker(this);
    const bool wasBlocking = signalsBlocked();
    blockSignals(true); // prevent client emitting "retabbed to nowhere" cause it's about to be entabbed the next moment
    untab();
    blockSignals(wasBlocking);

    TabGroup *newGroup = other->tabGroup() ? other->tabGroup() : new TabGroup(other);

    if (!newGroup->add(this, other, behind, activate)) {
        if (newGroup->count() < 2) { // adding "c" to "to" failed for whatever reason
            newGroup->remove(other);
            delete newGroup;
        }
        return false;
    }
    return true;
}

bool Client::untab(const QRect &toGeometry, bool clientRemoved)
{
    TabGroup *group = tab_group;
    if (group && group->remove(this)) { // remove sets the tabgroup to "0", therefore the pointer is cached
        if (group->isEmpty()) {
            delete group;
        }
        if (clientRemoved)
            return true; // there's been a broadcast signal that this client is now removed - don't touch it
        setClientShown(!(isMinimized() || isShade()));
        bool keepSize = toGeometry.size() == size();
        bool changedSize = false;
        if (quickTileMode() != QuickTileNone) {
            changedSize = true;
            setQuickTileMode(QuickTileNone); // if we leave a quicktiled group, assume that the user wants to untile
        }
        if (toGeometry.isValid()) {
            if (maximizeMode() != Client::MaximizeRestore) {
                changedSize = true;
                maximize(Client::MaximizeRestore); // explicitly calling for a geometry -> unmaximize
            }
            if (keepSize && changedSize) {
                geom_restore = geometry(); // checkWorkspacePosition() invokes it
                QPoint cpoint = Cursor::pos();
                QPoint point = cpoint;
                point.setX((point.x() - toGeometry.x()) * geom_restore.width() / toGeometry.width());
                point.setY((point.y() - toGeometry.y()) * geom_restore.height() / toGeometry.height());
                geom_restore.moveTo(cpoint-point);
            } else {
                geom_restore = toGeometry; // checkWorkspacePosition() invokes it
            }
            setGeometry(geom_restore);
            checkWorkspacePosition();
        }
        return true;
    }
    return false;
}

void Client::setTabGroup(TabGroup *group)
{
    tab_group = group;
    if (group) {
        unsigned long data[] = {qHash(group)}; //->id();
        m_client.changeProperty(atoms->kde_net_wm_tab_group, XCB_ATOM_CARDINAL, 32, 1, data);
    }
    else
        m_client.deleteProperty(atoms->kde_net_wm_tab_group);
    emit tabGroupChanged();
}

bool Client::isCurrentTab() const
{
    return !tab_group || tab_group->current() == this;
}

void Client::syncTabGroupFor(QString property, bool fromThisClient)
{
    if (tab_group)
        tab_group->sync(property.toAscii().data(), fromThisClient ? this : tab_group->current());
}

void Client::dontMoveResize()
{
    buttonDown = false;
    stopDelayedMoveResize();
    if (moveResizeMode)
        finishMoveResize(false);
}

void Client::setClientShown(bool shown)
{
    if (deleting)
        return; // Don't change shown status if this client is being deleted
    if (shown != hidden)
        return; // nothing to change
    hidden = !shown;
    if (options->isInactiveTabsSkipTaskbar())
        setSkipTaskbar(hidden, false); // TODO: Causes reshuffle of the taskbar
    if (shown) {
        map();
        takeFocus();
        autoRaise();
        FocusChain::self()->update(this, FocusChain::MakeFirst);
    } else {
        unmap();
        // Don't move tabs to the end of the list when another tab get's activated
        if (isCurrentTab())
            FocusChain::self()->update(this, FocusChain::MakeLast);
        addWorkspaceRepaint(visibleRect());
    }
}

void Client::getWMHints()
{
    XWMHints* hints = XGetWMHints(display(), window());
    input = true;
    m_windowGroup = XCB_WINDOW_NONE;
    urgency = false;
    if (hints) {
        if (hints->flags & InputHint)
            input = hints->input;
        if (hints->flags & WindowGroupHint)
            m_windowGroup = hints->window_group;
        urgency = !!(hints->flags & UrgencyHint);   // Need boolean, it's a uint bitfield
        XFree((char*)hints);
    }
    checkGroup();
    updateUrgency();
    updateAllowedActions(); // Group affects isMinimizable()
}

void Client::getMotifHints()
{
    bool mgot_noborder, mnoborder, mresize, mmove, mminimize, mmaximize, mclose;
    Motif::readFlags(m_client, mgot_noborder, mnoborder, mresize, mmove, mminimize, mmaximize, mclose);
    if (mgot_noborder && motif_noborder != mnoborder) {
        motif_noborder = mnoborder;
        // If we just got a hint telling us to hide decorations, we do so.
        if (motif_noborder)
            noborder = rules()->checkNoBorder(true);
        // If the Motif hint is now telling us to show decorations, we only do so if the app didn't
        // instruct us to hide decorations in some other way, though.
        else if (!app_noborder)
            noborder = rules()->checkNoBorder(false);
    }
    if (!hasNETSupport()) {
        // NETWM apps should set type and size constraints
        motif_may_resize = mresize; // This should be set using minsize==maxsize, but oh well
        motif_may_move = mmove;
    } else
        motif_may_resize = motif_may_move = true;

    // mminimize; - Ignore, bogus - E.g. shading or sending to another desktop is "minimizing" too
    // mmaximize; - Ignore, bogus - Maximizing is basically just resizing
    const bool closabilityChanged = motif_may_close != mclose;
    motif_may_close = mclose; // Motif apps like to crash when they set this hint and WM closes them anyway
    if (isManaged())
        updateDecoration(true);   // Check if noborder state has changed
    if (decoration && closabilityChanged)
        emit decoration->decorationButtonsChanged();
}

void Client::getIcons()
{
    // First read icons from the window itself
    m_icon = QIcon();
    auto readIcon = [this](int size, bool scale = true) {
        const QPixmap pix = KWindowSystem::icon(window(), size, size, scale, KWindowSystem::NETWM | KWindowSystem::WMHints);
        if (!pix.isNull()) {
            m_icon.addPixmap(pix);
        }
    };
    readIcon(16);
    readIcon(32);
    readIcon(48, false);
    readIcon(64, false);
    readIcon(128, false);
    if (m_icon.isNull()) {
        // Then try window group
        m_icon = group()->icon();
    }
    if (m_icon.isNull() && isTransient()) {
        // Then mainclients
        ClientList mainclients = mainClients();
        for (ClientList::ConstIterator it = mainclients.constBegin();
                it != mainclients.constEnd() && m_icon.isNull();
                ++it) {
            if (!(*it)->icon().isNull()) {
                m_icon = (*it)->icon();
                break;
            }
        }
    }
    if (m_icon.isNull()) {
        // And if nothing else, load icon from classhint or xapp icon
        m_icon.addPixmap(KWindowSystem::icon(window(),  32,  32,  true, KWindowSystem::ClassHint | KWindowSystem::XApp));
        m_icon.addPixmap(KWindowSystem::icon(window(),  16,  16,  true, KWindowSystem::ClassHint | KWindowSystem::XApp));
        m_icon.addPixmap(KWindowSystem::icon(window(),  64,  64, false, KWindowSystem::ClassHint | KWindowSystem::XApp));
        m_icon.addPixmap(KWindowSystem::icon(window(), 128, 128, false, KWindowSystem::ClassHint | KWindowSystem::XApp));
    }
    emit iconChanged();
}

void Client::getWindowProtocols()
{
    Atom* p;
    int i, n;

    Pdeletewindow = 0;
    Ptakefocus = 0;
    Pcontexthelp = 0;
    Pping = 0;

    if (XGetWMProtocols(display(), window(), &p, &n)) {
        for (i = 0; i < n; ++i) {
            if (p[i] == atoms->wm_delete_window)
                Pdeletewindow = 1;
            else if (p[i] == atoms->wm_take_focus)
                Ptakefocus = 1;
            else if (p[i] == atoms->net_wm_context_help)
                Pcontexthelp = 1;
            else if (p[i] == atoms->net_wm_ping)
                Pping = 1;
        }
        if (n > 0)
            XFree(p);
    }
}

void Client::getSyncCounter()
{
    if (!Xcb::Extensions::self()->isSyncAvailable())
        return;

    Xcb::Property syncProp(false, window(), atoms->net_wm_sync_request_counter, XCB_ATOM_CARDINAL, 0, 1);
    const xcb_sync_counter_t counter = syncProp.value<xcb_sync_counter_t>(XCB_NONE);
    if (counter != XCB_NONE) {
        syncRequest.counter = counter;
        syncRequest.value.hi = 0;
        syncRequest.value.lo = 0;
        auto *c = connection();
        xcb_sync_set_counter(c, syncRequest.counter, syncRequest.value);
        if (syncRequest.alarm == XCB_NONE) {
            const uint32_t mask = XCB_SYNC_CA_COUNTER | XCB_SYNC_CA_VALUE_TYPE | XCB_SYNC_CA_TEST_TYPE | XCB_SYNC_CA_EVENTS;
            const uint32_t values[] = {
                syncRequest.counter,
                XCB_SYNC_VALUETYPE_RELATIVE,
                XCB_SYNC_TESTTYPE_POSITIVE_TRANSITION,
                1
            };
            syncRequest.alarm = xcb_generate_id(c);
            auto cookie = xcb_sync_create_alarm_checked(c, syncRequest.alarm, mask, values);
            ScopedCPointer<xcb_generic_error_t> error(xcb_request_check(c, cookie));
            if (!error.isNull()) {
                syncRequest.alarm = XCB_NONE;
            } else {
                XSyncAlarmAttributes attrs;
                XSyncIntToValue(&attrs.trigger.wait_value, 1);
                XSyncIntToValue(&attrs.delta, 1);
                XSyncChangeAlarm(display(), syncRequest.alarm, XSyncCADelta | XSyncCAValue, &attrs);
            }
        }
    }
}

/**
 * Send the client a _NET_SYNC_REQUEST
 */
void Client::sendSyncRequest()
{
    if (syncRequest.counter == XCB_NONE || syncRequest.isPending)
        return; // do NOT, NEVER send a sync request when there's one on the stack. the clients will just stop respoding. FOREVER! ...

    if (!syncRequest.failsafeTimeout) {
        syncRequest.failsafeTimeout = new QTimer(this);
        connect(syncRequest.failsafeTimeout, &QTimer::timeout, this,
            [this]() {
                // client does not respond to XSYNC requests in reasonable time, remove support
                if (!ready_for_painting) {
                    // failed on initial pre-show request
                    setReadyForPainting();
                    return;
                }
                // failed during resize
                syncRequest.isPending = false;
                syncRequest.counter = syncRequest.alarm = XCB_NONE;
                delete syncRequest.timeout; delete syncRequest.failsafeTimeout;
                syncRequest.timeout = syncRequest.failsafeTimeout = nullptr;
                syncRequest.lastTimestamp = XCB_CURRENT_TIME;
            }
        );
        syncRequest.failsafeTimeout->setSingleShot(true);
    }
    // if there's no response within 10 seconds, sth. went wrong and we remove XSYNC support from this client.
    // see events.cpp Client::syncEvent()
    syncRequest.failsafeTimeout->start(ready_for_painting ? 10000 : 1000);

    // We increment before the notify so that after the notify
    // syncCounterSerial will equal the value we are expecting
    // in the acknowledgement
    const uint32_t oldLo = syncRequest.value.lo;
    syncRequest.value.lo++;;
    if (oldLo > syncRequest.value.lo) {
        syncRequest.value.hi++;
    }
    if (syncRequest.lastTimestamp >= xTime()) {
        updateXTime();
    }

    // Send the message to client
    sendClientMessage(window(), atoms->wm_protocols, atoms->net_wm_sync_request, syncRequest.value.lo, syncRequest.value.hi);
    syncRequest.isPending = true;
    syncRequest.lastTimestamp = xTime();
}

bool Client::wantsTabFocus() const
{
    return (isNormalWindow() || isDialog()) && wantsInput();
}

bool Client::wantsInput() const
{
    return rules()->checkAcceptFocus(input || Ptakefocus);
}

bool Client::isSpecialWindow() const
{
    // TODO
    return isDesktop() || isDock() || isSplash() || isToolbar();
}

/**
 * Sets an appropriate cursor shape for the logical mouse position \a m
 */
void Client::updateCursor()
{
    Position m = mode;
    if (!isResizable() || isShade())
        m = PositionCenter;
    Qt::CursorShape c = Qt::ArrowCursor;
    switch(m) {
    case PositionTopLeft:
    case PositionBottomRight:
        c = Qt::SizeFDiagCursor;
        break;
    case PositionBottomLeft:
    case PositionTopRight:
        c = Qt::SizeBDiagCursor;
        break;
    case PositionTop:
    case PositionBottom:
        c = Qt::SizeVerCursor;
        break;
    case PositionLeft:
    case PositionRight:
        c = Qt::SizeHorCursor;
        break;
    default:
        if (moveResizeMode)
            c = Qt::SizeAllCursor;
        else
            c = Qt::ArrowCursor;
        break;
    }
    if (c == m_cursor)
        return;
    m_cursor = c;
    if (decoration != NULL)
        decoration->window()->setCursor(m_cursor);
    xcb_cursor_t nativeCursor = Cursor::x11Cursor(m_cursor);
    m_frame.defineCursor(nativeCursor);
    if (m_decoInputExtent.isValid())
        m_decoInputExtent.defineCursor(nativeCursor);
    if (moveResizeMode) {
        // changing window attributes doesn't change cursor if there's pointer grab active
        xcb_change_active_pointer_grab(connection(), nativeCursor, xTime(),
            XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW);
    }
}

void Client::setBlockingCompositing(bool block)
{
    const bool usedToBlock = blocks_compositing;
    blocks_compositing = rules()->checkBlockCompositing(block);
    if (usedToBlock != blocks_compositing) {
        emit blockingCompositingChanged(blocks_compositing ? this : 0);
    }
}

Client::Position Client::mousePosition(const QPoint& p) const
{
    if (decoration != NULL)
        return decoration->mousePosition(p);
    return PositionCenter;
}

void Client::updateAllowedActions(bool force)
{
    if (!isManaged() && !force)
        return;
    NET::Actions old_allowed_actions = NET::Actions(allowed_actions);
    allowed_actions = 0;
    if (isMovable())
        allowed_actions |= NET::ActionMove;
    if (isResizable())
        allowed_actions |= NET::ActionResize;
    if (isMinimizable())
        allowed_actions |= NET::ActionMinimize;
    if (isShadeable())
        allowed_actions |= NET::ActionShade;
    // Sticky state not supported
    if (isMaximizable())
        allowed_actions |= NET::ActionMax;
    if (userCanSetFullScreen())
        allowed_actions |= NET::ActionFullScreen;
    allowed_actions |= NET::ActionChangeDesktop; // Always (Pagers shouldn't show Docks etc.)
    if (isCloseable())
        allowed_actions |= NET::ActionClose;
    if (old_allowed_actions == allowed_actions)
        return;
    // TODO: This could be delayed and compressed - It's only for pagers etc. anyway
    info->setAllowedActions(allowed_actions);

    // ONLY if relevant features have changed (and the window didn't just get/loose moveresize for maximization state changes)
    const NET::Actions relevant = ~(NET::ActionMove|NET::ActionResize);
    if (decoration && (allowed_actions & relevant) != (old_allowed_actions & relevant))
        emit decoration->decorationButtonsChanged();
}

void Client::autoRaise()
{
    workspace()->raiseClient(this);
    cancelAutoRaise();
}

void Client::cancelAutoRaise()
{
    delete autoRaiseTimer;
    autoRaiseTimer = 0;
}

void Client::debug(QDebug& stream) const
{
    print<QDebug>(stream);
}

void Client::checkActivities()
{
#ifdef KWIN_BUILD_ACTIVITIES
    QStringList newActivitiesList;
    QByteArray prop = Xcb::StringProperty(window(), atoms->activities);
    activitiesDefined = !prop.isEmpty();
    if (QString::fromUtf8(prop) == Activities::nullUuid()) {
        //copied from setOnAllActivities to avoid a redundant XChangeProperty.
        if (!activityList.isEmpty()) {
            activityList.clear();
            updateActivities(true);
        }
        return;
    }
    if (prop.isEmpty()) {
        //note: this makes it *act* like it's on all activities but doesn't set the property to 'ALL'
        if (!activityList.isEmpty()) {
            activityList.clear();
            updateActivities(true);
        }
        return;
    }

    newActivitiesList = QString::fromUtf8(prop).split(QStringLiteral(","));

    if (newActivitiesList == activityList)
        return; //expected change, it's ok.

    //otherwise, somebody else changed it. we need to validate before reacting
    QStringList allActivities = Activities::self()->all();
    if (allActivities.isEmpty()) {
        qDebug() << "no activities!?!?";
        //don't touch anything, there's probably something bad going on and we don't wanna make it worse
        return;
    }
    for (int i = 0; i < newActivitiesList.size(); ++i) {
        if (! allActivities.contains(newActivitiesList.at(i))) {
            qDebug() << "invalid:" << newActivitiesList.at(i);
            newActivitiesList.removeAt(i--);
        }
    }
    setOnActivities(newActivitiesList);
#endif
}

void Client::setSessionInteract(bool needed)
{
    needsSessionInteract = needed;
}

QRect Client::decorationRect() const
{
    if (decoration) {
        return decoration->rect().translated(-padding_left, -padding_top);
    } else {
        return QRect(0, 0, width(), height());
    }
}

KDecorationDefines::Position Client::titlebarPosition() const
{
    Position titlePos = PositionCenter; // PositionTop is returned by the default implementation
                                        // this will hint errors in the metaobject usage ;-)
    if (decoration)
        QMetaObject::invokeMethod(decoration, "titlebarPosition", Qt::DirectConnection,
                                            Q_RETURN_ARG(KDecorationDefines::Position, titlePos));
    return titlePos;
}

void Client::updateFirstInTabBox()
{
    // TODO: move into KWindowInfo
    Xcb::Property property(false, m_client, atoms->kde_first_in_window_list,
                           atoms->kde_first_in_window_list, 0, 1);
    setFirstInTabBox(property.toBool(32, atoms->kde_first_in_window_list));
}

void Client::updateColorScheme()
{
    // TODO: move into KWindowInfo
    QString path = QString::fromUtf8(Xcb::StringProperty(m_client, atoms->kde_color_sheme));
    path = rules()->checkDecoColor(path);
    if (!path.isNull()) {
        m_palette = KColorScheme::createApplicationPalette(KSharedConfig::openConfig(path));
    } else {
        m_palette = QApplication::palette();
    }
    triggerDecorationRepaint();
}

bool Client::isClient() const
{
    return true;
}

#ifdef KWIN_BUILD_KAPPMENU
void Client::setAppMenuAvailable()
{
    m_menuAvailable = true;
    emit appMenuAvailable();
}

void Client::setAppMenuUnavailable()
{
    m_menuAvailable = false;
    emit appMenuUnavailable();
}

void Client::showApplicationMenu(const QPoint &p)
{
    ApplicationMenu::self()->showApplicationMenu(p, window());
}
#endif

NET::WindowType Client::windowType(bool direct, int supportedTypes) const
{
    // TODO: does it make sense to cache the returned window type for SUPPORTED_MANAGED_WINDOW_TYPES_MASK?
    if (supportedTypes == 0) {
        supportedTypes = SUPPORTED_MANAGED_WINDOW_TYPES_MASK;
    }
    NET::WindowType wt = info->windowType(NET::WindowTypes(supportedTypes));
    if (direct) {
        return wt;
    }
    NET::WindowType wt2 = client_rules.checkType(wt);
    if (wt != wt2) {
        wt = wt2;
        info->setWindowType(wt);   // force hint change
    }
    // hacks here
    if (wt == NET::Unknown)   // this is more or less suggested in NETWM spec
        wt = isTransient() ? NET::Dialog : NET::Normal;
    return wt;
}

bool Client::decorationHasAlpha() const
{
    if (!decoration || !decorationPlugin()->hasAlpha()) {
        // either no decoration or decoration has alpha disabled
        return false;
    }
    if (decorationPlugin()->supportsAnnounceAlpha()) {
        return decoration->isAlphaEnabled();
    } else {
        // decoration has alpha enabled and does not support alpha announcement
        return true;
    }
}

void Client::cancelFocusOutTimer()
{
    if (m_focusOutTimer) {
        m_focusOutTimer->stop();
    }
}

xcb_window_t Client::frameId() const
{
    return m_frame;
}

void Client::updateShowOnScreenEdge()
{
    Xcb::Property property(false, window(), atoms->kde_screen_edge_show, XCB_ATOM_CARDINAL, 0, 1);
    const uint32_t value = property.value<uint32_t>(ElectricNone);
    ElectricBorder border = ElectricNone;
    switch (value) {
    case 0:
        border = ElectricTop;
        break;
    case 1:
        border = ElectricRight;
        break;
    case 2:
        border = ElectricBottom;
        break;
    case 3:
        border = ElectricLeft;
        break;
    }
    if (border != ElectricNone) {
        hideClient(true);
        ScreenEdges::self()->reserve(this, border);
    } else if (!property.isNull() && property->type != XCB_ATOM_NONE) {
        // property value is incorrect, delete the property
        // so that the client knows that it is not hidden
        xcb_delete_property(connection(), window(), atoms->kde_screen_edge_show);
    } else {
        // restore
        // TODO: add proper unreserve
        ScreenEdges::self()->reserve(this, ElectricNone);
        hideClient(false);
    }
}

void Client::showOnScreenEdge()
{
    hideClient(false);
    xcb_delete_property(connection(), window(), atoms->kde_screen_edge_show);
}

void Client::sendPointerButtonEvent(uint32_t button, InputRedirection::PointerButtonState state)
{
    // TODO: don't use xtest
    uint8_t type = XCB_BUTTON_PRESS;
    if (state == KWin::InputRedirection::PointerButtonReleased) {
        type = XCB_BUTTON_RELEASE;
    }
    xcb_test_fake_input(connection(), type, InputRedirection::toXPointerButton(button), XCB_TIME_CURRENT_TIME, frameId(), 0, 0, 0);
}

void Client::sendPointerAxisEvent(InputRedirection::PointerAxis axis, qreal delta)
{
    // TODO: don't use xtest
    const int val = qRound(delta);
    if (val == 0) {
        return;
    }
    const uint8_t button = InputRedirection::toXPointerButton(axis, delta);
    for (int i = 0; i < qAbs(val); ++i) {
        xcb_test_fake_input(connection(), XCB_BUTTON_PRESS, button, XCB_TIME_CURRENT_TIME, frameId(), 0, 0, 0);
        xcb_test_fake_input(connection(), XCB_BUTTON_RELEASE, button, XCB_TIME_CURRENT_TIME, frameId(), 0, 0, 0);
    }
}

void Client::sendKeybordKeyEvent(uint32_t key, InputRedirection::KeyboardKeyState state)
{
    // TODO: don't use xtest
    uint8_t type = XCB_KEY_PRESS;
    if (state == InputRedirection::KeyboardKeyReleased) {
        type = XCB_KEY_RELEASE;
    }
    xcb_test_fake_input(connection(), type, key + 8, XCB_TIME_CURRENT_TIME, frameId(), 0, 0, 0);
}

} // namespace

#include "client.moc"
