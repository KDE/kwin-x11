/********************************************************************
 KWin - the KDE window manager
 This file is part of the KDE project.

Copyright (C) 1999, 2000 Matthias Ettrich <ettrich@kde.org>
Copyright (C) 2003 Lubos Lunak <l.lunak@kde.org>
Copyright (C) 2009 Martin Gräßlin <mgraesslin@kde.org>

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

//#define QT_CLEAN_NAMESPACE
// own
#include "tabbox.h"
// tabbox
#include "tabbox/clientmodel.h"
#include "tabbox/desktopmodel.h"
#include "tabbox/tabboxconfig.h"
#include "tabbox/desktopchain.h"
// kwin
#ifdef KWIN_BUILD_ACTIVITIES
#include "activities.h"
#endif
#include "client.h"
#include "effects.h"
#include "input.h"
#include "focuschain.h"
#ifdef KWIN_BUILD_SCREENEDGES
#include "screenedge.h"
#endif
#include "screens.h"
#include "unmanaged.h"
#include "virtualdesktops.h"
#include "workspace.h"
#include "xcbutils.h"
// Qt
#include <QAction>
#include <QDebug>
// KDE
#include <KConfig>
#include <KConfigGroup>
#include <KGlobalAccel>
#include <KLocalizedString>
#include <kkeyserver.h>
// X11
#include <X11/keysym.h>
#include <X11/keysymdef.h>

// specify externals before namespace

namespace KWin
{

namespace TabBox
{

TabBoxHandlerImpl::TabBoxHandlerImpl(TabBox* tabBox)
    : TabBoxHandler(tabBox)
    , m_tabBox(tabBox)
    , m_desktopFocusChain(new DesktopChainManager(this))
{
    // connects for DesktopFocusChainManager
    VirtualDesktopManager *vds = VirtualDesktopManager::self();
    connect(vds, SIGNAL(countChanged(uint,uint)), m_desktopFocusChain, SLOT(resize(uint,uint)));
    connect(vds, SIGNAL(currentChanged(uint,uint)), m_desktopFocusChain, SLOT(addDesktop(uint,uint)));
#ifdef KWIN_BUILD_ACTIVITIES
    connect(Activities::self(), SIGNAL(currentChanged(QString)), m_desktopFocusChain, SLOT(useChain(QString)));
#endif
}

TabBoxHandlerImpl::~TabBoxHandlerImpl()
{
}

int TabBoxHandlerImpl::activeScreen() const
{
    return screens()->current();
}

int TabBoxHandlerImpl::currentDesktop() const
{
    return VirtualDesktopManager::self()->current();
}

QString TabBoxHandlerImpl::desktopName(TabBoxClient* client) const
{
    if (TabBoxClientImpl* c = static_cast< TabBoxClientImpl* >(client)) {
        if (!c->client()->isOnAllDesktops())
            return VirtualDesktopManager::self()->name(c->client()->desktop());
    }
    return VirtualDesktopManager::self()->name(VirtualDesktopManager::self()->current());
}

QString TabBoxHandlerImpl::desktopName(int desktop) const
{
    return VirtualDesktopManager::self()->name(desktop);
}

QWeakPointer<TabBoxClient> TabBoxHandlerImpl::nextClientFocusChain(TabBoxClient* client) const
{
    if (TabBoxClientImpl* c = static_cast< TabBoxClientImpl* >(client)) {
        Client* next = FocusChain::self()->nextMostRecentlyUsed(c->client());
        if (next)
            return next->tabBoxClient();
    }
    return QWeakPointer<TabBoxClient>();
}

QWeakPointer< TabBoxClient > TabBoxHandlerImpl::firstClientFocusChain() const
{
    if (Client *c = FocusChain::self()->firstMostRecentlyUsed()) {
        return QWeakPointer<TabBoxClient>(c->tabBoxClient());
    } else {
        return QWeakPointer<TabBoxClient>();
    }
}

bool TabBoxHandlerImpl::isInFocusChain(TabBoxClient *client) const
{
    if (TabBoxClientImpl *c = static_cast<TabBoxClientImpl*>(client)) {
        return FocusChain::self()->contains(c->client());
    }
    return false;
}

int TabBoxHandlerImpl::nextDesktopFocusChain(int desktop) const
{
    return m_desktopFocusChain->next(desktop);
}

int TabBoxHandlerImpl::numberOfDesktops() const
{
    return VirtualDesktopManager::self()->count();
}

QWeakPointer<TabBoxClient> TabBoxHandlerImpl::activeClient() const
{
    if (Workspace::self()->activeClient())
        return Workspace::self()->activeClient()->tabBoxClient();
    else
        return QWeakPointer<TabBoxClient>();
}

bool TabBoxHandlerImpl::checkDesktop(TabBoxClient* client, int desktop) const
{
    Client* current = (static_cast< TabBoxClientImpl* >(client))->client();

    switch (config().clientDesktopMode()) {
    case TabBoxConfig::AllDesktopsClients:
        return true;
    case TabBoxConfig::ExcludeCurrentDesktopClients:
        return !current->isOnDesktop(desktop);
    default:       // TabBoxConfig::OnlyCurrentDesktopClients
        return current->isOnDesktop(desktop);
    }
}

bool TabBoxHandlerImpl::checkActivity(TabBoxClient* client) const
{
    Client* current = (static_cast< TabBoxClientImpl* >(client))->client();

    switch (config().clientActivitiesMode()) {
    case TabBoxConfig::AllActivitiesClients:
        return true;
    case TabBoxConfig::ExcludeCurrentActivityClients:
        return !current->isOnCurrentActivity();
    default:       // TabBoxConfig::OnlyCurrentActivityClients
        return current->isOnCurrentActivity();
    }
}

bool TabBoxHandlerImpl::checkApplications(TabBoxClient* client) const
{
    Client* current = (static_cast< TabBoxClientImpl* >(client))->client();
    TabBoxClientImpl* c;
    QListIterator< QWeakPointer<TabBoxClient> > i(clientList());

    switch (config().clientApplicationsMode()) {
    case TabBoxConfig::OneWindowPerApplication:
        // check if the list already contains an entry of this application
        while (i.hasNext()) {
            QSharedPointer<TabBoxClient> client = i.next().toStrongRef();
            if (!client) {
                continue;
            }
            if ((c = dynamic_cast< TabBoxClientImpl* >(client.data()))) {
                if (c->client()->resourceClass() == current->resourceClass()) {
                    return false;
                }
            }
	}
        return true;
    case TabBoxConfig::AllWindowsCurrentApplication: {
        QSharedPointer<TabBoxClient> pointer = tabBox->activeClient().toStrongRef();
        if (!pointer) {
            return false;
        }
        if ((c = dynamic_cast< TabBoxClientImpl* >(pointer.data()))) {
            if (c->client()->resourceClass() == current->resourceClass()) {
                return true;
            }
        }
        return false;
    }
    default:       // TabBoxConfig::AllWindowsAllApplications
      return true;
    }
}

bool TabBoxHandlerImpl::checkMinimized(TabBoxClient* client) const
{
    switch (config().clientMinimizedMode()) {
    case TabBoxConfig::ExcludeMinimizedClients:
        return !client->isMinimized();
    case TabBoxConfig::OnlyMinimizedClients:
        return client->isMinimized();
    default:       // TabBoxConfig::IgnoreMinimizedStatus
        return true;
    }
}

bool TabBoxHandlerImpl::checkMultiScreen(TabBoxClient* client) const
{
    Client* current = (static_cast< TabBoxClientImpl* >(client))->client();

    switch (config().clientMultiScreenMode()) {
    case TabBoxConfig::IgnoreMultiScreen:
        return true;
    case TabBoxConfig::ExcludeCurrentScreenClients:
        return current->screen() != screens()->current();
    default:       // TabBoxConfig::OnlyCurrentScreenClients
        return current->screen() == screens()->current();
    }
}

QWeakPointer<TabBoxClient> TabBoxHandlerImpl::clientToAddToList(TabBoxClient* client, int desktop) const
{
    if (!client) {
        return QWeakPointer<TabBoxClient>();
    }
    Client* ret = nullptr;
    Client* current = (static_cast< TabBoxClientImpl* >(client))->client();

    bool addClient = checkDesktop(client, desktop)
                  && checkActivity(client)
                  && checkApplications(client)
                  && checkMinimized(client)
                  && checkMultiScreen(client);
    addClient = addClient && current->wantsTabFocus() && !current->skipSwitcher();
    if (addClient) {
        // don't add windows that have modal dialogs
        Client* modal = current->findModal();
        if (modal == nullptr || modal == current)
            ret = current;
        else if (!clientList().contains(modal->tabBoxClient()))
            ret = modal;
        else {
            // nothing
        }
    }
    if (ret)
        return ret->tabBoxClient();
    else
        return QWeakPointer<TabBoxClient>();
}

TabBoxClientList TabBoxHandlerImpl::stackingOrder() const
{
    ToplevelList stacking = Workspace::self()->stackingOrder();
    TabBoxClientList ret;
    foreach (Toplevel *toplevel, stacking) {
        if (Client *client = qobject_cast<Client*>(toplevel)) {
            ret.append(client->tabBoxClient());
        }
    }
    return ret;
}

bool TabBoxHandlerImpl::isKWinCompositing() const {
    return Workspace::self()->compositing();
}

void TabBoxHandlerImpl::raiseClient(TabBoxClient* c) const
{
    Workspace::self()->raiseClient(static_cast<TabBoxClientImpl*>(c)->client());
}

void TabBoxHandlerImpl::restack(TabBoxClient *c, TabBoxClient *under)
{
    Workspace::self()->restack(static_cast<TabBoxClientImpl*>(c)->client(),
                               static_cast<TabBoxClientImpl*>(under)->client());
}

void TabBoxHandlerImpl::elevateClient(TabBoxClient *c, WId tabbox, bool b) const
{
    Client *cl = static_cast<TabBoxClientImpl*>(c)->client();
    cl->elevate(b);
    if (Unmanaged *w = Workspace::self()->findUnmanaged(tabbox))
        w->elevate(b);
}


QWeakPointer<TabBoxClient> TabBoxHandlerImpl::desktopClient() const
{
    foreach (Toplevel *toplevel, Workspace::self()->stackingOrder()) {
        Client *client = qobject_cast<Client*>(toplevel);
        if (client && client->isDesktop() && client->isOnCurrentDesktop() && client->screen() == screens()->current()) {
            return client->tabBoxClient();
        }
    }
    return QWeakPointer<TabBoxClient>();
}

void TabBoxHandlerImpl::activateAndClose()
{
    m_tabBox->accept();
}

/*********************************************************
* TabBoxClientImpl
*********************************************************/

TabBoxClientImpl::TabBoxClientImpl(Client *client)
    : TabBoxClient()
    , m_client(client)
{
}

TabBoxClientImpl::~TabBoxClientImpl()
{
}

QString TabBoxClientImpl::caption() const
{
    if (m_client->isDesktop())
        return i18nc("Special entry in alt+tab list for minimizing all windows",
                     "Show Desktop");
    return m_client->caption();
}

QIcon TabBoxClientImpl::icon() const
{
    if (m_client->isDesktop()) {
        return QIcon::fromTheme(QStringLiteral("user-desktop"));
    }
    return m_client->icon();
}

WId TabBoxClientImpl::window() const
{
    return m_client->window();
}

bool TabBoxClientImpl::isMinimized() const
{
    return m_client->isMinimized();
}

int TabBoxClientImpl::x() const
{
    return m_client->x();
}

int TabBoxClientImpl::y() const
{
    return m_client->y();
}

int TabBoxClientImpl::width() const
{
    return m_client->width();
}

int TabBoxClientImpl::height() const
{
    return m_client->height();
}

bool TabBoxClientImpl::isCloseable() const
{
    return m_client->isCloseable();
}

void TabBoxClientImpl::close()
{
    m_client->closeWindow();
}

bool TabBoxClientImpl::isFirstInTabBox() const
{
    return m_client->isFirstInTabBox();
}

/*********************************************************
* TabBox
*********************************************************/
TabBox *TabBox::s_self = nullptr;

TabBox *TabBox::create(QObject *parent)
{
    Q_ASSERT(!s_self);
    s_self = new TabBox(parent);
    return s_self;
}

TabBox::TabBox(QObject *parent)
    : QObject(parent)
    , m_displayRefcount(0)
    , m_desktopGrab(false)
    , m_tabGrab(false)
    , m_noModifierGrab(false)
    , m_forcedGlobalMouseGrab(false)
    , m_ready(false)
{
    m_isShown = false;
    m_defaultConfig = TabBoxConfig();
    m_defaultConfig.setTabBoxMode(TabBoxConfig::ClientTabBox);
    m_defaultConfig.setClientDesktopMode(TabBoxConfig::OnlyCurrentDesktopClients);
    m_defaultConfig.setClientActivitiesMode(TabBoxConfig::OnlyCurrentActivityClients);
    m_defaultConfig.setClientApplicationsMode(TabBoxConfig::AllWindowsAllApplications);
    m_defaultConfig.setClientMinimizedMode(TabBoxConfig::IgnoreMinimizedStatus);
    m_defaultConfig.setShowDesktopMode(TabBoxConfig::DoNotShowDesktopClient);
    m_defaultConfig.setClientMultiScreenMode(TabBoxConfig::IgnoreMultiScreen);
    m_defaultConfig.setClientSwitchingMode(TabBoxConfig::FocusChainSwitching);

    m_alternativeConfig = TabBoxConfig();
    m_alternativeConfig.setTabBoxMode(TabBoxConfig::ClientTabBox);
    m_alternativeConfig.setClientDesktopMode(TabBoxConfig::AllDesktopsClients);
    m_alternativeConfig.setClientActivitiesMode(TabBoxConfig::OnlyCurrentActivityClients);
    m_alternativeConfig.setClientApplicationsMode(TabBoxConfig::AllWindowsAllApplications);
    m_alternativeConfig.setClientMinimizedMode(TabBoxConfig::IgnoreMinimizedStatus);
    m_alternativeConfig.setShowDesktopMode(TabBoxConfig::DoNotShowDesktopClient);
    m_alternativeConfig.setClientMultiScreenMode(TabBoxConfig::IgnoreMultiScreen);
    m_alternativeConfig.setClientSwitchingMode(TabBoxConfig::FocusChainSwitching);

    m_defaultCurrentApplicationConfig = m_defaultConfig;
    m_defaultCurrentApplicationConfig.setClientApplicationsMode(TabBoxConfig::AllWindowsCurrentApplication);

    m_alternativeCurrentApplicationConfig = m_alternativeConfig;
    m_alternativeCurrentApplicationConfig.setClientApplicationsMode(TabBoxConfig::AllWindowsCurrentApplication);

    m_desktopConfig = TabBoxConfig();
    m_desktopConfig.setTabBoxMode(TabBoxConfig::DesktopTabBox);
    m_desktopConfig.setShowTabBox(true);
    m_desktopConfig.setShowDesktopMode(TabBoxConfig::DoNotShowDesktopClient);
    m_desktopConfig.setDesktopSwitchingMode(TabBoxConfig::MostRecentlyUsedDesktopSwitching);

    m_desktopListConfig = TabBoxConfig();
    m_desktopListConfig.setTabBoxMode(TabBoxConfig::DesktopTabBox);
    m_desktopListConfig.setShowTabBox(true);
    m_desktopListConfig.setShowDesktopMode(TabBoxConfig::DoNotShowDesktopClient);
    m_desktopListConfig.setDesktopSwitchingMode(TabBoxConfig::StaticDesktopSwitching);
    m_tabBox = new TabBoxHandlerImpl(this);
    QTimer::singleShot(0, this, SLOT(handlerReady()));

    m_tabBoxMode = TabBoxDesktopMode; // init variables
    connect(&m_delayedShowTimer, SIGNAL(timeout()), this, SLOT(show()));
    connect(Workspace::self(), SIGNAL(configChanged()), this, SLOT(reconfigure()));

    connect(input(), &InputRedirection::keyboardModifiersChanged, this, &TabBox::modifiersChanged);
}

TabBox::~TabBox()
{
    s_self = nullptr;
}

void TabBox::handlerReady()
{
    m_tabBox->setConfig(m_defaultConfig);
    reconfigure();
    m_ready = true;
}

template <typename Slot>
void TabBox::key(const char *actionName, Slot slot, const QKeySequence &shortcut)
{
    QAction *a = new QAction(this);
    a->setObjectName(QString::fromUtf8(actionName));
    a->setText(i18n(actionName));
    KGlobalAccel::self()->setShortcut(a, QList<QKeySequence>() << shortcut);
    connect(a, &QAction::triggered, TabBox::self(), slot);
    input()->registerShortcut(shortcut, a);
    auto cuts = KGlobalAccel::self()->shortcut(a);
    globalShortcutChanged(a, cuts.isEmpty() ? QKeySequence() : cuts.first());
}

static const char *s_windows        = I18N_NOOP("Walk Through Windows");
static const char *s_windowsRev     = I18N_NOOP("Walk Through Windows (Reverse)");
static const char *s_windowsAlt     = I18N_NOOP("Walk Through Windows Alternative");
static const char *s_windowsAltRev  = I18N_NOOP("Walk Through Windows Alternative (Reverse)");
static const char *s_app            = I18N_NOOP("Walk Through Windows of Current Application");
static const char *s_appRev         = I18N_NOOP("Walk Through Windows of Current Application (Reverse)");
static const char *s_appAlt         = I18N_NOOP("Walk Through Windows of Current Application Alternative");
static const char *s_appAltRev      = I18N_NOOP("Walk Through Windows of Current Application Alternative (Reverse)");
static const char *s_desktops       = I18N_NOOP("Walk Through Desktops");
static const char *s_desktopsRev    = I18N_NOOP("Walk Through Desktops (Reverse)");
static const char *s_desktopList    = I18N_NOOP("Walk Through Desktop List");
static const char *s_desktopListRev = I18N_NOOP("Walk Through Desktop List (Reverse)");

void TabBox::initShortcuts()
{
    key(s_windows,        &TabBox::slotWalkThroughWindows, Qt::ALT + Qt::Key_Tab);
    key(s_windowsRev,     &TabBox::slotWalkBackThroughWindows, Qt::ALT + Qt::SHIFT + Qt::Key_Backtab);
    key(s_app,            &TabBox::slotWalkThroughCurrentAppWindows, Qt::ALT + Qt::Key_QuoteLeft);
    key(s_appRev,         &TabBox::slotWalkBackThroughCurrentAppWindows, Qt::ALT + Qt::Key_AsciiTilde);
    key(s_windowsAlt,     &TabBox::slotWalkThroughWindowsAlternative);
    key(s_windowsAltRev,  &TabBox::slotWalkBackThroughWindowsAlternative);
    key(s_appAlt,         &TabBox::slotWalkThroughCurrentAppWindowsAlternative);
    key(s_appAltRev,      &TabBox::slotWalkBackThroughCurrentAppWindowsAlternative);
    key(s_desktops,       &TabBox::slotWalkThroughDesktops);
    key(s_desktopsRev,    &TabBox::slotWalkBackThroughDesktops);
    key(s_desktopList,    &TabBox::slotWalkThroughDesktopList);
    key(s_desktopListRev, &TabBox::slotWalkBackThroughDesktopList);

    connect(KGlobalAccel::self(), &KGlobalAccel::globalShortcutChanged, this, &TabBox::globalShortcutChanged);
}

void TabBox::globalShortcutChanged(QAction *action, const QKeySequence &seq)
{
    if (qstrcmp(qPrintable(action->objectName()), s_windows) == 0) {
        m_cutWalkThroughWindows = seq;
    } else if (qstrcmp(qPrintable(action->objectName()), s_windowsRev) == 0) {
        m_cutWalkThroughWindowsReverse = seq;
    } else if (qstrcmp(qPrintable(action->objectName()), s_app) == 0) {
        m_cutWalkThroughCurrentAppWindows = seq;
    } else if (qstrcmp(qPrintable(action->objectName()), s_appRev) == 0) {
        m_cutWalkThroughCurrentAppWindowsReverse = seq;
    } else if (qstrcmp(qPrintable(action->objectName()), s_windowsAlt) == 0) {
        m_cutWalkThroughWindowsAlternative = seq;
    } else if (qstrcmp(qPrintable(action->objectName()), s_windowsAltRev) == 0) {
        m_cutWalkThroughWindowsAlternativeReverse = seq;
    } else if (qstrcmp(qPrintable(action->objectName()), s_appAlt) == 0) {
        m_cutWalkThroughCurrentAppWindowsAlternative = seq;
    } else if (qstrcmp(qPrintable(action->objectName()), s_appAltRev) == 0) {
        m_cutWalkThroughCurrentAppWindowsAlternativeReverse = seq;
    } else if (qstrcmp(qPrintable(action->objectName()), s_desktops) == 0) {
        m_cutWalkThroughDesktops = seq;
    } else if (qstrcmp(qPrintable(action->objectName()), s_desktopsRev) == 0) {
        m_cutWalkThroughDesktopsReverse = seq;
    } else if (qstrcmp(qPrintable(action->objectName()), s_desktopList) == 0) {
        m_cutWalkThroughDesktopList = seq;
    } else if (qstrcmp(qPrintable(action->objectName()), s_desktopListRev) == 0) {
        m_cutWalkThroughDesktopListReverse = seq;
    }
}

/*!
  Sets the current mode to \a mode, either TabBoxDesktopListMode or TabBoxWindowsMode

  \sa mode()
 */
void TabBox::setMode(TabBoxMode mode)
{
    m_tabBoxMode = mode;
    switch(mode) {
    case TabBoxWindowsMode:
        m_tabBox->setConfig(m_defaultConfig);
        break;
    case TabBoxWindowsAlternativeMode:
        m_tabBox->setConfig(m_alternativeConfig);
        break;
    case TabBoxCurrentAppWindowsMode:
        m_tabBox->setConfig(m_defaultCurrentApplicationConfig);
        break;
    case TabBoxCurrentAppWindowsAlternativeMode:
        m_tabBox->setConfig(m_alternativeCurrentApplicationConfig);
        break;
    case TabBoxDesktopMode:
        m_tabBox->setConfig(m_desktopConfig);
        break;
    case TabBoxDesktopListMode:
        m_tabBox->setConfig(m_desktopListConfig);
        break;
    }
}

/*!
  Resets the tab box to display the active client in TabBoxWindowsMode, or the
  current desktop in TabBoxDesktopListMode
 */
void TabBox::reset(bool partial_reset)
{
    switch(m_tabBox->config().tabBoxMode()) {
    case TabBoxConfig::ClientTabBox:
        m_tabBox->createModel(partial_reset);
        if (!partial_reset) {
            if (Workspace::self()->activeClient())
                setCurrentClient(Workspace::self()->activeClient());
            // it's possible that the active client is not part of the model
            // in that case the index is invalid
            if (!m_tabBox->currentIndex().isValid())
                setCurrentIndex(m_tabBox->first());
        } else {
            if (!m_tabBox->currentIndex().isValid() || !m_tabBox->client(m_tabBox->currentIndex()))
                setCurrentIndex(m_tabBox->first());
        }
        break;
    case TabBoxConfig::DesktopTabBox:
        m_tabBox->createModel();

        if (!partial_reset)
            setCurrentDesktop(VirtualDesktopManager::self()->current());
        break;
    }

    emit tabBoxUpdated();
}

/*!
  Shows the next or previous item, depending on \a next
 */
void TabBox::nextPrev(bool next)
{
    setCurrentIndex(m_tabBox->nextPrev(next), false);
    emit tabBoxUpdated();
}

/*!
  Returns the currently displayed client ( only works in TabBoxWindowsMode ).
  Returns 0 if no client is displayed.
 */
Client* TabBox::currentClient()
{
    if (TabBoxClientImpl* client = static_cast< TabBoxClientImpl* >(m_tabBox->client(m_tabBox->currentIndex()))) {
        if (!Workspace::self()->hasClient(client->client()))
            return nullptr;
        return client->client();
    } else
        return nullptr;
}

/*!
  Returns the list of clients potentially displayed ( only works in
  TabBoxWindowsMode ).
  Returns an empty list if no clients are available.
 */
ClientList TabBox::currentClientList()
{
    TabBoxClientList list = m_tabBox->clientList();
    ClientList ret;
    foreach (const QWeakPointer<TabBoxClient> &clientPointer, list) {
        QSharedPointer<TabBoxClient> client = clientPointer.toStrongRef();
        if (!client)
            continue;
        if (const TabBoxClientImpl* c = static_cast< const TabBoxClientImpl* >(client.data()))
            ret.append(c->client());
    }
    return ret;
}

/*!
  Returns the currently displayed virtual desktop ( only works in
  TabBoxDesktopListMode )
  Returns -1 if no desktop is displayed.
 */
int TabBox::currentDesktop()
{
    return m_tabBox->desktop(m_tabBox->currentIndex());
}

/*!
  Returns the list of desktops potentially displayed ( only works in
  TabBoxDesktopListMode )
  Returns an empty list if no are available.
 */
QList< int > TabBox::currentDesktopList()
{
    return m_tabBox->desktopList();
}

/*!
  Change the currently selected client, and notify the effects.

  \sa setCurrentDesktop()
 */
void TabBox::setCurrentClient(Client* newClient)
{
    setCurrentIndex(m_tabBox->index(newClient->tabBoxClient()));
}

/*!
  Change the currently selected desktop, and notify the effects.

  \sa setCurrentClient()
 */
void TabBox::setCurrentDesktop(int newDesktop)
{
    setCurrentIndex(m_tabBox->desktopIndex(newDesktop));
}

void TabBox::setCurrentIndex(QModelIndex index, bool notifyEffects)
{
    if (!index.isValid())
        return;
    m_tabBox->setCurrentIndex(index);
    if (notifyEffects) {
        emit tabBoxUpdated();
    }
}

/*!
  Notify effects that the tab box is being shown, and only display the
  default tab box QFrame if no effect has referenced the tab box.
*/
void TabBox::show()
{
    emit tabBoxAdded(m_tabBoxMode);
    if (isDisplayed()) {
        m_isShown = false;
        return;
    }
    reference();
    m_isShown = true;
    m_tabBox->show();
}

/*!
  Notify effects that the tab box is being hidden.
*/
void TabBox::hide(bool abort)
{
    m_delayedShowTimer.stop();
    if (m_isShown) {
        m_isShown = false;
        unreference();
    }
    emit tabBoxClosed();
    if (isDisplayed())
        qDebug() << "Tab box was not properly closed by an effect";
    m_tabBox->hide(abort);
    Xcb::sync();
}

void TabBox::reconfigure()
{
    KSharedConfigPtr c = KSharedConfig::openConfig();
    KConfigGroup config = c->group("TabBox");

    loadConfig(c->group("TabBox"), m_defaultConfig);
    loadConfig(c->group("TabBoxAlternative"), m_alternativeConfig);

    m_defaultCurrentApplicationConfig = m_defaultConfig;
    m_defaultCurrentApplicationConfig.setClientApplicationsMode(TabBoxConfig::AllWindowsCurrentApplication);
    m_alternativeCurrentApplicationConfig = m_alternativeConfig;
    m_alternativeCurrentApplicationConfig.setClientApplicationsMode(TabBoxConfig::AllWindowsCurrentApplication);

    m_tabBox->setConfig(m_defaultConfig);

    m_delayShow = config.readEntry<bool>("ShowDelay", true);
    m_delayShowTime = config.readEntry<int>("DelayTime", 90);

    m_desktopConfig.setLayoutName(config.readEntry("DesktopLayout", "informative"));
    m_desktopListConfig.setLayoutName(config.readEntry("DesktopListLayout", "informative"));

#ifdef KWIN_BUILD_SCREENEDGES
    QList<ElectricBorder> *borders = &m_borderActivate;
    QString borderConfig = QStringLiteral("BorderActivate");
    for (int i = 0; i < 2; ++i) {
        foreach (ElectricBorder border, *borders) {
            ScreenEdges::self()->unreserve(border, this);
        }
        borders->clear();
        QStringList list = config.readEntry(borderConfig, QStringList());
        foreach (const QString &s, list) {
            bool ok;
            const int i = s.toInt(&ok);
            if (!ok)
                continue;
            borders->append(ElectricBorder(i));
            ScreenEdges::self()->reserve(ElectricBorder(i), this, "toggle");
        }
        borders = &m_borderAlternativeActivate;
        borderConfig = QStringLiteral("BorderAlternativeActivate");
    }
#endif
}

void TabBox::loadConfig(const KConfigGroup& config, TabBoxConfig& tabBoxConfig)
{
    tabBoxConfig.setClientDesktopMode(TabBoxConfig::ClientDesktopMode(
                                       config.readEntry<int>("DesktopMode", TabBoxConfig::defaultDesktopMode())));
    tabBoxConfig.setClientActivitiesMode(TabBoxConfig::ClientActivitiesMode(
                                       config.readEntry<int>("ActivitiesMode", TabBoxConfig::defaultActivitiesMode())));
    tabBoxConfig.setClientApplicationsMode(TabBoxConfig::ClientApplicationsMode(
                                       config.readEntry<int>("ApplicationsMode", TabBoxConfig::defaultApplicationsMode())));
    tabBoxConfig.setClientMinimizedMode(TabBoxConfig::ClientMinimizedMode(
                                       config.readEntry<int>("MinimizedMode", TabBoxConfig::defaultMinimizedMode())));
    tabBoxConfig.setShowDesktopMode(TabBoxConfig::ShowDesktopMode(
                                       config.readEntry<int>("ShowDesktopMode", TabBoxConfig::defaultShowDesktopMode())));
    tabBoxConfig.setClientMultiScreenMode(TabBoxConfig::ClientMultiScreenMode(
                                       config.readEntry<int>("MultiScreenMode", TabBoxConfig::defaultMultiScreenMode())));
    tabBoxConfig.setClientSwitchingMode(TabBoxConfig::ClientSwitchingMode(
                                            config.readEntry<int>("SwitchingMode", TabBoxConfig::defaultSwitchingMode())));

    tabBoxConfig.setShowTabBox(config.readEntry<bool>("ShowTabBox",
                               TabBoxConfig::defaultShowTabBox()));
    tabBoxConfig.setHighlightWindows(config.readEntry<bool>("HighlightWindows",
                                     TabBoxConfig::defaultHighlightWindow()));

    tabBoxConfig.setLayoutName(config.readEntry<QString>("LayoutName", TabBoxConfig::defaultLayoutName()));
}

/*!
  Rikkus: please document!   (Matthias)

  Ok, here's the docs :)

  You call delayedShow() instead of show() directly.

  If the 'ShowDelay' setting is false, show() is simply called.

  Otherwise, we start a timer for the delay given in the settings and only
  do a show() when it times out.

  This means that you can alt-tab between windows and you don't see the
  tab box immediately. Not only does this make alt-tabbing faster, it gives
  less 'flicker' to the eyes. You don't need to see the tab box if you're
  just quickly switching between 2 or 3 windows. It seems to work quite
  nicely.
 */
void TabBox::delayedShow()
{
    if (isDisplayed() || m_delayedShowTimer.isActive())
        // already called show - no need to call it twice
        return;

    if (!m_delayShowTime) {
        show();
        return;
    }

    m_delayedShowTimer.setSingleShot(true);
    m_delayedShowTimer.start(m_delayShowTime);
}

bool TabBox::handleMouseEvent(xcb_button_press_event_t *e)
{
    xcb_allow_events(connection(), XCB_ALLOW_ASYNC_POINTER, XCB_CURRENT_TIME);
    if (!m_isShown && isDisplayed()) {
        // tabbox has been replaced, check effects
        if (effects && static_cast<EffectsHandlerImpl*>(effects)->checkInputWindowEvent(e))
            return true;
    }
    if ((e->response_type & ~0x80) == XCB_BUTTON_PRESS) {
        // press outside Tabbox?
        QPoint pos(e->root_x, e->root_y);

        if ((!m_isShown && isDisplayed())
                || (!m_tabBox->containsPos(pos) &&
                    (e->detail == XCB_BUTTON_INDEX_1 || e->detail == XCB_BUTTON_INDEX_2 || e->detail == XCB_BUTTON_INDEX_3))) {
            close();  // click outside closes tab
            return true;
        }
        if (e->detail == XCB_BUTTON_INDEX_5 || e->detail == XCB_BUTTON_INDEX_4) {
            // mouse wheel event
            const QModelIndex index = m_tabBox->nextPrev(e->detail == XCB_BUTTON_INDEX_5);
            if (index.isValid()) {
                setCurrentIndex(index);
            }
            return true;
        }
    }
    return false;
}

bool TabBox::handleMouseEvent(xcb_motion_notify_event_t *e)
{
    xcb_allow_events(connection(), XCB_ALLOW_ASYNC_POINTER, XCB_CURRENT_TIME);
    if (!m_isShown && isDisplayed()) {
        // tabbox has been replaced, check effects
        if (effects && static_cast<EffectsHandlerImpl*>(effects)->checkInputWindowEvent(e))
            return true;
    }
    return false;
}

void TabBox::grabbedKeyEvent(QKeyEvent* event)
{
    emit tabBoxKeyEvent(event);
    if (!m_isShown && isDisplayed()) {
        // tabbox has been replaced, check effects
        return;
    }
    if (m_noModifierGrab) {
        if (event->key() == Qt::Key_Enter || event->key() == Qt::Key_Return || event->key() == Qt::Key_Space) {
            accept();
            return;
        }
    }
    m_tabBox->grabbedKeyEvent(event);
}

/*!
  Handles alt-tab / control-tab
 */
static bool areKeySymXsDepressed(bool bAll, const uint keySyms[], int nKeySyms) {
    char keymap[32];

    qDebug() << "areKeySymXsDepressed: " << (bAll ? "all of " : "any of ") << nKeySyms;

    XQueryKeymap(display(), keymap);

    for (int iKeySym = 0; iKeySym < nKeySyms; iKeySym++) {
        uint keySymX = keySyms[ iKeySym ];
        uchar keyCodeX = XKeysymToKeycode(display(), keySymX);
        int i = keyCodeX / 8;
        char mask = 1 << (keyCodeX - (i * 8));

        // Abort if bad index value,
        if (i < 0 || i >= 32)
            return false;

        qDebug()    << iKeySym << ": keySymX=0x" << QString::number(keySymX, 16)
                    << " i=" << i << " mask=0x" << QString::number(mask, 16)
                    << " keymap[i]=0x" << QString::number(keymap[i], 16) << endl;

        // If ALL keys passed need to be depressed,
        if (bAll) {
            if ((keymap[i] & mask) == 0)
                return false;
        } else {
            // If we are looking for ANY key press, and this key is depressed,
            if (keymap[i] & mask)
                return true;
        }
    }

    // If we were looking for ANY key press, then none was found, return false,
    // If we were looking for ALL key presses, then all were found, return true.
    return bAll;
}

static bool areModKeysDepressed(const QKeySequence& seq) {
    uint rgKeySyms[10];
    int nKeySyms = 0;
    if (seq.isEmpty())
        return false;
    int mod = seq[seq.count()-1] & Qt::KeyboardModifierMask;

    if (mod & Qt::SHIFT) {
        rgKeySyms[nKeySyms++] = XK_Shift_L;
        rgKeySyms[nKeySyms++] = XK_Shift_R;
    }
    if (mod & Qt::CTRL) {
        rgKeySyms[nKeySyms++] = XK_Control_L;
        rgKeySyms[nKeySyms++] = XK_Control_R;
    }
    if (mod & Qt::ALT) {
        rgKeySyms[nKeySyms++] = XK_Alt_L;
        rgKeySyms[nKeySyms++] = XK_Alt_R;
    }
    if (mod & Qt::META) {
        // It would take some code to determine whether the Win key
        // is associated with Super or Meta, so check for both.
        // See bug #140023 for details.
        rgKeySyms[nKeySyms++] = XK_Super_L;
        rgKeySyms[nKeySyms++] = XK_Super_R;
        rgKeySyms[nKeySyms++] = XK_Meta_L;
        rgKeySyms[nKeySyms++] = XK_Meta_R;
    }

    return areKeySymXsDepressed(false, rgKeySyms, nKeySyms);
}

void TabBox::navigatingThroughWindows(bool forward, const QKeySequence &shortcut, TabBoxMode mode)
{
    if (!m_ready || isGrabbed() || !Workspace::self()->isOnCurrentHead()) {
        return;
    }
    if (!options->focusPolicyIsReasonable()) {
        //ungrabXKeyboard(); // need that because of accelerator raw mode
        // CDE style raise / lower
        CDEWalkThroughWindows(forward);
    } else {
        if (areModKeysDepressed(shortcut)) {
            if (startKDEWalkThroughWindows(mode))
                KDEWalkThroughWindows(forward);
        } else
            // if the shortcut has no modifiers, don't show the tabbox,
            // don't grab, but simply go to the next window
            KDEOneStepThroughWindows(forward, mode);
    }
}

void TabBox::slotWalkThroughWindows()
{
    navigatingThroughWindows(true, m_cutWalkThroughWindows, TabBoxWindowsMode);
}

void TabBox::slotWalkBackThroughWindows()
{
    navigatingThroughWindows(false, m_cutWalkThroughWindowsReverse, TabBoxWindowsMode);
}

void TabBox::slotWalkThroughWindowsAlternative()
{
    navigatingThroughWindows(true, m_cutWalkThroughWindowsAlternative, TabBoxWindowsAlternativeMode);
}

void TabBox::slotWalkBackThroughWindowsAlternative()
{
    navigatingThroughWindows(false, m_cutWalkThroughWindowsAlternativeReverse, TabBoxWindowsAlternativeMode);
}

void TabBox::slotWalkThroughCurrentAppWindows()
{
    navigatingThroughWindows(true, m_cutWalkThroughCurrentAppWindows, TabBoxCurrentAppWindowsMode);
}

void TabBox::slotWalkBackThroughCurrentAppWindows()
{
    navigatingThroughWindows(false, m_cutWalkThroughCurrentAppWindowsReverse, TabBoxCurrentAppWindowsMode);
}

void TabBox::slotWalkThroughCurrentAppWindowsAlternative()
{
    navigatingThroughWindows(true, m_cutWalkThroughCurrentAppWindowsAlternative, TabBoxCurrentAppWindowsAlternativeMode);
}

void TabBox::slotWalkBackThroughCurrentAppWindowsAlternative()
{
    navigatingThroughWindows(false, m_cutWalkThroughCurrentAppWindowsAlternativeReverse, TabBoxCurrentAppWindowsAlternativeMode);
}

void TabBox::slotWalkThroughDesktops()
{
    if (!m_ready || isGrabbed() || !Workspace::self()->isOnCurrentHead()) {
        return;
    }
    if (areModKeysDepressed(m_cutWalkThroughDesktops)) {
        if (startWalkThroughDesktops())
            walkThroughDesktops(true);
    } else {
        oneStepThroughDesktops(true);
    }
}

void TabBox::slotWalkBackThroughDesktops()
{
    if (!m_ready || isGrabbed() || !Workspace::self()->isOnCurrentHead()) {
        return;
    }
    if (areModKeysDepressed(m_cutWalkThroughDesktopsReverse)) {
        if (startWalkThroughDesktops())
            walkThroughDesktops(false);
    } else {
        oneStepThroughDesktops(false);
    }
}

void TabBox::slotWalkThroughDesktopList()
{
    if (!m_ready || isGrabbed() || !Workspace::self()->isOnCurrentHead()) {
        return;
    }
    if (areModKeysDepressed(m_cutWalkThroughDesktopList)) {
        if (startWalkThroughDesktopList())
            walkThroughDesktops(true);
    } else {
        oneStepThroughDesktopList(true);
    }
}

void TabBox::slotWalkBackThroughDesktopList()
{
    if (!m_ready || isGrabbed() || !Workspace::self()->isOnCurrentHead()) {
        return;
    }
    if (areModKeysDepressed(m_cutWalkThroughDesktopListReverse)) {
        if (startWalkThroughDesktopList())
            walkThroughDesktops(false);
    } else {
        oneStepThroughDesktopList(false);
    }
}

bool TabBox::toggle(ElectricBorder eb)
{
    if (!options->focusPolicyIsReasonable())
        return false; // not supported.
    if (isDisplayed()) {
        ungrabXKeyboard();
        accept();
        return true;
    }
    if (!grabXKeyboard())
        return false;
    m_noModifierGrab = m_tabGrab = true;
    if (m_borderAlternativeActivate.contains(eb))
        setMode(TabBoxWindowsAlternativeMode);
    else
        setMode(TabBoxWindowsMode);
    reset();
    show();
    return true;
}

bool TabBox::startKDEWalkThroughWindows(TabBoxMode mode)
{
    if (!establishTabBoxGrab())
        return false;
    m_tabGrab = true;
    m_noModifierGrab = false;
    setMode(mode);
    reset();
    return true;
}

bool TabBox::startWalkThroughDesktops(TabBoxMode mode)
{
    if (!establishTabBoxGrab())
        return false;
    m_desktopGrab = true;
    m_noModifierGrab = false;
    setMode(mode);
    reset();
    return true;
}

bool TabBox::startWalkThroughDesktops()
{
    return startWalkThroughDesktops(TabBoxDesktopMode);
}

bool TabBox::startWalkThroughDesktopList()
{
    return startWalkThroughDesktops(TabBoxDesktopListMode);
}

void TabBox::KDEWalkThroughWindows(bool forward)
{
    nextPrev(forward);
    delayedShow();
}

void TabBox::walkThroughDesktops(bool forward)
{
    nextPrev(forward);
    delayedShow();
}

void TabBox::CDEWalkThroughWindows(bool forward)
{
    Client* c = nullptr;
// this function find the first suitable client for unreasonable focus
// policies - the topmost one, with some exceptions (can't be keepabove/below,
// otherwise it gets stuck on them)
//     Q_ASSERT(Workspace::self()->block_stacking_updates == 0);
    for (int i = Workspace::self()->stackingOrder().size() - 1;
            i >= 0 ;
            --i) {
        Client* it = qobject_cast<Client*>(Workspace::self()->stackingOrder().at(i));
        if (it && it->isOnCurrentActivity() && it->isOnCurrentDesktop() && !it->isSpecialWindow()
                && it->isShown(false) && it->wantsTabFocus()
                && !it->keepAbove() && !it->keepBelow()) {
            c = it;
            break;
        }
    }
    Client* nc = c;
    bool options_traverse_all;
    {
        KConfigGroup group(KSharedConfig::openConfig(), "TabBox");
        options_traverse_all = group.readEntry("TraverseAll", false);
    }

    Client* firstClient = nullptr;
    do {
        nc = forward ? nextClientStatic(nc) : previousClientStatic(nc);
        if (!firstClient) {
            // When we see our first client for the second time,
            // it's time to stop.
            firstClient = nc;
        } else if (nc == firstClient) {
            // No candidates found.
            nc = nullptr;
            break;
        }
    } while (nc && nc != c &&
            ((!options_traverse_all && !nc->isOnDesktop(currentDesktop())) ||
             nc->isMinimized() || !nc->wantsTabFocus() || nc->keepAbove() || nc->keepBelow() || !nc->isOnCurrentActivity()));
    if (nc) {
        if (c && c != nc)
            Workspace::self()->lowerClient(c);
        if (options->focusPolicyIsReasonable()) {
            Workspace::self()->activateClient(nc);
            if (nc->isShade() && options->isShadeHover())
                nc->setShade(ShadeActivated);
        } else {
            if (!nc->isOnDesktop(currentDesktop()))
                setCurrentDesktop(nc->desktop());
            Workspace::self()->raiseClient(nc);
        }
    }
}

void TabBox::KDEOneStepThroughWindows(bool forward, TabBoxMode mode)
{
    setMode(mode);
    reset();
    nextPrev(forward);
    if (Client* c = currentClient()) {
        Workspace::self()->activateClient(c);
        if (c->isShade() && options->isShadeHover())
            c->setShade(ShadeActivated);
    }
}

void TabBox::oneStepThroughDesktops(bool forward, TabBoxMode mode)
{
    setMode(mode);
    reset();
    nextPrev(forward);
    if (currentDesktop() != -1)
        setCurrentDesktop(currentDesktop());
}

void TabBox::oneStepThroughDesktops(bool forward)
{
    oneStepThroughDesktops(forward, TabBoxDesktopMode);
}

void TabBox::oneStepThroughDesktopList(bool forward)
{
    oneStepThroughDesktops(forward, TabBoxDesktopListMode);
}

/*!
  Handles holding alt-tab / control-tab
 */
void TabBox::keyPress(int keyQt)
{
    bool forward = false;
    bool backward = false;

    auto contains = [](const QKeySequence &shortcut, int key) -> bool {
        for (int i = 0; i < shortcut.count(); ++i) {
            if (shortcut[i] == key) {
                return true;
            }
        }
        return false;
    };
    auto testBacktab = [&forward,&backward,keyQt,contains](const QKeySequence &forwardShortcut, const QKeySequence &backwardShortcut) {
        if (forward || backward || !(keyQt & Qt::ShiftModifier)) {
            return;
        }
        // a shortcut containing Shift+Tab will not fire as it is registered as Alt+Backtab
        // the keyQt we get does not contain the backtab, so we need to convert it
        // extract the modifiers, tests whether the key is tab and create a new test shortcut
        // containing the extracted modifiers and backtab instead of tab
        Qt::KeyboardModifiers mods = Qt::ShiftModifier;
        auto testMod = [&mods,keyQt](Qt::KeyboardModifier modifier) {
            if (keyQt & modifier) {
                mods |= modifier;
            }
        };
        // unfortunately we have to test each of the modifiers and cannot just and with ~NoModifier
        // we don't need the shift test as we already know that shift is hold
        testMod(Qt::ControlModifier);
        testMod(Qt::AltModifier);
        testMod(Qt::MetaModifier);
        testMod(Qt::KeypadModifier);
        testMod(Qt::GroupSwitchModifier);
        // Do not AND with Key_Tab! Key_Tab is 0x01000001 which would also return true for
        // Qt::Key_Backspace which is 0x01000003 and a whole bunch of other keys!
        if ((keyQt & ~mods) == Qt::Key_Tab) {
            forward = contains(forwardShortcut, mods | Qt::Key_Backtab);
            backward = contains(backwardShortcut, mods | Qt::Key_Backtab);
        }
    };

    if (m_tabGrab) {
        QKeySequence forwardShortcut;
        QKeySequence backwardShortcut;
        switch (mode()) {
            case TabBoxWindowsMode:
                forwardShortcut = m_cutWalkThroughWindows;
                backwardShortcut = m_cutWalkThroughWindowsReverse;
                break;
            case TabBoxWindowsAlternativeMode:
                forwardShortcut = m_cutWalkThroughWindowsAlternative;
                backwardShortcut = m_cutWalkThroughWindowsAlternativeReverse;
                break;
            case TabBoxCurrentAppWindowsMode:
                forwardShortcut = m_cutWalkThroughCurrentAppWindows;
                backwardShortcut = m_cutWalkThroughCurrentAppWindowsReverse;
                break;
            case TabBoxCurrentAppWindowsAlternativeMode:
                forwardShortcut = m_cutWalkThroughCurrentAppWindowsAlternative;
                backwardShortcut = m_cutWalkThroughCurrentAppWindowsAlternativeReverse;
                break;
            default:
                qDebug() << "Invalid TabBoxMode";
                return;
        }
        forward = contains(forwardShortcut, keyQt);
        backward = contains(backwardShortcut, keyQt);
        testBacktab(forwardShortcut, backwardShortcut);
        if ((keyQt & Qt::ShiftModifier) && !(forward || backward)) {
            // if the shortcuts do not match, try matching again after filtering the shift key from keyQt
            // it is needed to handle correctly the ALT+~ shorcut for example as it is coded as ALT+SHIFT+~ in keyQt
            keyQt &= ~Qt::ShiftModifier;
            forward = contains(forwardShortcut, keyQt);
            backward = contains(backwardShortcut, keyQt);
            if (!(forward || backward)) {
                // the tabkey is however overly special and withdrawing the shift state will not turn backtab into tab
                // yet kglobalaccel fires for both. since we ensure this is in a Shift condition, try the other key
                // TODO: Check requirement regarding Qt5

                // NOTICE: it is very important to restore the Shift modifier, since otherwise we can't distiguish
                // between the regular alt+tab / alt+shift+tab anymore!!
                if ((keyQt & Qt::Key_Backtab) == Qt::Key_Backtab) {// regular case
                    keyQt &= ~Qt::Key_Backtab;
                    keyQt |= (Qt::Key_Tab|Qt::ShiftModifier);
                } else if ((keyQt & Qt::Key_Tab) == Qt::Key_Tab) { // just to be very sure ... :-(
                    keyQt &= ~Qt::Key_Tab;
                    keyQt |= (Qt::Key_Backtab|Qt::ShiftModifier);
                }
                forward = contains(forwardShortcut, keyQt);
                backward = contains(backwardShortcut, keyQt);
            }
        }
        if (forward || backward) {
            qDebug() << "== " << forwardShortcut.toString()
                        << " or " << backwardShortcut.toString() << endl;
            KDEWalkThroughWindows(forward);
        }
    } else if (m_desktopGrab) {
        forward = contains(m_cutWalkThroughDesktops, keyQt) ||
                  contains(m_cutWalkThroughDesktopList, keyQt);
        backward = contains(m_cutWalkThroughDesktopsReverse, keyQt) ||
                   contains(m_cutWalkThroughDesktopListReverse, keyQt);
        testBacktab(m_cutWalkThroughDesktops, m_cutWalkThroughDesktopsReverse);
        testBacktab(m_cutWalkThroughDesktopList, m_cutWalkThroughDesktopListReverse);
        if ((keyQt & Qt::ShiftModifier) && !(forward || backward)) {
            // if the shortcuts do not match, try matching again after filtering the shift key from keyQt
            // it is needed to handle correctly the ALT+~ shorcut for example as it is coded as ALT+SHIFT+~ in keyQt
            keyQt &= ~Qt::ShiftModifier;
            forward = contains(m_cutWalkThroughDesktops, keyQt) ||
                  contains(m_cutWalkThroughDesktopList, keyQt);
            backward = contains(m_cutWalkThroughDesktopsReverse, keyQt) ||
                   contains(m_cutWalkThroughDesktopListReverse, keyQt);
            if (!(forward || backward)) {
                // the tabkey is however overly special and withdrawing the shift state will not turn backtab into tab
                // yet kglobalaccel fires for both. since we ensure this is in a Shift condition, try the other key
                // TODO: Check requirement regarding Qt5

                // NOTICE: it is very important to restore the Shift modifier, since otherwise we can't distiguish
                // between the regular alt+tab / alt+shift+tab anymore!!
                if ((keyQt & Qt::Key_Backtab) == Qt::Key_Backtab) {// regular case
                    keyQt &= ~Qt::Key_Backtab;
                    keyQt |= (Qt::Key_Tab|Qt::ShiftModifier);
                } else if ((keyQt & Qt::Key_Tab) == Qt::Key_Tab) { // just to be very sure ... :-(
                    keyQt &= ~Qt::Key_Tab;
                    keyQt |= (Qt::Key_Backtab|Qt::ShiftModifier);
                }
                forward = contains(m_cutWalkThroughDesktops, keyQt) ||
                  contains(m_cutWalkThroughDesktopList, keyQt);
                backward = contains(m_cutWalkThroughDesktopsReverse, keyQt) ||
                   contains(m_cutWalkThroughDesktopListReverse, keyQt);
            }
        }
        if (forward || backward)
            walkThroughDesktops(forward);
    }

    if (m_desktopGrab || m_tabGrab) {
        if (((keyQt & ~Qt::KeyboardModifierMask) == Qt::Key_Escape)
                && !(forward || backward)) {
            // if Escape is part of the shortcut, don't cancel
            close(true);
        } else if (!(forward || backward)) {
            QKeyEvent* event = new QKeyEvent(QEvent::KeyPress, keyQt & ~Qt::KeyboardModifierMask, Qt::NoModifier);
            grabbedKeyEvent(event);
        }
    }
}

void TabBox::close(bool abort)
{
    if (isGrabbed()) {
        removeTabBoxGrab();
    }
    hide(abort);
    m_tabGrab = false;
    m_desktopGrab = false;
    m_noModifierGrab = false;
}

void TabBox::accept()
{
    Client* c = currentClient();
    close();
    if (c) {
        Workspace::self()->activateClient(c);
        if (c->isShade() && options->isShadeHover())
            c->setShade(ShadeActivated);
        if (c->isDesktop())
            Workspace::self()->setShowingDesktop(!Workspace::self()->showingDesktop());
    }
}

/*!
  Handles alt-tab / control-tab releasing
 */
void TabBox::keyRelease(const xcb_key_release_event_t *ev)
{
    if (m_noModifierGrab) {
        return;
    }
    unsigned int mk = ev->state &
                      (KKeyServer::modXShift() |
                       KKeyServer::modXCtrl() |
                       KKeyServer::modXAlt() |
                       KKeyServer::modXMeta());
    // ev.state is state before the key release, so just checking mk being 0 isn't enough
    // using XQueryPointer() also doesn't seem to work well, so the check that all
    // modifiers are released: only one modifier is active and the currently released
    // key is this modifier - if yes, release the grab
    int mod_index = -1;
    for (int i = XCB_MAP_INDEX_SHIFT;
            i <= XCB_MAP_INDEX_5;
            ++i)
        if ((mk & (1 << i)) != 0) {
            if (mod_index >= 0)
                return;
            mod_index = i;
        }
    bool release = false;
    if (mod_index == -1)
        release = true;
    else {
        XModifierKeymap* xmk = XGetModifierMapping(display());
        for (int i = 0; i < xmk->max_keypermod; i++)
            if (xmk->modifiermap[xmk->max_keypermod * mod_index + i]
                    == ev->detail)
                release = true;
        XFreeModifiermap(xmk);
    }
    if (!release)
        return;
    if (m_tabGrab) {
        bool old_control_grab = m_desktopGrab;
        accept();
        m_desktopGrab = old_control_grab;
    }
    if (m_desktopGrab) {
        bool old_tab_grab = m_tabGrab;
        int desktop = currentDesktop();
        close();
        m_tabGrab = old_tab_grab;
        if (desktop != -1) {
            setCurrentDesktop(desktop);
            VirtualDesktopManager::self()->setCurrent(desktop);
        }
    }
}

void TabBox::modifiersChanged(Qt::KeyboardModifiers mods)
{
    if (m_noModifierGrab || !(!mods)) {
        return;
    }
    if (m_tabGrab) {
        bool old_control_grab = m_desktopGrab;
        accept();
        m_desktopGrab = old_control_grab;
    }
    if (m_desktopGrab) {
        bool old_tab_grab = m_tabGrab;
        int desktop = currentDesktop();
        close();
        m_tabGrab = old_tab_grab;
        if (desktop != -1) {
            setCurrentDesktop(desktop);
            VirtualDesktopManager::self()->setCurrent(desktop);
        }
    }
}

int TabBox::nextDesktopStatic(int iDesktop) const
{
    DesktopNext functor;
    return functor(iDesktop, true);
}

int TabBox::previousDesktopStatic(int iDesktop) const
{
    DesktopPrevious functor;
    return functor(iDesktop, true);
}

/*!
  auxiliary functions to travers all clients according to the static
  order. Useful for the CDE-style Alt-tab feature.
*/
Client* TabBox::nextClientStatic(Client* c) const
{
    if (!c || Workspace::self()->clientList().isEmpty())
        return 0;
    int pos = Workspace::self()->clientList().indexOf(c);
    if (pos == -1)
        return Workspace::self()->clientList().first();
    ++pos;
    if (pos == Workspace::self()->clientList().count())
        return Workspace::self()->clientList().first();
    return Workspace::self()->clientList()[ pos ];
}

/*!
  auxiliary functions to travers all clients according to the static
  order. Useful for the CDE-style Alt-tab feature.
*/
Client* TabBox::previousClientStatic(Client* c) const
{
    if (!c || Workspace::self()->clientList().isEmpty())
        return 0;
    int pos = Workspace::self()->clientList().indexOf(c);
    if (pos == -1)
        return Workspace::self()->clientList().last();
    if (pos == 0)
        return Workspace::self()->clientList().last();
    --pos;
    return Workspace::self()->clientList()[ pos ];
}

bool TabBox::establishTabBoxGrab()
{
    updateXTime();
    if (!grabXKeyboard())
        return false;
    // Don't try to establish a global mouse grab using XGrabPointer, as that would prevent
    // using Alt+Tab while DND (#44972). However force passive grabs on all windows
    // in order to catch MouseRelease events and close the tabbox (#67416).
    // All clients already have passive grabs in their wrapper windows, so check only
    // the active client, which may not have it.
    assert(!m_forcedGlobalMouseGrab);
    m_forcedGlobalMouseGrab = true;
    if (Workspace::self()->activeClient() != nullptr)
        Workspace::self()->activeClient()->updateMouseGrab();
    return true;
}

void TabBox::removeTabBoxGrab()
{
    updateXTime();
    ungrabXKeyboard();
    assert(m_forcedGlobalMouseGrab);
    m_forcedGlobalMouseGrab = false;
    if (Workspace::self()->activeClient() != nullptr)
        Workspace::self()->activeClient()->updateMouseGrab();
}
} // namespace TabBox
} // namespace

#include "tabbox.moc"
