/********************************************************************
 KWin - the KDE window manager
 This file is part of the KDE project.

Copyright (C) 1999, 2000 Matthias Ettrich <ettrich@kde.org>
Copyright (C) 2003 Lubos Lunak <l.lunak@kde.org>
Copyright (C) 2009 Lucas Murray <lmurray@undefinedfire.com>

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

#ifndef KWIN_WORKSPACE_H
#define KWIN_WORKSPACE_H

// kwin
#include <kdecoration.h>
#include "sm.h"
#include "utils.h"
// Qt
#include <QTimer>
#include <QVector>
// std
#include <functional>

// TODO: Cleanup the order of things in this .h file

class QStringList;
class KConfig;
class KConfigGroup;
class KStartupInfo;
class KStartupInfoId;
class KStartupInfoData;

namespace KWin
{

namespace Xcb
{
class Window;
}

class Client;
class KillWindow;
class ShortcutDialog;
class UserActionsMenu;
class Compositor;
enum class Predicate;

class Workspace : public QObject, public KDecorationDefines
{
    Q_OBJECT
public:
    explicit Workspace(bool restore = false);
    virtual ~Workspace();

    static Workspace* self() {
        return _self;
    }

    bool workspaceEvent(xcb_generic_event_t*);
    bool workspaceEvent(QEvent*);

    bool hasClient(const Client*);

    /**
     * @brief Finds the first Client matching the condition expressed by passed in @p func.
     *
     * Internally findClient uses the std::find_if algorithm and that determines how the function
     * needs to be implemented. An example usage for finding a Client with a matching windowId
     * @code
     * xcb_window_t w; // our test window
     * Client *client = findClient([w](const Client *c) -> bool {
     *     return c->window() == w;
     * });
     * @endcode
     *
     * For the standard cases of matching the window id with one of the Client's windows use
     * the simplified overload method findClient(Predicate, xcb_window_t). Above example
     * can be simplified to:
     * @code
     * xcb_window_t w; // our test window
     * Client *client = findClient(Predicate::WindowMatch, w);
     * @endcode
     *
     * @param func Unary function that accepts a Client* as argument and
     * returns a value convertible to bool. The value returned indicates whether the
     * Client* is considered a match in the context of this function.
     * The function shall not modify its argument.
     * This can either be a function pointer or a function object.
     * @return KWin::Client* The found Client or @c null
     * @see findClient(Predicate, xcb_window_t)
     */
    Client *findClient(std::function<bool (const Client*)> func) const;
    /**
     * @brief Finds the Client matching the given match @p predicate for the given window.
     *
     * @param predicate Which window should be compared
     * @param w The window id to test against
     * @return KWin::Client* The found Client or @c null
     * @see findClient(std::function<bool (const Client*)>)
     */
    Client *findClient(Predicate predicate, xcb_window_t w) const;
    void forEachClient(std::function<void (Client*)> func);
    Unmanaged *findUnmanaged(std::function<bool (const Unmanaged*)> func) const;
    /**
     * @brief Finds the Unmanaged with the given window id.
     *
     * @param w The window id to search for
     * @return KWin::Unmanaged* Found Unmanaged or @c null if there is no Unmanaged with given Id.
     */
    Unmanaged *findUnmanaged(xcb_window_t w) const;
    void forEachUnmanaged(std::function<void (Unmanaged*)> func);

    QRect clientArea(clientAreaOption, const QPoint& p, int desktop) const;
    QRect clientArea(clientAreaOption, const Client* c) const;
    QRect clientArea(clientAreaOption, int screen, int desktop) const;

    QRegion restrictedMoveArea(int desktop, StrutAreas areas = StrutAreaAll) const;

    bool initializing() const;

    /**
     * Returns the active client, i.e. the client that has the focus (or None
     * if no client has the focus)
     */
    Client* activeClient() const;
    /**
     * Client that was activated, but it's not yet really activeClient(), because
     * we didn't process yet the matching FocusIn event. Used mostly in focus
     * stealing prevention code.
     */
    Client* mostRecentlyActivatedClient() const;

    Client* clientUnderMouse(int screen) const;

    void activateClient(Client*, bool force = false);
    void requestFocus(Client* c, bool force = false);
    enum ActivityFlag {
        ActivityFocus = 1 << 0, // focus the window
        ActivityFocusForce = 1 << 1 | ActivityFocus, // focus even if Dock etc.
        ActivityRaise = 1 << 2 // raise the window
    };
    Q_DECLARE_FLAGS(ActivityFlags, ActivityFlag)
    void takeActivity(Client* c, ActivityFlags flags);
    bool allowClientActivation(const Client* c, xcb_timestamp_t time = -1U, bool focus_in = false,
                               bool ignore_desktop = false);
    void restoreFocus();
    void gotFocusIn(const Client*);
    void setShouldGetFocus(Client*);
    bool activateNextClient(Client* c);
    bool focusChangeEnabled() {
        return block_focus == 0;
    }

    /**
     * Indicates that the client c is being moved around by the user.
     */
    void setClientIsMoving(Client* c);

    QPoint adjustClientPosition(Client* c, QPoint pos, bool unrestricted, double snapAdjust = 1.0);
    QRect adjustClientSize(Client* c, QRect moveResizeGeom, int mode);
    void raiseClient(Client* c, bool nogroup = false);
    void lowerClient(Client* c, bool nogroup = false);
    void raiseClientRequest(Client* c, NET::RequestSource src, xcb_timestamp_t timestamp);
    void lowerClientRequest(Client* c, NET::RequestSource src, xcb_timestamp_t timestamp);
    void restackClientUnderActive(Client*);
    void restack(Client *c, Client *under);
    void updateClientLayer(Client* c);
    void raiseOrLowerClient(Client*);
    void resetUpdateToolWindowsTimer();
    void restoreSessionStackingOrder(Client* c);
    void updateStackingOrder(bool propagate_new_clients = false);
    void forceRestacking();

    void clientHidden(Client*);
    void clientAttentionChanged(Client* c, bool set);

    /**
     * @return List of clients currently managed by Workspace
     **/
    const ClientList &clientList() const {
        return clients;
    }
    /**
     * @return List of unmanaged "clients" currently registered in Workspace
     **/
    const UnmanagedList &unmanagedList() const {
        return unmanaged;
    }
    /**
     * @return List of desktop "clients" currently managed by Workspace
     **/
    const ClientList &desktopList() const {
        return desktops;
    }
    /**
     * @return List of deleted "clients" currently managed by Workspace
     **/
    const DeletedList &deletedList() const {
        return deleted;
    }

#ifdef KWIN_BUILD_SCREENEDGES
    void stackScreenEdgesUnderOverrideRedirect();
#endif

public:
    QPoint cascadeOffset(const Client *c) const;

private:
    Compositor *m_compositor;

    //-------------------------------------------------
    // Unsorted

public:
    bool isOnCurrentHead();
    // True when performing Workspace::updateClientArea().
    // The calls below are valid only in that case.
    bool inUpdateClientArea() const;
    QRegion previousRestrictedMoveArea(int desktop, StrutAreas areas = StrutAreaAll) const;
    QVector< QRect > previousScreenSizes() const;
    int oldDisplayWidth() const;
    int oldDisplayHeight() const;

    /**
     * Returns the list of clients sorted in stacking order, with topmost client
     * at the last position
     */
    const ToplevelList& stackingOrder() const;
    ToplevelList xStackingOrder() const;
    ClientList ensureStackingOrder(const ClientList& clients) const;

    Client* topClientOnDesktop(int desktop, int screen, bool unconstrained = false,
                               bool only_normal = true) const;
    Client* findDesktop(bool topmost, int desktop) const;
    void sendClientToDesktop(Client* c, int desktop, bool dont_activate);
    void windowToPreviousDesktop(Client* c);
    void windowToNextDesktop(Client* c);
    void sendClientToScreen(Client* c, int screen);

    /**
     * Shows the menu operations menu for the client and makes it active if
     * it's not already.
     */
    void showWindowMenu(const QRect& pos, Client* cl);
    const UserActionsMenu *userActionsMenu() const {
        return m_userActionsMenu;
    }

    void updateMinimizedOfTransients(Client*);
    void updateOnAllDesktopsOfTransients(Client*);
    void checkTransients(xcb_window_t w);

    void storeSession(KConfig* config, SMSavePhase phase);
    void storeClient(KConfigGroup &cg, int num, Client *c);
    void storeSubSession(const QString &name, QSet<QByteArray> sessionIds);
    void loadSubSessionInfo(const QString &name);

    SessionInfo* takeSessionInfo(Client*);

    // D-Bus interface
    bool waitForCompositingSetup();
    QString supportInformation() const;

    void setCurrentScreen(int new_screen);

    void setShowingDesktop(bool showing);
    void resetShowingDesktop(bool keep_hidden);
    bool showingDesktop() const;

    void sendPingToWindow(xcb_window_t w, xcb_timestamp_t timestamp);   // Called from Client::pingWindow()

    void removeClient(Client*);   // Only called from Client::destroyClient() or Client::releaseWindow()
    void setActiveClient(Client*);
    Group* findGroup(xcb_window_t leader) const;
    void addGroup(Group* group);
    void removeGroup(Group* group);
    Group* findClientLeaderGroup(const Client* c) const;

    void removeUnmanaged(Unmanaged*);   // Only called from Unmanaged::release()
    void removeDeleted(Deleted*);
    void addDeleted(Deleted*, Toplevel*);

    bool checkStartupNotification(xcb_window_t w, KStartupInfoId& id, KStartupInfoData& data);

    void focusToNull(); // SELI TODO: Public?

    void clientShortcutUpdated(Client* c);
    bool shortcutAvailable(const QKeySequence &cut, Client* ignore = NULL) const;
    bool globalShortcutsDisabled() const;
    void disableGlobalShortcutsForClient(bool disable);

    void sessionSaveStarted();
    void sessionSaveDone();
    void setWasUserInteraction();
    bool wasUserInteraction() const;
    bool sessionSaving() const;

    int packPositionLeft(const Client* cl, int oldx, bool left_edge) const;
    int packPositionRight(const Client* cl, int oldx, bool right_edge) const;
    int packPositionUp(const Client* cl, int oldy, bool top_edge) const;
    int packPositionDown(const Client* cl, int oldy, bool bottom_edge) const;

    void cancelDelayFocus();
    void requestDelayFocus(Client*);
    void updateFocusMousePosition(const QPoint& pos);
    QPoint focusMousePosition() const;

    Client* getMovingClient() {
        return movingClient;
    }

    /**
     * @returns Whether we have a Compositor and it is active (Scene created)
     **/
    bool compositing() const;

public Q_SLOTS:
    void performWindowOperation(KWin::Client* c, KDecorationDefines::WindowOperation op);
    // Keybindings
    //void slotSwitchToWindow( int );
    void slotWindowToDesktop();

    //void slotWindowToListPosition( int );
    void slotSwitchToScreen();
    void slotWindowToScreen();
    void slotSwitchToNextScreen();
    void slotWindowToNextScreen();
    void slotSwitchToPrevScreen();
    void slotWindowToPrevScreen();
    void slotToggleShowDesktop();

    void slotWindowMaximize();
    void slotWindowMaximizeVertical();
    void slotWindowMaximizeHorizontal();
    void slotWindowMinimize();
    void slotWindowShade();
    void slotWindowRaise();
    void slotWindowLower();
    void slotWindowRaiseOrLower();
    void slotActivateAttentionWindow();
    void slotWindowPackLeft();
    void slotWindowPackRight();
    void slotWindowPackUp();
    void slotWindowPackDown();
    void slotWindowGrowHorizontal();
    void slotWindowGrowVertical();
    void slotWindowShrinkHorizontal();
    void slotWindowShrinkVertical();
    void slotWindowQuickTileLeft();
    void slotWindowQuickTileRight();
    void slotWindowQuickTileTopLeft();
    void slotWindowQuickTileTopRight();
    void slotWindowQuickTileBottomLeft();
    void slotWindowQuickTileBottomRight();

    void slotSwitchWindowUp();
    void slotSwitchWindowDown();
    void slotSwitchWindowRight();
    void slotSwitchWindowLeft();

    void slotIncreaseWindowOpacity();
    void slotLowerWindowOpacity();

    void slotWindowOperations();
    void slotWindowClose();
    void slotWindowMove();
    void slotWindowResize();
    void slotWindowAbove();
    void slotWindowBelow();
    void slotWindowOnAllDesktops();
    void slotWindowFullScreen();
    void slotWindowNoBorder();

    void slotWindowToNextDesktop();
    void slotWindowToPreviousDesktop();
    void slotWindowToDesktopRight();
    void slotWindowToDesktopLeft();
    void slotWindowToDesktopUp();
    void slotWindowToDesktopDown();

    void reconfigure();
    void slotReconfigure();

    void slotKillWindow();

    void slotSetupWindowShortcut();
    void setupWindowShortcutDone(bool);
    void slotToggleCompositing();
    void slotInvertScreen();

    void updateClientArea();

    void slotActivateNextTab(); // Slot to move left the active Client.
    void slotActivatePrevTab(); // Slot to move right the active Client.
    void slotUntab(); // Slot to remove the active client from its group.

private Q_SLOTS:
    void desktopResized();
    void selectWmInputEventMask();
    void slotUpdateToolWindows();
    void delayFocus();
    void slotReloadConfig();
    void updateCurrentActivity(const QString &new_activity);
    // virtual desktop handling
    void moveClientsFromRemovedDesktops();
    void slotDesktopCountChanged(uint previousCount, uint newCount);
    void slotCurrentDesktopChanged(uint oldDesktop, uint newDesktop);

Q_SIGNALS:
    /**
     * Emitted after the Workspace has setup the complete initialization process.
     * This can be used to connect to for performing post-workspace initialization.
     **/
    void workspaceInitialized();

    //Signals required for the scripting interface
    void desktopPresenceChanged(KWin::Client*, int);
    void currentDesktopChanged(int, KWin::Client*);
    void clientAdded(KWin::Client*);
    void clientRemoved(KWin::Client*);
    void clientActivated(KWin::Client*);
    void clientDemandsAttentionChanged(KWin::Client*, bool);
    void groupAdded(KWin::Group*);
    void unmanagedAdded(KWin::Unmanaged*);
    void unmanagedRemoved(KWin::Unmanaged*);
    void deletedRemoved(KWin::Deleted*);
    void propertyNotify(long a);
    void configChanged();
    void reinitializeCompositing();
    /**
     * This signels is emitted when ever the stacking order is change, ie. a window is risen
     * or lowered
     */
    void stackingOrderChanged();

private:
    void init();
    void initShortcuts();
    template <typename Slot>
    void initShortcut(const QString &actionName, const QString &description, const QKeySequence &shortcut,
                      Slot slot, const QVariant &data = QVariant());
    void setupWindowShortcut(Client* c);
    enum Direction {
        DirectionNorth,
        DirectionEast,
        DirectionSouth,
        DirectionWest
    };
    void switchWindow(Direction direction);

    void propagateClients(bool propagate_new_clients);   // Called only from updateStackingOrder
    ToplevelList constrainedStackingOrder();
    void raiseClientWithinApplication(Client* c);
    void lowerClientWithinApplication(Client* c);
    bool allowFullClientRaising(const Client* c, xcb_timestamp_t timestamp);
    bool keepTransientAbove(const Client* mainwindow, const Client* transient);
    void blockStackingUpdates(bool block);
    void updateToolWindows(bool also_hide);
    void fixPositionAfterCrash(xcb_window_t w, const xcb_get_geometry_reply_t *geom);
    void saveOldScreenSizes();

    /// This is the right way to create a new client
    Client* createClient(xcb_window_t w, bool is_mapped);
    void addClient(Client* c);
    Unmanaged* createUnmanaged(xcb_window_t w);
    void addUnmanaged(Unmanaged* c);

    //---------------------------------------------------------------------

    void closeActivePopup();
    void updateClientArea(bool force);
    void resetClientAreas(uint desktopCount);
    void updateClientVisibilityOnDesktopChange(uint oldDesktop, uint newDesktop);
    void activateClientOnNewDesktop(uint desktop);
    Client *findClientToActivateOnDesktop(uint desktop);

    QWidget* active_popup;
    Client* active_popup_client;

    void loadSessionInfo();
    void addSessionInfo(KConfigGroup &cg);

    QList<SessionInfo*> session;
    static const char* windowTypeToTxt(NET::WindowType type);
    static NET::WindowType txtToWindowType(const char* txt);
    static bool sessionInfoWindowTypeMatch(Client* c, SessionInfo* info);

    Client* active_client;
    Client* last_active_client;
    Client* most_recently_raised; // Used ONLY by raiseOrLowerClient()
    Client* movingClient;

    // Delay(ed) window focus timer and client
    QTimer* delayFocusTimer;
    Client* delayfocus_client;
    QPoint focusMousePos;

    ClientList clients;
    ClientList desktops;
    UnmanagedList unmanaged;
    DeletedList deleted;

    ToplevelList unconstrained_stacking_order; // Topmost last
    ToplevelList stacking_order; // Topmost last
    bool force_restacking;
    mutable ToplevelList x_stacking; // From XQueryTree()
    mutable bool x_stacking_dirty;
    ClientList should_get_focus; // Last is most recent
    ClientList attention_chain;

    bool showing_desktop;
    ClientList showing_desktop_clients;
    int block_showing_desktop;

    GroupList groups;

    bool was_user_interaction;
    bool session_saving;
    int session_active_client;
    int session_desktop;

    int block_focus;

    /**
     * Holds the menu containing the user actions which is shown
     * on e.g. right click the window decoration.
     **/
    UserActionsMenu *m_userActionsMenu;

    void modalActionsSwitch(bool enabled);

    ShortcutDialog* client_keys_dialog;
    Client* client_keys_client;
    bool global_shortcuts_disabled_for_client;

    // Timer to collect requests for 'reconfigure'
    QTimer reconfigureTimer;

    QTimer updateToolWindowsTimer;

    static Workspace* _self;

    bool workspaceInit;

    KStartupInfo* startup;

    QVector<QRect> workarea; // Array of workareas for virtual desktops
    // Array of restricted areas that window cannot be moved into
    QVector<StrutRects> restrictedmovearea;
    // Array of the previous restricted areas that window cannot be moved into
    QVector<StrutRects> oldrestrictedmovearea;
    QVector< QVector<QRect> > screenarea; // Array of workareas per xinerama screen for all virtual desktops
    QVector< QRect > oldscreensizes; // array of previous sizes of xinerama screens
    QSize olddisplaysize; // previous sizes od displayWidth()/displayHeight()

    int set_active_client_recursion;
    int block_stacking_updates; // When > 0, stacking updates are temporarily disabled
    bool blocked_propagating_new_clients; // Propagate also new clients after enabling stacking updates?
    QScopedPointer<Xcb::Window> m_nullFocus;
    friend class StackingUpdatesBlocker;

    QScopedPointer<KillWindow> m_windowKiller;

private:
    friend bool performTransiencyCheck();
    friend Workspace *workspace();
};

/**
 * Helper for Workspace::blockStackingUpdates() being called in pairs (True/false)
 */
class StackingUpdatesBlocker
{
public:
    explicit StackingUpdatesBlocker(Workspace* w)
        : ws(w) {
        ws->blockStackingUpdates(true);
    }
    ~StackingUpdatesBlocker() {
        ws->blockStackingUpdates(false);
    }

private:
    Workspace* ws;
};

class ColorMapper : public QObject
{
    Q_OBJECT
public:
    ColorMapper(QObject *parent);
    virtual ~ColorMapper();
public Q_SLOTS:
    void update();
private:
    xcb_colormap_t m_default;
    xcb_colormap_t m_installed;
};

//---------------------------------------------------------
// Unsorted

inline bool Workspace::initializing() const
{
    return workspaceInit;
}

inline Client* Workspace::activeClient() const
{
    return active_client;
}

inline Client* Workspace::mostRecentlyActivatedClient() const
{
    return should_get_focus.count() > 0 ? should_get_focus.last() : active_client;
}

inline void Workspace::addGroup(Group* group)
{
    emit groupAdded(group);
    groups.append(group);
}

inline void Workspace::removeGroup(Group* group)
{
    groups.removeAll(group);
}

inline const ToplevelList& Workspace::stackingOrder() const
{
    // TODO: Q_ASSERT( block_stacking_updates == 0 );
    return stacking_order;
}

inline void Workspace::setWasUserInteraction()
{
    was_user_interaction = true;
}

inline bool Workspace::wasUserInteraction() const
{
    return was_user_interaction;
}

inline void Workspace::sessionSaveStarted()
{
    session_saving = true;
}

inline bool Workspace::sessionSaving() const
{
    return session_saving;
}

inline bool Workspace::showingDesktop() const
{
    return showing_desktop;
}

inline bool Workspace::globalShortcutsDisabled() const
{
    return global_shortcuts_disabled_for_client;
}

inline void Workspace::forceRestacking()
{
    force_restacking = true;
    StackingUpdatesBlocker blocker(this);   // Do restacking if not blocked
}

inline void Workspace::updateFocusMousePosition(const QPoint& pos)
{
    focusMousePos = pos;
}

inline QPoint Workspace::focusMousePosition() const
{
    return focusMousePos;
}

inline
void Workspace::forEachClient(std::function< void (Client*) > func)
{
    std::for_each(clients.constBegin(), clients.constEnd(), func);
    std::for_each(desktops.constBegin(), desktops.constEnd(), func);
}

inline
void Workspace::forEachUnmanaged(std::function< void (Unmanaged*) > func)
{
    std::for_each(unmanaged.constBegin(), unmanaged.constEnd(), func);
}

inline bool Workspace::hasClient(const Client* c)
{
    return findClient([c](const Client *test) {
        return test == c;
    });
}

inline Workspace *workspace()
{
    return Workspace::_self;
}

} // namespace
Q_DECLARE_OPERATORS_FOR_FLAGS(KWin::Workspace::ActivityFlags)

#endif
