/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2013 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
// own
#include "globalshortcuts.h"
// config
#include <config-kwin.h>
// kwin
#include "gestures.h"
#include "kwinglobals.h"
#include "main.h"
#include "utils/common.h"
// KDE
#include <KGlobalAccel/private/kglobalaccel_interface.h>
#include <KGlobalAccel/private/kglobalacceld.h>
// Qt
#include <QAction>
// system
#include <signal.h>
#include <variant>

namespace KWin
{
GlobalShortcut::GlobalShortcut(Shortcut &&sc, QAction *action)
    : m_shortcut(sc)
    , m_action(action)
{
    static const QMap<SwipeDirection, SwipeGesture::Direction> swipeDirs = {
        {SwipeDirection::Up, SwipeGesture::Direction::Up},
        {SwipeDirection::Down, SwipeGesture::Direction::Down},
        {SwipeDirection::Left, SwipeGesture::Direction::Left},
        {SwipeDirection::Right, SwipeGesture::Direction::Right},
    };
    static const QMap<PinchDirection, PinchGesture::Direction> pinchDirs = {
        {PinchDirection::Expanding, PinchGesture::Direction::Expanding},
        {PinchDirection::Contracting, PinchGesture::Direction::Contracting}};
    if (auto swipeGesture = std::get_if<SwipeShortcut>(&sc)) {
        m_swipeGesture.reset(new SwipeGesture());
        m_swipeGesture->setDirection(swipeDirs[swipeGesture->direction]);
        m_swipeGesture->setMaximumFingerCount(swipeGesture->fingerCount);
        m_swipeGesture->setMinimumFingerCount(swipeGesture->fingerCount);
        QObject::connect(m_swipeGesture.get(), &SwipeGesture::triggered, m_action, &QAction::trigger, Qt::QueuedConnection);
    } else if (auto rtSwipeGesture = std::get_if<RealtimeFeedbackSwipeShortcut>(&sc)) {
        m_swipeGesture.reset(new SwipeGesture());
        m_swipeGesture->setDirection(swipeDirs[rtSwipeGesture->direction]);
        m_swipeGesture->setMinimumDelta(QSizeF(200, 200));
        m_swipeGesture->setMaximumFingerCount(rtSwipeGesture->fingerCount);
        m_swipeGesture->setMinimumFingerCount(rtSwipeGesture->fingerCount);
        QObject::connect(m_swipeGesture.get(), &SwipeGesture::triggered, m_action, &QAction::trigger, Qt::QueuedConnection);
        QObject::connect(m_swipeGesture.get(), &SwipeGesture::cancelled, m_action, &QAction::trigger, Qt::QueuedConnection);
        QObject::connect(m_swipeGesture.get(), &SwipeGesture::progress, [cb = rtSwipeGesture->progressCallback](qreal v) {
            cb(v);
        });
    } else if (auto pinchGesture = std::get_if<PinchShortcut>(&sc)) {
        m_pinchGesture.reset(new PinchGesture());
        m_pinchGesture->setDirection(pinchDirs[pinchGesture->direction]);
        m_pinchGesture->setMaximumFingerCount(pinchGesture->fingerCount);
        m_pinchGesture->setMinimumFingerCount(pinchGesture->fingerCount);
        QObject::connect(m_pinchGesture.get(), &PinchGesture::triggered, m_action, &QAction::trigger, Qt::QueuedConnection);
    } else if (auto rtPinchGesture = std::get_if<RealtimeFeedbackPinchShortcut>(&sc)) {
        m_pinchGesture.reset(new PinchGesture());
        m_pinchGesture->setDirection(pinchDirs[rtPinchGesture->direction]);
        m_pinchGesture->setMaximumFingerCount(rtPinchGesture->fingerCount);
        m_pinchGesture->setMinimumFingerCount(rtPinchGesture->fingerCount);
        QObject::connect(m_pinchGesture.get(), &PinchGesture::triggered, m_action, &QAction::trigger, Qt::QueuedConnection);
        QObject::connect(m_pinchGesture.get(), &PinchGesture::cancelled, m_action, &QAction::trigger, Qt::QueuedConnection);
        QObject::connect(m_pinchGesture.get(), &PinchGesture::progress, [cb = rtPinchGesture->scaleCallback](qreal v) {
            cb(v);
        });
    }
}

GlobalShortcut::~GlobalShortcut()
{
}

QAction *GlobalShortcut::action() const
{
    return m_action;
}

void GlobalShortcut::invoke() const
{
    QMetaObject::invokeMethod(m_action, "trigger", Qt::QueuedConnection);
}

const Shortcut &GlobalShortcut::shortcut() const
{
    return m_shortcut;
}

SwipeGesture *GlobalShortcut::swipeGesture() const
{
    return m_swipeGesture.get();
}

PinchGesture *GlobalShortcut::pinchGesture() const
{
    return m_pinchGesture.get();
}

GlobalShortcutsManager::GlobalShortcutsManager(QObject *parent)
    : QObject(parent)
    , m_gestureRecognizer(new GestureRecognizer(this))
{
}

GlobalShortcutsManager::~GlobalShortcutsManager()
{
}

void GlobalShortcutsManager::init()
{
    if (kwinApp()->shouldUseWaylandForCompositing()) {
        qputenv("KGLOBALACCELD_PLATFORM", QByteArrayLiteral("org.kde.kwin"));
        m_kglobalAccel = new KGlobalAccelD(this);
        if (!m_kglobalAccel->init()) {
            qCDebug(KWIN_CORE) << "Init of kglobalaccel failed";
            delete m_kglobalAccel;
            m_kglobalAccel = nullptr;
        } else {
            qCDebug(KWIN_CORE) << "KGlobalAcceld inited";
        }
    }
}

void GlobalShortcutsManager::objectDeleted(QObject *object)
{
    auto it = m_shortcuts.begin();
    while (it != m_shortcuts.end()) {
        if (it->action() == object) {
            it = m_shortcuts.erase(it);
        } else {
            ++it;
        }
    }
}

bool GlobalShortcutsManager::addIfNotExists(GlobalShortcut sc)
{
    for (const auto &cs : qAsConst(m_shortcuts)) {
        if (sc.shortcut() == cs.shortcut()) {
            return false;
        }
    }

    if (std::holds_alternative<SwipeShortcut>(sc.shortcut()) || std::holds_alternative<RealtimeFeedbackSwipeShortcut>(sc.shortcut())) {
        m_gestureRecognizer->registerSwipeGesture(sc.swipeGesture());
    } else if (std::holds_alternative<PinchShortcut>(sc.shortcut()) || std::holds_alternative<RealtimeFeedbackPinchShortcut>(sc.shortcut())) {
        m_gestureRecognizer->registerPinchGesture(sc.pinchGesture());
    }
    connect(sc.action(), &QAction::destroyed, this, &GlobalShortcutsManager::objectDeleted);
    m_shortcuts.push_back(std::move(sc));
    return true;
}

void GlobalShortcutsManager::registerPointerShortcut(QAction *action, Qt::KeyboardModifiers modifiers, Qt::MouseButtons pointerButtons)
{
    addIfNotExists(GlobalShortcut(PointerButtonShortcut{modifiers, pointerButtons}, action));
}

void GlobalShortcutsManager::registerAxisShortcut(QAction *action, Qt::KeyboardModifiers modifiers, PointerAxisDirection axis)
{
    addIfNotExists(GlobalShortcut(PointerAxisShortcut{modifiers, axis}, action));
}

void GlobalShortcutsManager::registerTouchpadSwipe(QAction *action, SwipeDirection direction, uint fingerCount)
{
    addIfNotExists(GlobalShortcut(SwipeShortcut{direction, fingerCount}, action));
}

void GlobalShortcutsManager::registerRealtimeTouchpadSwipe(QAction *action, std::function<void(qreal)> progressCallback, SwipeDirection direction, uint fingerCount)
{
    addIfNotExists(GlobalShortcut(RealtimeFeedbackSwipeShortcut{direction, progressCallback, fingerCount}, action));
}

void GlobalShortcutsManager::registerTouchpadPinch(QAction *action, PinchDirection direction, uint fingerCount)
{
    addIfNotExists(GlobalShortcut(PinchShortcut{direction, fingerCount}, action));
}

void GlobalShortcutsManager::registerRealtimeTouchpadPinch(QAction *onUp, std::function<void(qreal)> progressCallback, PinchDirection direction, uint fingerCount)
{
    addIfNotExists(GlobalShortcut(RealtimeFeedbackPinchShortcut{direction, progressCallback, fingerCount}, onUp));
}

bool GlobalShortcutsManager::processKey(Qt::KeyboardModifiers mods, int keyQt)
{
    if (m_kglobalAccelInterface) {
        if (!keyQt && !mods) {
            return false;
        }
        auto check = [this](Qt::KeyboardModifiers mods, int keyQt) {
            bool retVal = false;
            QMetaObject::invokeMethod(m_kglobalAccelInterface,
                                      "checkKeyPressed",
                                      Qt::DirectConnection,
                                      Q_RETURN_ARG(bool, retVal),
                                      Q_ARG(int, int(mods) | keyQt));
            return retVal;
        };
        if (check(mods, keyQt)) {
            return true;
        } else if (keyQt == Qt::Key_Backtab) {
            // KGlobalAccel on X11 has some workaround for Backtab
            // see kglobalaccel/src/runtime/plugins/xcb/kglobalccel_x11.cpp method x11KeyPress
            // Apparently KKeySequenceWidget captures Shift+Tab instead of Backtab
            // thus if the key is backtab we should adjust to add shift again and use tab
            // in addition KWin registers the shortcut incorrectly as Alt+Shift+Backtab
            // this should be changed to either Alt+Backtab or Alt+Shift+Tab to match KKeySequenceWidget
            // trying the variants
            if (check(mods | Qt::ShiftModifier, keyQt)) {
                return true;
            }
            if (check(mods | Qt::ShiftModifier, Qt::Key_Tab)) {
                return true;
            }
        }
    }
    return false;
}

template<typename ShortcutKind, typename... Args>
bool match(QVector<GlobalShortcut> &shortcuts, Args... args)
{
    for (auto &sc : shortcuts) {
        if (std::holds_alternative<ShortcutKind>(sc.shortcut())) {
            if (std::get<ShortcutKind>(sc.shortcut()) == ShortcutKind{args...}) {
                sc.invoke();
                return true;
            }
        }
    }
    return false;
}

// TODO(C++20): use ranges for a nicer way of filtering by shortcut type
bool GlobalShortcutsManager::processPointerPressed(Qt::KeyboardModifiers mods, Qt::MouseButtons pointerButtons)
{
    return match<PointerButtonShortcut>(m_shortcuts, mods, pointerButtons);
}

bool GlobalShortcutsManager::processAxis(Qt::KeyboardModifiers mods, PointerAxisDirection axis)
{
    return match<PointerAxisShortcut>(m_shortcuts, mods, axis);
}

void GlobalShortcutsManager::processSwipeStart(uint fingerCount)
{
    m_gestureRecognizer->startSwipeGesture(fingerCount);
}

void GlobalShortcutsManager::processSwipeUpdate(const QSizeF &delta)
{
    m_gestureRecognizer->updateSwipeGesture(delta);
}

void GlobalShortcutsManager::processSwipeCancel()
{
    m_gestureRecognizer->cancelSwipeGesture();
}

void GlobalShortcutsManager::processSwipeEnd()
{
    m_gestureRecognizer->endSwipeGesture();
    // TODO: cancel on Wayland Seat if one triggered
}

void GlobalShortcutsManager::processPinchStart(uint fingerCount)
{
    m_gestureRecognizer->startPinchGesture(fingerCount);
}

void GlobalShortcutsManager::processPinchUpdate(qreal scale, qreal angleDelta, const QSizeF &delta)
{
    m_gestureRecognizer->updatePinchGesture(scale, angleDelta, delta);
}

void GlobalShortcutsManager::processPinchCancel()
{
    m_gestureRecognizer->cancelPinchGesture();
}

void GlobalShortcutsManager::processPinchEnd()
{
    m_gestureRecognizer->endPinchGesture();
}

} // namespace
