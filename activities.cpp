/********************************************************************
 KWin - the KDE window manager
 This file is part of the KDE project.

Copyright (C) 2013 Martin Gräßlin <mgraesslin@kde.org>

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
#include "activities.h"
// KWin
#include "client.h"
#include "workspace.h"
// KDE
#include <KConfigGroup>
#include <kactivities/controller.h>
// Qt
#include <QtConcurrentRun>
#include <QDBusInterface>
#include <QDBusPendingCall>
#include <QFutureWatcher>

namespace KWin
{

KWIN_SINGLETON_FACTORY(Activities)

Activities::Activities(QObject *parent)
    : QObject(parent)
    , m_controller(new KActivities::Controller(this))
{
    connect(m_controller, &KActivities::Controller::activityRemoved, this, &Activities::slotRemoved);
    connect(m_controller, &KActivities::Controller::activityRemoved, this, &Activities::removed);
    connect(m_controller, &KActivities::Controller::activityAdded,   this, &Activities::added);
    connect(m_controller, &KActivities::Controller::currentActivityChanged, this, &Activities::slotCurrentChanged);
}

Activities::~Activities()
{
    s_self = NULL;
}

void Activities::setCurrent(const QString &activity)
{
    m_controller->setCurrentActivity(activity);
}

void Activities::slotCurrentChanged(const QString &newActivity)
{
    if (m_current == newActivity) {
        return;
    }
    m_previous = m_current;
    m_current = newActivity;
    emit currentChanged(newActivity);
}

void Activities::slotRemoved(const QString &activity)
{
    foreach (Client * client, Workspace::self()->clientList()) {
        client->setOnActivity(activity, false);
    }
    //toss out any session data for it
    KConfigGroup cg(KSharedConfig::openConfig(), QString("SubSession: ") + activity);
    cg.deleteGroup();
}

void Activities::toggleClientOnActivity(Client* c, const QString &activity, bool dont_activate)
{
    //int old_desktop = c->desktop();
    bool was_on_activity = c->isOnActivity(activity);
    bool was_on_all = c->isOnAllActivities();
    //note: all activities === no activities
    bool enable = was_on_all || !was_on_activity;
    c->setOnActivity(activity, enable);
    if (c->isOnActivity(activity) == was_on_activity && c->isOnAllActivities() == was_on_all)   // No change
        return;

    Workspace *ws = Workspace::self();
    if (c->isOnCurrentActivity()) {
        if (c->wantsTabFocus() && options->focusPolicyIsReasonable() &&
                !was_on_activity && // for stickyness changes
                //FIXME not sure if the line above refers to the correct activity
                !dont_activate)
            ws->requestFocus(c);
        else
            ws->restackClientUnderActive(c);
    } else
        ws->raiseClient(c);

    //notifyWindowDesktopChanged( c, old_desktop );

    ClientList transients_stacking_order = ws->ensureStackingOrder(c->transients());
    for (ClientList::ConstIterator it = transients_stacking_order.constBegin();
            it != transients_stacking_order.constEnd();
            ++it)
        toggleClientOnActivity(*it, activity, dont_activate);
    ws->updateClientArea();
}

bool Activities::start(const QString &id)
{
    Workspace *ws = Workspace::self();
    if (ws->sessionSaving()) {
        return false; //ksmserver doesn't queue requests (yet)
    }

    if (!all().contains(id)) {
        return false; //bogus id
    }

    ws->loadSubSessionInfo(id);

    QDBusInterface ksmserver("org.kde.ksmserver", "/KSMServer", "org.kde.KSMServerInterface");
    if (ksmserver.isValid()) {
        ksmserver.asyncCall("restoreSubSession", id);
    } else {
        qDebug() << "couldn't get ksmserver interface";
        return false;
    }
    return true;
}

bool Activities::stop(const QString &id)
{
    if (Workspace::self()->sessionSaving()) {
        return false; //ksmserver doesn't queue requests (yet)
        //FIXME what about session *loading*?
    }

    //ugly hack to avoid dbus deadlocks
    QMetaObject::invokeMethod(this, "reallyStop", Qt::QueuedConnection, Q_ARG(QString, id));
    //then lie and assume it worked.
    return true;
}

void Activities::reallyStop(const QString &id)
{
    Workspace *ws = Workspace::self();
    if (ws->sessionSaving())
        return; //ksmserver doesn't queue requests (yet)

    qDebug() << id;

    QSet<QByteArray> saveSessionIds;
    QSet<QByteArray> dontCloseSessionIds;
    const ClientList &clients = ws->clientList();
    for (ClientList::const_iterator it = clients.constBegin(); it != clients.constEnd(); ++it) {
        const Client* c = (*it);
        const QByteArray sessionId = c->sessionId();
        if (sessionId.isEmpty()) {
            continue; //TODO support old wm_command apps too?
        }

        //qDebug() << sessionId;

        //if it's on the activity that's closing, it needs saving
        //but if a process is on some other open activity, I don't wanna close it yet
        //this is, of course, complicated by a process having many windows.
        if (c->isOnAllActivities()) {
            dontCloseSessionIds << sessionId;
            continue;
        }

        const QStringList activities = c->activities();
        foreach (const QString & activityId, activities) {
            if (activityId == id) {
                saveSessionIds << sessionId;
            } else if (running().contains(activityId)) {
                dontCloseSessionIds << sessionId;
            }
        }
    }

    ws->storeSubSession(id, saveSessionIds);

    QStringList saveAndClose;
    QStringList saveOnly;
    foreach (const QByteArray & sessionId, saveSessionIds) {
        if (dontCloseSessionIds.contains(sessionId)) {
            saveOnly << sessionId;
        } else {
            saveAndClose << sessionId;
        }
    }

    qDebug() << "saveActivity" << id << saveAndClose << saveOnly;

    //pass off to ksmserver
    QDBusInterface ksmserver("org.kde.ksmserver", "/KSMServer", "org.kde.KSMServerInterface");
    if (ksmserver.isValid()) {
        ksmserver.asyncCall("saveSubSession", id, saveAndClose, saveOnly);
    } else {
        qDebug() << "couldn't get ksmserver interface";
    }
}

} // namespace
