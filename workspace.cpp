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
#include "workspace.h"
// kwin libs
#include <kdecorationfactory.h>
#include <kwinglplatform.h>
#include <kwinxrenderutils.h>
// kwin
#ifdef KWIN_BUILD_ACTIVITIES
#include "activities.h"
#endif
#ifdef KWIN_BUILD_KAPPMENU
#include "appmenu.h"
#endif
#include "atoms.h"
#include "client.h"
#include "composite.h"
#include "cursor.h"
#include "dbusinterface.h"
#include "decorations.h"
#include "deleted.h"
#include "effects.h"
#include "focuschain.h"
#include "group.h"
#include "input.h"
#include "killwindow.h"
#include "netinfo.h"
#include "outline.h"
#include "placement.h"
#include "rules.h"
#ifdef KWIN_BUILD_SCREENEDGES
#include "screenedge.h"
#endif
#include "screens.h"
#include "scripting/scripting.h"
#ifdef KWIN_BUILD_TABBOX
#include "tabbox.h"
#endif
#include "unmanaged.h"
#include "useractions.h"
#include "virtualdesktops.h"
#if HAVE_WAYLAND
#include "wayland_backend.h"
#endif
#include "xcbutils.h"
#include "main.h"
// KDE
#include <KConfig>
#include <KConfigGroup>
#include <KLocalizedString>
#include <KStartupInfo>
#include <KWindowInfo>
#include <KWindowSystem>
// Qt
#include <QtConcurrentRun>

namespace KWin
{

extern int screen_number;
extern bool is_multihead;

ColorMapper::ColorMapper(QObject *parent)
    : QObject(parent)
    , m_default(defaultScreen()->default_colormap)
    , m_installed(defaultScreen()->default_colormap)
{
}

ColorMapper::~ColorMapper()
{
}

void ColorMapper::update()
{
    xcb_colormap_t cmap = m_default;
    if (Client *c = Workspace::self()->activeClient()) {
        if (c->colormap() != XCB_COLORMAP_NONE) {
            cmap = c->colormap();
        }
    }
    if (cmap != m_installed) {
        xcb_install_colormap(connection(), cmap);
        m_installed = cmap;
    }
}

Workspace* Workspace::_self = 0;

Workspace::Workspace(bool restore)
    : QObject(0)
    , m_compositor(NULL)
    // Unsorted
    , active_popup(NULL)
    , active_popup_client(NULL)
    , active_client(0)
    , last_active_client(0)
    , most_recently_raised(0)
    , movingClient(0)
    , delayfocus_client(0)
    , force_restacking(false)
    , x_stacking_dirty(true)
    , showing_desktop(false)
    , block_showing_desktop(0)
    , was_user_interaction(false)
    , session_saving(false)
    , block_focus(0)
    , m_userActionsMenu(new UserActionsMenu(this))
    , client_keys_dialog(NULL)
    , client_keys_client(NULL)
    , global_shortcuts_disabled_for_client(false)
    , workspaceInit(true)
    , startup(0)
    , set_active_client_recursion(0)
    , block_stacking_updates(0)
{
    // If KWin was already running it saved its configuration after loosing the selection -> Reread
    QFuture<void> reparseConfigFuture = QtConcurrent::run(options, &Options::reparseConfiguration);

#ifdef KWIN_BUILD_KAPPMENU
    ApplicationMenu::create(this);
#endif

    _self = this;

    // first initialize the extensions
    Xcb::Extensions::self();

    InputRedirection::create(this);

    // start the Wayland Backend - will only be created if WAYLAND_DISPLAY is present
#if HAVE_WAYLAND
    Wayland::WaylandBackend::create(this);
    if (kwinApp()->operationMode() != Application::OperationModeX11) {
        connect(this, SIGNAL(stackingOrderChanged()), input(), SLOT(updatePointerWindow()));
    }
#endif
    // start the cursor support
    Cursor::create(this);

#ifdef KWIN_BUILD_ACTIVITIES
    Activities *activities = Activities::create(this);
    connect(activities, SIGNAL(currentChanged(QString)), SLOT(updateCurrentActivity(QString)));
#endif

    // PluginMgr needs access to the config file, so we need to wait for it for finishing
    reparseConfigFuture.waitForFinished();

    // get screen support
    Screens *screens = Screens::create(this);
    connect(screens, SIGNAL(changed()), SLOT(desktopResized()));

    options->loadConfig();
    options->loadCompositingConfig(false);
    DecorationPlugin::create(this);
    ColorMapper *colormaps = new ColorMapper(this);
    connect(this, SIGNAL(clientActivated(KWin::Client*)), colormaps, SLOT(update()));

    delayFocusTimer = 0;

    if (restore)
        loadSessionInfo();

    RuleBook::create(this)->load();

    // Call this before XSelectInput() on the root window
    startup = new KStartupInfo(
        KStartupInfo::DisableKWinModule | KStartupInfo::AnnounceSilenceChanges, this);

    // Select windowmanager privileges
    selectWmInputEventMask();

#if QT_VERSION < 0x050302
    // WORKAROUND: QXcbScreen before 5.3.2 overrides them, see bug #335926, QTBUG-39648
    // TODO once we depend on Qt 5.4 remove it
    connect(qApp, SIGNAL(screenAdded(QScreen*)), this, SLOT(selectWmInputEventMask()));
#endif

#ifdef KWIN_BUILD_SCREENEDGES
    ScreenEdges::create(this);
#endif

    // VirtualDesktopManager needs to be created prior to init shortcuts
    // and prior to TabBox, due to TabBox connecting to signals
    // actual initialization happens in init()
    VirtualDesktopManager::create(this);

#ifdef KWIN_BUILD_TABBOX
    // need to create the tabbox before compositing scene is setup
    TabBox::TabBox::create(this);
#endif

    // init XRenderUtils
    XRenderUtils::init(connection(), rootWindow());
    m_compositor = Compositor::create(this);
    connect(this, SIGNAL(currentDesktopChanged(int,KWin::Client*)), m_compositor, SLOT(addRepaintFull()));
    connect(m_compositor, &Compositor::compositingToggled, decorationPlugin(), &DecorationPlugin::compositingToggled);

    new DBusInterface(this);

    // Compatibility
    int32_t data = 1;

    xcb_change_property(connection(), XCB_PROP_MODE_APPEND, rootWindow(), atoms->kwin_running,
                        atoms->kwin_running, 32, 1, &data);

    Outline::create(this);

    initShortcuts();

    init();
}

void Workspace::init()
{
    updateXTime(); // Needed for proper initialization of user_time in Client ctor
    KSharedConfigPtr config = KSharedConfig::openConfig();
    Screens *screens = Screens::self();
    screens->setConfig(config);
    screens->reconfigure();
    connect(options, SIGNAL(configChanged()), screens, SLOT(reconfigure()));
#ifdef KWIN_BUILD_SCREENEDGES
    ScreenEdges *screenEdges = ScreenEdges::self();
    screenEdges->setConfig(config);
    screenEdges->init();
    connect(options, SIGNAL(configChanged()), screenEdges, SLOT(reconfigure()));
    connect(VirtualDesktopManager::self(), SIGNAL(layoutChanged(int,int)), screenEdges, SLOT(updateLayout()));
    connect(this, SIGNAL(clientActivated(KWin::Client*)), screenEdges, SIGNAL(checkBlocking()));
#endif

    FocusChain *focusChain = FocusChain::create(this);
    connect(this, SIGNAL(clientRemoved(KWin::Client*)), focusChain, SLOT(remove(KWin::Client*)));
    connect(this, SIGNAL(clientActivated(KWin::Client*)), focusChain, SLOT(setActiveClient(KWin::Client*)));
    connect(VirtualDesktopManager::self(), SIGNAL(countChanged(uint,uint)), focusChain, SLOT(resize(uint,uint)));
    connect(VirtualDesktopManager::self(), SIGNAL(currentChanged(uint,uint)), focusChain, SLOT(setCurrentDesktop(uint,uint)));
    connect(options, SIGNAL(separateScreenFocusChanged(bool)), focusChain, SLOT(setSeparateScreenFocus(bool)));
    focusChain->setSeparateScreenFocus(options->isSeparateScreenFocus());

    const uint32_t nullFocusValues[] = {true};
    m_nullFocus.reset(new Xcb::Window(QRect(-1, -1, 1, 1), XCB_WINDOW_CLASS_INPUT_ONLY, XCB_CW_OVERRIDE_REDIRECT, nullFocusValues));
    m_nullFocus->map();

    RootInfo *rootInfo = RootInfo::create();

    // create VirtualDesktopManager and perform dependency injection
    VirtualDesktopManager *vds = VirtualDesktopManager::self();
    connect(vds, SIGNAL(desktopsRemoved(uint)), SLOT(moveClientsFromRemovedDesktops()));
    connect(vds, SIGNAL(countChanged(uint,uint)), SLOT(slotDesktopCountChanged(uint,uint)));
    connect(vds, SIGNAL(currentChanged(uint,uint)), SLOT(slotCurrentDesktopChanged(uint,uint)));
    vds->setNavigationWrappingAround(options->isRollOverDesktops());
    connect(options, SIGNAL(rollOverDesktopsChanged(bool)), vds, SLOT(setNavigationWrappingAround(bool)));
    vds->setRootInfo(rootInfo);
    vds->setConfig(config);

    // Now we know how many desktops we'll have, thus we initialize the positioning object
    Placement::create(this);

    // positioning object needs to be created before the virtual desktops are loaded.
    vds->load();
    vds->updateLayout();

    // Extra NETRootInfo instance in Client mode is needed to get the values of the properties
    NETRootInfo client_info(connection(), NET::ActiveWindow | NET::CurrentDesktop);
    int initial_desktop;
    if (!qApp->isSessionRestored())
        initial_desktop = client_info.currentDesktop();
    else {
#if KWIN_QT5_PORTING
        KConfigGroup group(kapp->sessionConfig(), "Session");
        initial_desktop = group.readEntry("desktop", 1);
#else
        initial_desktop = 1;
#endif
    }
    if (!VirtualDesktopManager::self()->setCurrent(initial_desktop))
        VirtualDesktopManager::self()->setCurrent(1);

    reconfigureTimer.setSingleShot(true);
    updateToolWindowsTimer.setSingleShot(true);

    connect(&reconfigureTimer, SIGNAL(timeout()), this, SLOT(slotReconfigure()));
    connect(&updateToolWindowsTimer, SIGNAL(timeout()), this, SLOT(slotUpdateToolWindows()));

    // TODO: do we really need to reconfigure everything when fonts change?
    // maybe just reconfigure the decorations? Move this into libkdecoration?
    QDBusConnection::sessionBus().connect(QString(),
                                          QStringLiteral("/KDEPlatformTheme"),
                                          QStringLiteral("org.kde.KDEPlatformTheme"),
                                          QStringLiteral("refreshFonts"),
                                          this, SLOT(reconfigure()));

    active_client = NULL;
    rootInfo->setActiveWindow(None);
    focusToNull();
    if (!qApp->isSessionRestored())
        ++block_focus; // Because it will be set below

    {
        // Begin updates blocker block
        StackingUpdatesBlocker blocker(this);

        Xcb::Tree tree(rootWindow());
        xcb_window_t *wins = xcb_query_tree_children(tree.data());

        QVector<Xcb::WindowAttributes> windowAttributes(tree->children_len);
        QVector<Xcb::WindowGeometry> windowGeometries(tree->children_len);

        // Request the attributes and geometries of all toplevel windows
        for (int i = 0; i < tree->children_len; i++) {
            windowAttributes[i] = Xcb::WindowAttributes(wins[i]);
            windowGeometries[i] = Xcb::WindowGeometry(wins[i]);
        }

        // Get the replies
        for (int i = 0; i < tree->children_len; i++) {
            Xcb::WindowAttributes attr(windowAttributes.at(i));

            if (attr.isNull()) {
                continue;
            }

            if (attr->override_redirect) {
                if (attr->map_state == XCB_MAP_STATE_VIEWABLE &&
                    attr->_class != XCB_WINDOW_CLASS_INPUT_ONLY)
                    // ### This will request the attributes again
                    createUnmanaged(wins[i]);
            } else if (attr->map_state != XCB_MAP_STATE_UNMAPPED) {
                if (Application::wasCrash()) {
                    fixPositionAfterCrash(wins[i], windowGeometries.at(i).data());
                }

                // ### This will request the attributes again
                createClient(wins[i], true);
            }
        }

        // Propagate clients, will really happen at the end of the updates blocker block
        updateStackingOrder(true);

        saveOldScreenSizes();
        updateClientArea();

        // NETWM spec says we have to set it to (0,0) if we don't support it
        NETPoint* viewports = new NETPoint[VirtualDesktopManager::self()->count()];
        rootInfo->setDesktopViewport(VirtualDesktopManager::self()->count(), *viewports);
        delete[] viewports;
        QRect geom;
        for (int i = 0; i < screens->count(); i++) {
            geom |= screens->geometry(i);
        }
        NETSize desktop_geometry;
        desktop_geometry.width = geom.width();
        desktop_geometry.height = geom.height();
        rootInfo->setDesktopGeometry(desktop_geometry);
        setShowingDesktop(false);

    } // End updates blocker block

    Client* new_active_client = NULL;
    if (!qApp->isSessionRestored()) {
        --block_focus;
        new_active_client = findClient(Predicate::WindowMatch, client_info.activeWindow());
    }
    if (new_active_client == NULL
            && activeClient() == NULL && should_get_focus.count() == 0) {
        // No client activated in manage()
        if (new_active_client == NULL)
            new_active_client = topClientOnDesktop(VirtualDesktopManager::self()->current(), -1);
        if (new_active_client == NULL && !desktops.isEmpty())
            new_active_client = findDesktop(true, VirtualDesktopManager::self()->current());
    }
    if (new_active_client != NULL)
        activateClient(new_active_client);

    Scripting::create(this);

    // SELI TODO: This won't work with unreasonable focus policies,
    // and maybe in rare cases also if the selected client doesn't
    // want focus
    workspaceInit = false;

    // broadcast that Workspace is ready, but first process all events.
    QMetaObject::invokeMethod(this, "workspaceInitialized", Qt::QueuedConnection);

    // TODO: ungrabXServer()
}

Workspace::~Workspace()
{
    delete m_compositor;
    m_compositor = NULL;
    blockStackingUpdates(true);

    // TODO: grabXServer();

    // Use stacking_order, so that kwin --replace keeps stacking order
    const ToplevelList stack = stacking_order;
    // "mutex" the stackingorder, since anything trying to access it from now on will find
    // many dangeling pointers and crash
    stacking_order.clear();

    for (ToplevelList::const_iterator it = stack.constBegin(), end = stack.constEnd(); it != end; ++it) {
        Client *c = qobject_cast<Client*>(const_cast<Toplevel*>(*it));
        if (!c) {
            continue;
        }
        // Only release the window
        c->releaseWindow(true);
        // No removeClient() is called, it does more than just removing.
        // However, remove from some lists to e.g. prevent performTransiencyCheck()
        // from crashing.
        clients.removeAll(c);
        desktops.removeAll(c);
    }
    for (UnmanagedList::iterator it = unmanaged.begin(), end = unmanaged.end(); it != end; ++it)
        (*it)->release(ReleaseReason::KWinShutsDown);
    xcb_delete_property(connection(), rootWindow(), atoms->kwin_running);

    delete RuleBook::self();
    KSharedConfig::openConfig()->sync();

    RootInfo::destroy();
    delete startup;
    delete Placement::self();
    delete client_keys_dialog;
    foreach (SessionInfo * s, session)
    delete s;

    // TODO: ungrabXServer();

    Xcb::Extensions::destroy();
    _self = 0;
}

Client* Workspace::createClient(xcb_window_t w, bool is_mapped)
{
    StackingUpdatesBlocker blocker(this);
    Client* c = new Client();
    connect(c, SIGNAL(needsRepaint()), m_compositor, SLOT(scheduleRepaint()));
    connect(c, SIGNAL(activeChanged()), m_compositor, SLOT(checkUnredirect()));
    connect(c, SIGNAL(fullScreenChanged()), m_compositor, SLOT(checkUnredirect()));
    connect(c, SIGNAL(geometryChanged()), m_compositor, SLOT(checkUnredirect()));
    connect(c, SIGNAL(geometryShapeChanged(KWin::Toplevel*,QRect)), m_compositor, SLOT(checkUnredirect()));
    connect(c, SIGNAL(blockingCompositingChanged(KWin::Client*)), m_compositor, SLOT(updateCompositeBlocking(KWin::Client*)));
#ifdef KWIN_BUILD_SCREENEDGES
    connect(c, SIGNAL(clientFullScreenSet(KWin::Client*,bool,bool)), ScreenEdges::self(), SIGNAL(checkBlocking()));
#endif
    connect(c, SIGNAL(desktopPresenceChanged(KWin::Client*,int)), SIGNAL(desktopPresenceChanged(KWin::Client*,int)), Qt::QueuedConnection);
    if (!c->manage(w, is_mapped)) {
        Client::deleteClient(c);
        return NULL;
    }
    addClient(c);
    return c;
}

Unmanaged* Workspace::createUnmanaged(xcb_window_t w)
{
    if (m_compositor && m_compositor->checkForOverlayWindow(w))
        return NULL;
    Unmanaged* c = new Unmanaged();
    if (!c->track(w)) {
        Unmanaged::deleteUnmanaged(c);
        return NULL;
    }
    connect(c, SIGNAL(needsRepaint()), m_compositor, SLOT(scheduleRepaint()));
    addUnmanaged(c);
    emit unmanagedAdded(c);
    return c;
}

void Workspace::addClient(Client* c)
{
    Group* grp = findGroup(c->window());

    KWindowInfo info(c->window(), NET::WMAllProperties, NET::WM2WindowClass);

    emit clientAdded(c);

    if (grp != NULL)
        grp->gotLeader(c);

    if (c->isDesktop()) {
        desktops.append(c);
        if (active_client == NULL && should_get_focus.isEmpty() && c->isOnCurrentDesktop())
            requestFocus(c);   // TODO: Make sure desktop is active after startup if there's no other window active
    } else {
        FocusChain::self()->update(c, FocusChain::Update);
        clients.append(c);
    }
    if (!unconstrained_stacking_order.contains(c))
        unconstrained_stacking_order.append(c);   // Raise if it hasn't got any stacking position yet
    if (!stacking_order.contains(c))    // It'll be updated later, and updateToolWindows() requires
        stacking_order.append(c);      // c to be in stacking_order
    x_stacking_dirty = true;
    updateClientArea(); // This cannot be in manage(), because the client got added only now
    updateClientLayer(c);
    if (c->isDesktop()) {
        raiseClient(c);
        // If there's no active client, make this desktop the active one
        if (activeClient() == NULL && should_get_focus.count() == 0)
            activateClient(findDesktop(true, VirtualDesktopManager::self()->current()));
    }
    c->checkActiveModal();
    checkTransients(c->window());   // SELI TODO: Does this really belong here?
    updateStackingOrder(true);   // Propagate new client
    if (c->isUtility() || c->isMenu() || c->isToolbar())
        updateToolWindows(true);
    checkNonExistentClients();
#ifdef KWIN_BUILD_TABBOX
    if (TabBox::TabBox::self()->isDisplayed())
        TabBox::TabBox::self()->reset(true);
#endif
#ifdef KWIN_BUILD_KAPPMENU
        if (ApplicationMenu::self()->hasMenu(c->window()))
            c->setAppMenuAvailable();
#endif
}

void Workspace::addUnmanaged(Unmanaged* c)
{
    unmanaged.append(c);
    x_stacking_dirty = true;
}

/**
 * Destroys the client \a c
 */
void Workspace::removeClient(Client* c)
{
    emit clientRemoved(c);

    if (c == active_popup_client)
        closeActivePopup();
    if (m_userActionsMenu->isMenuClient(c)) {
        m_userActionsMenu->close();
    }

    c->untab(QRect(), true);

    if (client_keys_client == c)
        setupWindowShortcutDone(false);
    if (!c->shortcut().isEmpty()) {
        c->setShortcut(QString());   // Remove from client_keys
        clientShortcutUpdated(c);   // Needed, since this is otherwise delayed by setShortcut() and wouldn't run
    }

#ifdef KWIN_BUILD_TABBOX
    TabBox::TabBox *tabBox = TabBox::TabBox::self();
    if (tabBox->isDisplayed() && tabBox->currentClient() == c)
        tabBox->nextPrev(true);
#endif

    Q_ASSERT(clients.contains(c) || desktops.contains(c));
    // TODO: if marked client is removed, notify the marked list
    clients.removeAll(c);
    desktops.removeAll(c);
    x_stacking_dirty = true;
    attention_chain.removeAll(c);
    showing_desktop_clients.removeAll(c);
    Group* group = findGroup(c->window());
    if (group != NULL)
        group->lostLeader();

    if (c == most_recently_raised)
        most_recently_raised = 0;
    should_get_focus.removeAll(c);
    Q_ASSERT(c != active_client);
    if (c == last_active_client)
        last_active_client = 0;
    if (c == delayfocus_client)
        cancelDelayFocus();

    updateStackingOrder(true);

#ifdef KWIN_BUILD_TABBOX
    if (tabBox->isDisplayed())
        tabBox->reset(true);
#endif

    updateClientArea();
}

void Workspace::removeUnmanaged(Unmanaged* c)
{
    assert(unmanaged.contains(c));
    unmanaged.removeAll(c);
    emit unmanagedRemoved(c);
    x_stacking_dirty = true;
}

void Workspace::addDeleted(Deleted* c, Toplevel *orig)
{
    assert(!deleted.contains(c));
    deleted.append(c);
    const int unconstraintedIndex = unconstrained_stacking_order.indexOf(orig);
    if (unconstraintedIndex != -1) {
        unconstrained_stacking_order.replace(unconstraintedIndex, c);
    } else {
        unconstrained_stacking_order.append(c);
    }
    const int index = stacking_order.indexOf(orig);
    if (index != -1) {
        stacking_order.replace(index, c);
    } else {
        stacking_order.append(c);
    }
    x_stacking_dirty = true;
    connect(c, SIGNAL(needsRepaint()), m_compositor, SLOT(scheduleRepaint()));
}

void Workspace::removeDeleted(Deleted* c)
{
    assert(deleted.contains(c));
    emit deletedRemoved(c);
    deleted.removeAll(c);
    unconstrained_stacking_order.removeAll(c);
    stacking_order.removeAll(c);
    x_stacking_dirty = true;
    if (c->wasClient() && m_compositor) {
        m_compositor->updateCompositeBlocking();
    }
}

void Workspace::updateToolWindows(bool also_hide)
{
    // TODO: What if Client's transiency/group changes? should this be called too? (I'm paranoid, am I not?)
    if (!options->isHideUtilityWindowsForInactive()) {
        for (ClientList::ConstIterator it = clients.constBegin(); it != clients.constEnd(); ++it)
            if (!(*it)->tabGroup() || (*it)->tabGroup()->current() == *it)
                (*it)->hideClient(false);
        return;
    }
    const Group* group = NULL;
    const Client* client = active_client;
    // Go up in transiency hiearchy, if the top is found, only tool transients for the top mainwindow
    // will be shown; if a group transient is group, all tools in the group will be shown
    while (client != NULL) {
        if (!client->isTransient())
            break;
        if (client->groupTransient()) {
            group = client->group();
            break;
        }
        client = client->transientFor();
    }
    // Use stacking order only to reduce flicker, it doesn't matter if block_stacking_updates == 0,
    // I.e. if it's not up to date

    // SELI TODO: But maybe it should - what if a new client has been added that's not in stacking order yet?
    ClientList to_show, to_hide;
    for (ToplevelList::ConstIterator it = stacking_order.constBegin();
            it != stacking_order.constEnd();
            ++it) {
        Client *c = qobject_cast<Client*>(*it);
        if (!c) {
            continue;
        }
        if (c->isUtility() || c->isMenu() || c->isToolbar()) {
            bool show = true;
            if (!c->isTransient()) {
                if (c->group()->members().count() == 1)   // Has its own group, keep always visible
                    show = true;
                else if (client != NULL && c->group() == client->group())
                    show = true;
                else
                    show = false;
            } else {
                if (group != NULL && c->group() == group)
                    show = true;
                else if (client != NULL && client->hasTransient(c, true))
                    show = true;
                else
                    show = false;
            }
            if (!show && also_hide) {
                const ClientList mainclients = c->mainClients();
                // Don't hide utility windows which are standalone(?) or
                // have e.g. kicker as mainwindow
                if (mainclients.isEmpty())
                    show = true;
                for (ClientList::ConstIterator it2 = mainclients.constBegin();
                        it2 != mainclients.constEnd();
                        ++it2) {
                    if ((*it2)->isSpecialWindow())
                        show = true;
                }
                if (!show)
                    to_hide.append(c);
            }
            if (show)
                to_show.append(c);
        }
    } // First show new ones, then hide
    for (int i = to_show.size() - 1;
            i >= 0;
            --i)  // From topmost
        // TODO: Since this is in stacking order, the order of taskbar entries changes :(
        to_show.at(i)->hideClient(false);
    if (also_hide) {
        for (ClientList::ConstIterator it = to_hide.constBegin();
                it != to_hide.constEnd();
                ++it)  // From bottommost
            (*it)->hideClient(true);
        updateToolWindowsTimer.stop();
    } else // setActiveClient() is after called with NULL client, quickly followed
        // by setting a new client, which would result in flickering
        resetUpdateToolWindowsTimer();
}


void Workspace::resetUpdateToolWindowsTimer()
{
    updateToolWindowsTimer.start(200);
}

void Workspace::slotUpdateToolWindows()
{
    updateToolWindows(true);
}

void Workspace::slotReloadConfig()
{
    reconfigure();
}

void Workspace::reconfigure()
{
    reconfigureTimer.start(200);
}

/**
 * This D-Bus call is used by the compositing kcm. Since the reconfigure()
 * D-Bus call delays the actual reconfiguring, it is not possible to immediately
 * call compositingActive(). Therefore the kcm will instead call this to ensure
 * the reconfiguring has already happened.
 */
bool Workspace::waitForCompositingSetup()
{
    if (reconfigureTimer.isActive()) {
        reconfigureTimer.stop();
        slotReconfigure();
    }
    if (m_compositor) {
        return m_compositor->isActive();
    }
    return false;
}

/**
 * Reread settings
 */

void Workspace::slotReconfigure()
{
    qDebug() << "Workspace::slotReconfigure()";
    reconfigureTimer.stop();

    bool borderlessMaximizedWindows = options->borderlessMaximizedWindows();

    KSharedConfig::openConfig()->reparseConfiguration();
    options->updateSettings();

    emit configChanged();
    m_userActionsMenu->discard();
    updateToolWindows(true);

    DecorationPlugin *deco = DecorationPlugin::self();
    if (!deco->isDisabled() && deco->reset()) {
        deco->recreateDecorations();
        deco->destroyPreviousPlugin();
        connect(deco->factory(), &KDecorationFactory::recreateDecorations, deco, &DecorationPlugin::recreateDecorations);
    } else {
        foreach (Client * c, clients) {
            c->checkBorderSizes(true);
            c->triggerDecorationRepaint();
        }
    }

    RuleBook::self()->load();
    for (ClientList::Iterator it = clients.begin();
            it != clients.end();
            ++it) {
        (*it)->setupWindowRules(true);
        (*it)->applyWindowRules();
        RuleBook::self()->discardUsed(*it, false);
    }

    if (borderlessMaximizedWindows != options->borderlessMaximizedWindows() &&
            !options->borderlessMaximizedWindows()) {
        // in case borderless maximized windows option changed and new option
        // is to have borders, we need to unset the borders for all maximized windows
        for (ClientList::Iterator it = clients.begin();
                it != clients.end();
                ++it) {
            if ((*it)->maximizeMode() == MaximizeFull)
                (*it)->checkNoBorder();
        }
    }

    if (!deco->isDisabled()) {
        rootInfo()->setSupported(NET::WM2FrameOverlap, deco->factory()->supports(AbilityExtendIntoClientArea));
    } else {
        rootInfo()->setSupported(NET::WM2FrameOverlap, false);
    }
}

/**
 * During virt. desktop switching, desktop areas covered by windows that are
 * going to be hidden are first obscured by new windows with no background
 * ( i.e. transparent ) placed right below the windows. These invisible windows
 * are removed after the switch is complete.
 * Reduces desktop ( wallpaper ) repaints during desktop switching
 */
class ObscuringWindows
{
public:
    ~ObscuringWindows();
    void create(Client* c);
private:
    QList<xcb_window_t> obscuring_windows;
    static QList<xcb_window_t>* cached;
    static unsigned int max_cache_size;
};

QList<xcb_window_t>* ObscuringWindows::cached = nullptr;
unsigned int ObscuringWindows::max_cache_size = 0;

void ObscuringWindows::create(Client* c)
{
    if (!cached)
        cached = new QList<xcb_window_t>;
    Xcb::Window obs_win(XCB_WINDOW_NONE, false);
    if (cached->count() > 0) {
        obs_win.reset(cached->first(), false);
        cached->removeAll(obs_win);
        obs_win.setGeometry(c->geometry());
    } else {
        uint32_t values[] = {XCB_PIXMAP_NONE, true};
        obs_win.create(c->geometry(), XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_CW_BACK_PIXMAP | XCB_CW_OVERRIDE_REDIRECT, values);
    }
    uint32_t values[] = {c->frameId(), XCB_STACK_MODE_BELOW};
    xcb_configure_window(connection(), obs_win, XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE, values);
    obs_win.map();
    obscuring_windows.append(obs_win);
}

ObscuringWindows::~ObscuringWindows()
{
    max_cache_size = qMax(int(max_cache_size), obscuring_windows.count() + 4) - 1;
    for (auto it = obscuring_windows.constBegin();
            it != obscuring_windows.constEnd();
            ++it) {
        xcb_unmap_window(connection(), *it);
        if (cached->count() < int(max_cache_size))
            cached->prepend(*it);
        else
            xcb_destroy_window(connection(), *it);
    }
}

void Workspace::slotCurrentDesktopChanged(uint oldDesktop, uint newDesktop)
{
    closeActivePopup();
    ++block_focus;
    StackingUpdatesBlocker blocker(this);
    updateClientVisibilityOnDesktopChange(oldDesktop, newDesktop);
    // Restore the focus on this desktop
    --block_focus;

    activateClientOnNewDesktop(newDesktop);
    emit currentDesktopChanged(oldDesktop, movingClient);
}

void Workspace::updateClientVisibilityOnDesktopChange(uint oldDesktop, uint newDesktop)
{
    ++block_showing_desktop;
    ObscuringWindows obs_wins;
    for (ToplevelList::ConstIterator it = stacking_order.constBegin();
            it != stacking_order.constEnd();
            ++it) {
        Client *c = qobject_cast<Client*>(*it);
        if (!c) {
            continue;
        }
        if (!c->isOnDesktop(newDesktop) && c != movingClient && c->isOnCurrentActivity()) {
            if (c->isShown(true) && c->isOnDesktop(oldDesktop) && !compositing())
                obs_wins.create(c);
            (c)->updateVisibility();
        }
    }
    // Now propagate the change, after hiding, before showing
    rootInfo()->setCurrentDesktop(VirtualDesktopManager::self()->current());

    if (movingClient && !movingClient->isOnDesktop(newDesktop)) {
        movingClient->setDesktop(newDesktop);
    }

    for (int i = stacking_order.size() - 1; i >= 0 ; --i) {
        Client *c = qobject_cast<Client*>(stacking_order.at(i));
        if (!c) {
            continue;
        }
        if (c->isOnDesktop(newDesktop) && c->isOnCurrentActivity())
            c->updateVisibility();
    }
    --block_showing_desktop;
    if (showingDesktop())   // Do this only after desktop change to avoid flicker
        resetShowingDesktop(false);
}

void Workspace::activateClientOnNewDesktop(uint desktop)
{
    Client* c = NULL;
    if (options->focusPolicyIsReasonable()) {
        c = findClientToActivateOnDesktop(desktop);
    }
    // If "unreasonable focus policy" and active_client is on_all_desktops and
    // under mouse (Hence == old_active_client), conserve focus.
    // (Thanks to Volker Schatz <V.Schatz at thphys.uni-heidelberg.de>)
    else if (active_client && active_client->isShown(true) && active_client->isOnCurrentDesktop())
        c = active_client;

    if (c == NULL && !desktops.isEmpty())
        c = findDesktop(true, desktop);

    if (c != active_client)
        setActiveClient(NULL);

    if (c)
        requestFocus(c);
    else if (!desktops.isEmpty())
        requestFocus(findDesktop(true, desktop));
    else
        focusToNull();
}

Client *Workspace::findClientToActivateOnDesktop(uint desktop)
{
    if (movingClient != NULL && active_client == movingClient &&
        FocusChain::self()->contains(active_client, desktop) &&
        active_client->isShown(true) && active_client->isOnCurrentDesktop()) {
        // A requestFocus call will fail, as the client is already active
        return active_client;
    }
    // from actiavtion.cpp
    if (options->isNextFocusPrefersMouse()) {
        ToplevelList::const_iterator it = stackingOrder().constEnd();
        while (it != stackingOrder().constBegin()) {
            Client *client = qobject_cast<Client*>(*(--it));
            if (!client) {
                continue;
            }

            if (!(client->isShown(false) && client->isOnDesktop(desktop) &&
                client->isOnCurrentActivity() && client->isOnActiveScreen()))
                continue;

            if (client->geometry().contains(Cursor::pos())) {
                if (!client->isDesktop())
                    return client;
            break; // unconditional break  - we do not pass the focus to some client below an unusable one
            }
        }
    }
    return FocusChain::self()->getForActivation(desktop);
}

/**
 * Updates the current activity when it changes
 * do *not* call this directly; it does not set the activity.
 *
 * Shows/Hides windows according to the stacking order
 */

void Workspace::updateCurrentActivity(const QString &new_activity)
{
#ifdef KWIN_BUILD_ACTIVITIES
    //closeActivePopup();
    ++block_focus;
    // TODO: Q_ASSERT( block_stacking_updates == 0 ); // Make sure stacking_order is up to date
    StackingUpdatesBlocker blocker(this);

    ++block_showing_desktop; //FIXME should I be using that?
    // Optimized Desktop switching: unmapping done from back to front
    // mapping done from front to back => less exposure events
    //Notify::raise((Notify::Event) (Notify::DesktopChange+new_desktop));

    ObscuringWindows obs_wins;

    const QString &old_activity = Activities::self()->previous();

    for (ToplevelList::ConstIterator it = stacking_order.constBegin();
            it != stacking_order.constEnd();
            ++it) {
        Client *c = qobject_cast<Client*>(*it);
        if (!c) {
            continue;
        }
        if (!c->isOnActivity(new_activity) && c != movingClient && c->isOnCurrentDesktop()) {
            if (c->isShown(true) && c->isOnActivity(old_activity) && !compositing())
                obs_wins.create(c);
            c->updateVisibility();
        }
    }

    // Now propagate the change, after hiding, before showing
    //rootInfo->setCurrentDesktop( currentDesktop() );

    /* TODO someday enable dragging windows to other activities
    if ( movingClient && !movingClient->isOnDesktop( new_desktop ))
        {
        movingClient->setDesktop( new_desktop );
        */

    for (int i = stacking_order.size() - 1; i >= 0 ; --i) {
        Client *c = qobject_cast<Client*>(stacking_order.at(i));
        if (!c) {
            continue;
        }
        if (c->isOnActivity(new_activity))
            c->updateVisibility();
    }

    --block_showing_desktop;
    //FIXME not sure if I should do this either
    if (showingDesktop())   // Do this only after desktop change to avoid flicker
        resetShowingDesktop(false);

    // Restore the focus on this desktop
    --block_focus;
    Client* c = 0;

    //FIXME below here is a lot of focuschain stuff, probably all wrong now
    if (options->focusPolicyIsReasonable()) {
        // Search in focus chain
        c = FocusChain::self()->getForActivation(VirtualDesktopManager::self()->current());
    }
    // If "unreasonable focus policy" and active_client is on_all_desktops and
    // under mouse (Hence == old_active_client), conserve focus.
    // (Thanks to Volker Schatz <V.Schatz at thphys.uni-heidelberg.de>)
    else if (active_client && active_client->isShown(true) && active_client->isOnCurrentDesktop() && active_client->isOnCurrentActivity())
        c = active_client;

    if (c == NULL && !desktops.isEmpty())
        c = findDesktop(true, VirtualDesktopManager::self()->current());

    if (c != active_client)
        setActiveClient(NULL);

    if (c)
        requestFocus(c);
    else if (!desktops.isEmpty())
        requestFocus(findDesktop(true, VirtualDesktopManager::self()->current()));
    else
        focusToNull();

    // Not for the very first time, only if something changed and there are more than 1 desktops

    //if ( effects != NULL && old_desktop != 0 && old_desktop != new_desktop )
    //    static_cast<EffectsHandlerImpl*>( effects )->desktopChanged( old_desktop );
    if (compositing() && m_compositor)
        m_compositor->addRepaintFull();
#else
    Q_UNUSED(new_activity)
#endif
}

void Workspace::moveClientsFromRemovedDesktops()
{
    for (ClientList::ConstIterator it = clients.constBegin(); it != clients.constEnd(); ++it) {
        if (!(*it)->isOnAllDesktops() && (*it)->desktop() > static_cast<int>(VirtualDesktopManager::self()->count()))
            sendClientToDesktop(*it, VirtualDesktopManager::self()->count(), true);
    }
}

void Workspace::slotDesktopCountChanged(uint previousCount, uint newCount)
{
    Q_UNUSED(previousCount)
    Placement::self()->reinitCascading(0);

    resetClientAreas(newCount);
}

void Workspace::resetClientAreas(uint desktopCount)
{
    // Make it +1, so that it can be accessed as [1..numberofdesktops]
    workarea.clear();
    workarea.resize(desktopCount + 1);
    restrictedmovearea.clear();
    restrictedmovearea.resize(desktopCount + 1);
    screenarea.clear();

    updateClientArea(true);
}

void Workspace::selectWmInputEventMask()
{
    uint32_t presentMask = 0;
    Xcb::WindowAttributes attr(rootWindow());
    if (!attr.isNull()) {
        presentMask = attr->your_event_mask;
    }

    Xcb::selectInput(rootWindow(),
                     presentMask |
                     XCB_EVENT_MASK_KEY_PRESS |
                     XCB_EVENT_MASK_PROPERTY_CHANGE |
                     XCB_EVENT_MASK_COLOR_MAP_CHANGE |
                     XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
                     XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
                     XCB_EVENT_MASK_FOCUS_CHANGE | // For NotifyDetailNone
                     XCB_EVENT_MASK_EXPOSURE
    );
}

/**
 * Sends client \a c to desktop \a desk.
 *
 * Takes care of transients as well.
 */
void Workspace::sendClientToDesktop(Client* c, int desk, bool dont_activate)
{
    if ((desk < 1 && desk != NET::OnAllDesktops) || desk > static_cast<int>(VirtualDesktopManager::self()->count()))
        return;
    int old_desktop = c->desktop();
    bool was_on_desktop = c->isOnDesktop(desk) || c->isOnAllDesktops();
    c->setDesktop(desk);
    if (c->desktop() != desk)   // No change or desktop forced
        return;
    desk = c->desktop(); // Client did range checking

    if (c->isOnDesktop(VirtualDesktopManager::self()->current())) {
        if (c->wantsTabFocus() && options->focusPolicyIsReasonable() &&
                !was_on_desktop && // for stickyness changes
                !dont_activate)
            requestFocus(c);
        else
            restackClientUnderActive(c);
    } else
        raiseClient(c);

    c->checkWorkspacePosition( QRect(), old_desktop );

    ClientList transients_stacking_order = ensureStackingOrder(c->transients());
    for (ClientList::ConstIterator it = transients_stacking_order.constBegin();
            it != transients_stacking_order.constEnd();
            ++it)
        sendClientToDesktop(*it, desk, dont_activate);
    updateClientArea();
}

/**
 * checks whether the X Window with the input focus is on our X11 screen
 * if the window cannot be determined or inspected, resturn depends on whether there's actually
 * more than one screen
 *
 * this is NOT in any way related to XRandR multiscreen
 *
 */
extern bool is_multihead; // main.cpp
bool Workspace::isOnCurrentHead()
{
    if (!is_multihead) {
        return true;
    }

    Xcb::CurrentInput currentInput;
    if (currentInput.window() == XCB_WINDOW_NONE) {
        return !is_multihead;
    }

    Xcb::WindowGeometry geometry(currentInput.window());
    if (geometry.isNull()) { // should not happen
        return !is_multihead;
    }

    return rootWindow() == geometry->root;
}

void Workspace::sendClientToScreen(Client* c, int screen)
{
    c->sendToScreen(screen);
}

void Workspace::sendPingToWindow(xcb_window_t window, xcb_timestamp_t timestamp)
{
    rootInfo()->sendPing(window, timestamp);
}

/**
 * Delayed focus functions
 */
void Workspace::delayFocus()
{
    requestFocus(delayfocus_client);
    cancelDelayFocus();
}

void Workspace::requestDelayFocus(Client* c)
{
    delayfocus_client = c;
    delete delayFocusTimer;
    delayFocusTimer = new QTimer(this);
    connect(delayFocusTimer, SIGNAL(timeout()), this, SLOT(delayFocus()));
    delayFocusTimer->setSingleShot(true);
    delayFocusTimer->start(options->delayFocusInterval());
}

void Workspace::cancelDelayFocus()
{
    delete delayFocusTimer;
    delayFocusTimer = 0;
}

bool Workspace::checkStartupNotification(xcb_window_t w, KStartupInfoId &id, KStartupInfoData &data)
{
    return startup->checkStartup(w, id, data) == KStartupInfo::Match;
}

/**
 * Puts the focus on a dummy window
 * Just using XSetInputFocus() with None would block keyboard input
 */
void Workspace::focusToNull()
{
    m_nullFocus->focus();
}

void Workspace::setShowingDesktop(bool showing)
{
    rootInfo()->setShowingDesktop(showing);
    showing_desktop = showing;
    ++block_showing_desktop;
    if (showing_desktop) {
        showing_desktop_clients.clear();
        ++block_focus;
        ToplevelList cls = stackingOrder();
        // Find them first, then minimize, otherwise transients may get minimized with the window
        // they're transient for
        for (ToplevelList::ConstIterator it = cls.constBegin();
                it != cls.constEnd();
                ++it) {
            Client *c = qobject_cast<Client*>(*it);
            if (!c) {
                continue;
            }
            if (c->isOnCurrentActivity() && c->isOnCurrentDesktop() && c->isShown(true) && !c->isSpecialWindow())
                showing_desktop_clients.prepend(c);   // Topmost first to reduce flicker
        }
        for (ClientList::ConstIterator it = showing_desktop_clients.constBegin();
                it != showing_desktop_clients.constEnd();
                ++it)
            (*it)->minimize();
        --block_focus;
        if (Client* desk = findDesktop(true, VirtualDesktopManager::self()->current()))
            requestFocus(desk);
    } else {
        for (ClientList::ConstIterator it = showing_desktop_clients.constBegin();
                it != showing_desktop_clients.constEnd();
                ++it)
            (*it)->unminimize();
        if (showing_desktop_clients.count() > 0)
            requestFocus(showing_desktop_clients.first());
        showing_desktop_clients.clear();
    }
    --block_showing_desktop;
}

/**
 * Following Kicker's behavior:
 * Changing a virtual desktop resets the state and shows the windows again.
 * Unminimizing a window resets the state but keeps the windows hidden (except
 * the one that was unminimized).
 * A new window resets the state and shows the windows again, with the new window
 * being active. Due to popular demand (#67406) by people who apparently
 * don't see a difference between "show desktop" and "minimize all", this is not
 * true if "showDesktopIsMinimizeAll" is set in kwinrc. In such case showing
 * a new window resets the state but doesn't show windows.
 */
void Workspace::resetShowingDesktop(bool keep_hidden)
{
    if (block_showing_desktop > 0)
        return;
    rootInfo()->setShowingDesktop(false);
    showing_desktop = false;
    ++block_showing_desktop;
    if (!keep_hidden) {
        for (ClientList::ConstIterator it = showing_desktop_clients.constBegin();
                it != showing_desktop_clients.constEnd();
                ++it)
            (*it)->unminimize();
    }
    showing_desktop_clients.clear();
    --block_showing_desktop;
}

void Workspace::disableGlobalShortcutsForClient(bool disable)
{
    if (global_shortcuts_disabled_for_client == disable)
        return;
    QDBusMessage message = QDBusMessage::createMethodCall(QStringLiteral("org.kde.kglobalaccel"),
                                                          QStringLiteral("/kglobalaccel"),
                                                          QStringLiteral("org.kde.KGlobalAccel"),
                                                          QStringLiteral("blockGlobalShortcuts"));
    message.setArguments(QList<QVariant>() << disable);
    QDBusConnection::sessionBus().asyncCall(message);

    global_shortcuts_disabled_for_client = disable;
    // Update also Alt+LMB actions etc.
    for (ClientList::ConstIterator it = clients.constBegin();
            it != clients.constEnd();
            ++it)
        (*it)->updateMouseGrab();
}

QString Workspace::supportInformation() const
{
    QString support;

    support.append(ki18nc("Introductory text shown in the support information.",
        "KWin Support Information:\n"
        "The following information should be used when requesting support on e.g. http://forum.kde.org.\n"
        "It provides information about the currently running instance, which options are used,\n"
        "what OpenGL driver and which effects are running.\n"
        "Please post the information provided underneath this introductory text to a paste bin service\n"
        "like http://paste.kde.org instead of pasting into support threads.\n").toString());
    support.append(QStringLiteral("\n==========================\n\n"));
    // all following strings are intended for support. They need to be pasted to e.g forums.kde.org
    // it is expected that the support will happen in English language or that the people providing
    // help understand English. Because of that all texts are not translated
    support.append(QStringLiteral("Version\n"));
    support.append(QStringLiteral("=======\n"));
    support.append(QStringLiteral("KWin version: "));
    support.append(QStringLiteral(KWIN_VERSION_STRING));
    support.append(QStringLiteral("\n"));
    support.append(QStringLiteral("Qt Version: "));
    support.append(QString::fromUtf8(qVersion()));
    support.append(QStringLiteral("\n\n"));
    support.append(QStringLiteral("Operation Mode: "));
    switch (kwinApp()->operationMode()) {
    case Application::OperationModeX11:
        support.append(QStringLiteral("X11 only"));
        break;
    case Application::OperationModeWaylandAndX11:
        support.append(QStringLiteral("Wayland and X11"));
        break;
    }
    support.append(QStringLiteral("\n\n"));
    support.append(QStringLiteral("Options\n"));
    support.append(QStringLiteral("=======\n"));
    const QMetaObject *metaOptions = options->metaObject();
    for (int i=0; i<metaOptions->propertyCount(); ++i) {
        const QMetaProperty property = metaOptions->property(i);
        if (QLatin1String(property.name()) == QLatin1String("objectName")) {
            continue;
        }
        support.append(QLatin1String(property.name()) + QStringLiteral(": ") + options->property(property.name()).toString() + QStringLiteral("\n"));
    }
#ifdef KWIN_BUILD_SCREENEDGES
    support.append(QStringLiteral("\nScreen Edges\n"));
    support.append(QStringLiteral(  "============\n"));
    const QMetaObject *metaScreenEdges = ScreenEdges::self()->metaObject();
    for (int i=0; i<metaScreenEdges->propertyCount(); ++i) {
        const QMetaProperty property = metaScreenEdges->property(i);
        if (QLatin1String(property.name()) == QLatin1String("objectName")) {
            continue;
        }
        support.append(QLatin1String(property.name()) + QStringLiteral(": ") + ScreenEdges::self()->property(property.name()).toString() + QStringLiteral("\n"));
    }
#endif
    support.append(QStringLiteral("\nScreens\n"));
    support.append(QStringLiteral(  "=======\n"));
    support.append(QStringLiteral("Multi-Head: "));
    if (is_multihead) {
        support.append(QStringLiteral("yes\n"));
        support.append(QStringLiteral("Head: %1\n").arg(screen_number));
    } else {
        support.append(QStringLiteral("no\n"));
    }
    support.append(QStringLiteral("Active screen follows mouse: "));
    if (screens()->isCurrentFollowsMouse())
        support.append(QStringLiteral(" yes\n"));
    else
        support.append(QStringLiteral(" no\n"));
    support.append(QStringLiteral("Number of Screens: %1\n").arg(screens()->count()));
    for (int i=0; i<screens()->count(); ++i) {
        const QRect geo = screens()->geometry(i);
        support.append(QStringLiteral("Screen %1 Geometry: %2,%3,%4x%5\n")
                              .arg(i)
                              .arg(geo.x())
                              .arg(geo.y())
                              .arg(geo.width())
                              .arg(geo.height()));
    }
    support.append(QStringLiteral("\nDecoration\n"));
    support.append(QStringLiteral(  "==========\n"));
    support.append(decorationPlugin()->supportInformation());
    support.append(QStringLiteral("\nCompositing\n"));
    support.append(QStringLiteral(  "===========\n"));
    if (effects) {
        support.append(QStringLiteral("Compositing is active\n"));
        switch (effects->compositingType()) {
        case OpenGL2Compositing:
        case OpenGLCompositing: {
#ifdef KWIN_HAVE_OPENGLES
            support.append(QStringLiteral("Compositing Type: OpenGL ES 2.0\n"));
#else
            support.append(QStringLiteral("Compositing Type: OpenGL\n"));
#endif

            GLPlatform *platform = GLPlatform::instance();
            support.append(QStringLiteral("OpenGL vendor string: ") +   QString::fromUtf8(platform->glVendorString()) + QStringLiteral("\n"));
            support.append(QStringLiteral("OpenGL renderer string: ") + QString::fromUtf8(platform->glRendererString()) + QStringLiteral("\n"));
            support.append(QStringLiteral("OpenGL version string: ") +  QString::fromUtf8(platform->glVersionString()) + QStringLiteral("\n"));
            support.append(QStringLiteral("OpenGL platform interface: "));
            switch (platform->platformInterface()) {
            case GlxPlatformInterface:
                support.append(QStringLiteral("GLX"));
                break;
            case EglPlatformInterface:
                support.append(QStringLiteral("EGL"));
                break;
            default:
                support.append(QStringLiteral("UNKNOWN"));
            }
            support.append(QStringLiteral("\n"));

            if (platform->supports(LimitedGLSL) || platform->supports(GLSL))
                support.append(QStringLiteral("OpenGL shading language version string: ") + QString::fromUtf8(platform->glShadingLanguageVersionString()) + QStringLiteral("\n"));

            support.append(QStringLiteral("Driver: ") + GLPlatform::driverToString(platform->driver()) + QStringLiteral("\n"));
            if (!platform->isMesaDriver())
                support.append(QStringLiteral("Driver version: ") + GLPlatform::versionToString(platform->driverVersion()) + QStringLiteral("\n"));

            support.append(QStringLiteral("GPU class: ") + GLPlatform::chipClassToString(platform->chipClass()) + QStringLiteral("\n"));

            support.append(QStringLiteral("OpenGL version: ") + GLPlatform::versionToString(platform->glVersion()) + QStringLiteral("\n"));

            if (platform->supports(LimitedGLSL) || platform->supports(GLSL))
                support.append(QStringLiteral("GLSL version: ") + GLPlatform::versionToString(platform->glslVersion()) + QStringLiteral("\n"));

            if (platform->isMesaDriver())
                support.append(QStringLiteral("Mesa version: ") + GLPlatform::versionToString(platform->mesaVersion()) + QStringLiteral("\n"));
            if (platform->serverVersion() > 0)
                support.append(QStringLiteral("X server version: ") + GLPlatform::versionToString(platform->serverVersion()) + QStringLiteral("\n"));
            if (platform->kernelVersion() > 0)
                support.append(QStringLiteral("Linux kernel version: ") + GLPlatform::versionToString(platform->kernelVersion()) + QStringLiteral("\n"));

            support.append(QStringLiteral("Direct rendering: "));
            support.append(QStringLiteral("Requires strict binding: "));
            if (!platform->isLooseBinding()) {
                support.append(QStringLiteral("yes\n"));
            } else {
                support.append(QStringLiteral("no\n"));
            }
            support.append(QStringLiteral("GLSL shaders: "));
            if (platform->supports(GLSL)) {
                if (platform->supports(LimitedGLSL)) {
                    support.append(QStringLiteral(" limited\n"));
                } else {
                    support.append(QStringLiteral(" yes\n"));
                }
            } else {
                support.append(QStringLiteral(" no\n"));
            }
            support.append(QStringLiteral("Texture NPOT support: "));
            if (platform->supports(TextureNPOT)) {
                if (platform->supports(LimitedNPOT)) {
                    support.append(QStringLiteral(" limited\n"));
                } else {
                    support.append(QStringLiteral(" yes\n"));
                }
            } else {
                support.append(QStringLiteral(" no\n"));
            }
            support.append(QStringLiteral("Virtual Machine: "));
            if (platform->isVirtualMachine()) {
                support.append(QStringLiteral(" yes\n"));
            } else {
                support.append(QStringLiteral(" no\n"));
            }

            support.append(QStringLiteral("OpenGL 2 Shaders are used\n"));
            support.append(QStringLiteral("Painting blocks for vertical retrace: "));
            if (m_compositor->scene()->blocksForRetrace())
                support.append(QStringLiteral(" yes\n"));
            else
                support.append(QStringLiteral(" no\n"));
            break;
        }
        case XRenderCompositing:
            support.append(QStringLiteral("Compositing Type: XRender\n"));
            break;
        case QPainterCompositing:
            support.append("Compositing Type: QPainter\n");
            break;
        case NoCompositing:
        default:
            support.append(QStringLiteral("Something is really broken, neither OpenGL nor XRender is used"));
        }
        support.append(QStringLiteral("\nLoaded Effects:\n"));
        support.append(QStringLiteral(  "---------------\n"));
        foreach (const QString &effect, static_cast<EffectsHandlerImpl*>(effects)->loadedEffects()) {
            support.append(effect + QStringLiteral("\n"));
        }
        support.append(QStringLiteral("\nCurrently Active Effects:\n"));
        support.append(QStringLiteral(  "-------------------------\n"));
        foreach (const QString &effect, static_cast<EffectsHandlerImpl*>(effects)->activeEffects()) {
            support.append(effect + QStringLiteral("\n"));
        }
        support.append(QStringLiteral("\nEffect Settings:\n"));
        support.append(QStringLiteral(  "----------------\n"));
        foreach (const QString &effect, static_cast<EffectsHandlerImpl*>(effects)->loadedEffects()) {
            support.append(static_cast<EffectsHandlerImpl*>(effects)->supportInformation(effect));
            support.append(QStringLiteral("\n"));
        }
    } else {
        support.append(QStringLiteral("Compositing is not active\n"));
    }
    return support;
}

void Workspace::slotToggleCompositing()
{
    if (m_compositor) {
        m_compositor->slotToggleCompositing();
    }
}

Client *Workspace::findClient(std::function<bool (const Client*)> func) const
{
    if (Client *ret = Toplevel::findInList(clients, func)) {
        return ret;
    }
    if (Client *ret = Toplevel::findInList(desktops, func)) {
        return ret;
    }
    return nullptr;
}

Unmanaged *Workspace::findUnmanaged(std::function<bool (const Unmanaged*)> func) const
{
    return Toplevel::findInList(unmanaged, func);
}

Unmanaged *Workspace::findUnmanaged(xcb_window_t w) const
{
    return findUnmanaged([w](const Unmanaged *u) {
        return u->window() == w;
    });
}

Client *Workspace::findClient(Predicate predicate, xcb_window_t w) const
{
    switch (predicate) {
    case Predicate::WindowMatch:
        return findClient([w](const Client *c) {
            return c->window() == w;
        });
    case Predicate::WrapperIdMatch:
        return findClient([w](const Client *c) {
            return c->wrapperId() == w;
        });
    case Predicate::FrameIdMatch:
        return findClient([w](const Client *c) {
            return c->frameId() == w;
        });
    case Predicate::InputIdMatch:
        return findClient([w](const Client *c) {
            return c->inputId() == w;
        });
    }
    return nullptr;
}

} // namespace

#include "workspace.moc"
