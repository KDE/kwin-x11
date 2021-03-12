/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2013, 2016 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "keyboard_input.h"
#include "input_event.h"
#include "input_event_spy.h"
#include "keyboard_layout.h"
#include "keyboard_repeat.h"
#include "abstract_client.h"
#include "modifier_only_shortcuts.h"
#include "utils.h"
#include "screenlockerwatcher.h"
#include "toplevel.h"
#include "wayland_server.h"
#include "workspace.h"
// KWayland
#include <KWaylandServer/datadevice_interface.h>
#include <KWaylandServer/seat_interface.h>
//screenlocker
#include <KScreenLocker/KsldApp>
// Frameworks
#include <KGlobalAccel>
// Qt
#include <QKeyEvent>

namespace KWin
{

KeyboardInputRedirection::KeyboardInputRedirection(InputRedirection *parent)
    : QObject(parent)
    , m_input(parent)
    , m_xkb(new Xkb(parent))
{
    connect(m_xkb.data(), &Xkb::ledsChanged, this, &KeyboardInputRedirection::ledsChanged);
    if (waylandServer()) {
        m_xkb->setSeat(waylandServer()->seat());
    }
}

KeyboardInputRedirection::~KeyboardInputRedirection() = default;

class KeyStateChangedSpy : public InputEventSpy
{
public:
    KeyStateChangedSpy(InputRedirection *input)
        : m_input(input)
    {
    }

    void keyEvent(KeyEvent *event) override
    {
        if (event->isAutoRepeat()) {
            return;
        }
        emit m_input->keyStateChanged(event->nativeScanCode(), event->type() == QEvent::KeyPress ? InputRedirection::KeyboardKeyPressed : InputRedirection::KeyboardKeyReleased);
    }

private:
    InputRedirection *m_input;
};

class ModifiersChangedSpy : public InputEventSpy
{
public:
    ModifiersChangedSpy(InputRedirection *input)
        : m_input(input)
        , m_modifiers()
    {
    }

    void keyEvent(KeyEvent *event) override
    {
        if (event->isAutoRepeat()) {
            return;
        }
        updateModifiers(event->modifiers());
    }

    void updateModifiers(Qt::KeyboardModifiers mods)
    {
        if (mods == m_modifiers) {
            return;
        }
        emit m_input->keyboardModifiersChanged(mods, m_modifiers);
        m_modifiers = mods;
    }

private:
    InputRedirection *m_input;
    Qt::KeyboardModifiers m_modifiers;
};

bool KeyboardInputRedirection::isEnabled() const
{
    return m_isEnabled;
}

void KeyboardInputRedirection::setEnabled(bool enabled)
{
    if (m_isEnabled == enabled) {
        return;
    }
    m_isEnabled = enabled;
    if (enabled) {
        enable();
    } else {
        disable();
    }
}

void KeyboardInputRedirection::resetEnabled()
{
    m_isEnabled = false;
}

void KeyboardInputRedirection::enable()
{
    const auto config = kwinApp()->kxkbConfig();
    m_xkb->setNumLockConfig(InputConfig::self()->inputConfig());
    m_xkb->setConfig(config);

    m_keyStateChangedSpy.reset(new KeyStateChangedSpy(m_input));
    m_input->installInputEventSpy(m_keyStateChangedSpy.data());
    m_modifiersChangedSpy.reset(new ModifiersChangedSpy(m_input));
    m_input->installInputEventSpy(m_modifiersChangedSpy.data());
    m_keyboardLayout.reset(new KeyboardLayout(m_xkb.data(), config));
    m_keyboardLayout->init();
    m_input->installInputEventSpy(m_keyboardLayout.data());

    if (waylandServer()->hasGlobalShortcutSupport()) {
        m_modifierOnlyShortcutsSpy.reset(new ModifierOnlyShortcuts());
        m_input->installInputEventSpy(m_modifierOnlyShortcutsSpy.data());
    }

    m_keyboardRepeat.reset(new KeyboardRepeat(m_xkb.data()));
    connect(m_keyboardRepeat.data(), &KeyboardRepeat::keyRepeat, this, [this](quint32 button, quint32 time) {
        processKey(button, InputRedirection::KeyboardKeyAutoRepeat, time, nullptr);
    });
    m_input->installInputEventSpy(m_keyboardRepeat.data());

    connect(workspace(), &QObject::destroyed, this, &KeyboardInputRedirection::resetEnabled);
    connect(waylandServer(), &QObject::destroyed, this, &KeyboardInputRedirection::resetEnabled);
    m_clientActivatedConnection = connect(workspace(), &Workspace::clientActivated, this,
        [this] {
            disconnect(m_activeClientSurfaceChangedConnection);
            if (auto c = workspace()->activeClient()) {
                m_activeClientSurfaceChangedConnection = connect(c, &Toplevel::surfaceChanged, this, &KeyboardInputRedirection::update);
            } else {
                m_activeClientSurfaceChangedConnection = QMetaObject::Connection();
            }
            update();
        }
    );
    if (waylandServer()->hasScreenLockerIntegration()) {
        connect(ScreenLocker::KSldApp::self(), &ScreenLocker::KSldApp::lockStateChanged, this, &KeyboardInputRedirection::update);
    }
}

void KeyboardInputRedirection::disable()
{
    disconnect(m_activeClientSurfaceChangedConnection);
    m_activeClientSurfaceChangedConnection = QMetaObject::Connection();

    disconnect(m_clientActivatedConnection);
    m_clientActivatedConnection = QMetaObject::Connection();

    m_keyStateChangedSpy.reset();
    m_modifiersChangedSpy.reset();
    m_modifierOnlyShortcutsSpy.reset();
    m_keyboardLayout.reset();
    m_keyboardRepeat.reset();

    disconnect(workspace(), &QObject::destroyed, this, &KeyboardInputRedirection::resetEnabled);
    disconnect(waylandServer(), &QObject::destroyed, this, &KeyboardInputRedirection::resetEnabled);

    if (waylandServer()->hasScreenLockerIntegration()) {
        disconnect(ScreenLocker::KSldApp::self(), &ScreenLocker::KSldApp::lockStateChanged,
                   this, &KeyboardInputRedirection::update);
    }
}

void KeyboardInputRedirection::update()
{
    if (!isEnabled()) {
        return;
    }
    auto seat = waylandServer()->seat();
    // TODO: this needs better integration
    Toplevel *found = nullptr;
    if (waylandServer()->isScreenLocked()) {
        const QList<Toplevel *> &stacking = Workspace::self()->stackingOrder();
        if (!stacking.isEmpty()) {
            auto it = stacking.end();
            do {
                --it;
                Toplevel *t = (*it);
                if (t->isDeleted()) {
                    // a deleted window doesn't get mouse events
                    continue;
                }
                if (!t->isLockScreen()) {
                    continue;
                }
                if (!t->readyForPainting()) {
                    continue;
                }
                found = t;
                break;
            } while (it != stacking.begin());
        }
    } else if (!input()->isSelectingWindow()) {
        found = workspace()->activeClient();
    }
    if (found && found->surface()) {
        if (found->surface() != seat->focusedKeyboardSurface()) {
            seat->setFocusedKeyboardSurface(found->surface());
        }
    } else {
        seat->setFocusedKeyboardSurface(nullptr);
    }
}

void KeyboardInputRedirection::processKey(uint32_t key, InputRedirection::KeyboardKeyState state, uint32_t time, LibInput::Device *device)
{
    QEvent::Type type;
    bool autoRepeat = false;
    switch (state) {
    case InputRedirection::KeyboardKeyAutoRepeat:
        autoRepeat = true;
        // fall through
    case InputRedirection::KeyboardKeyPressed:
        type = QEvent::KeyPress;
        break;
    case InputRedirection::KeyboardKeyReleased:
        type = QEvent::KeyRelease;
        break;
    default:
        Q_UNREACHABLE();
    }

    const quint32 previousLayout = m_xkb->currentLayout();
    if (!autoRepeat) {
        m_xkb->updateKey(key, state);
    }

    const xkb_keysym_t keySym = m_xkb->currentKeysym();
    KeyEvent event(type,
                   m_xkb->toQtKey(keySym),
                   m_xkb->modifiers(),
                   key,
                   keySym,
                   m_xkb->toString(keySym),
                   autoRepeat,
                   time,
                   device);
    event.setModifiersRelevantForGlobalShortcuts(m_xkb->modifiersRelevantForGlobalShortcuts());

    m_input->processSpies(std::bind(&InputEventSpy::keyEvent, std::placeholders::_1, &event));
    if (!isEnabled()) {
        return;
    }
    m_input->processFilters(std::bind(&InputEventFilter::keyEvent, std::placeholders::_1, &event));

    m_xkb->forwardModifiers();

    if (event.modifiersRelevantForGlobalShortcuts() == Qt::KeyboardModifier::NoModifier && type != QEvent::KeyRelease) {
        m_keyboardLayout->checkLayoutChange(previousLayout);
    }
}

void KeyboardInputRedirection::processModifiers(uint32_t modsDepressed, uint32_t modsLatched, uint32_t modsLocked, uint32_t group)
{
    if (!isEnabled()) {
        return;
    }
    const quint32 previousLayout = m_xkb->currentLayout();
    // TODO: send to proper Client and also send when active Client changes
    m_xkb->updateModifiers(modsDepressed, modsLatched, modsLocked, group);
    m_modifiersChangedSpy->updateModifiers(modifiers());
    m_keyboardLayout->checkLayoutChange(previousLayout);
}

void KeyboardInputRedirection::processKeymapChange(int fd, uint32_t size)
{
    if (!isEnabled()) {
        return;
    }
    // TODO: should we pass the keymap to our Clients? Or only to the currently active one and update
    m_xkb->installKeymap(fd, size);
    m_keyboardLayout->resetLayout();
}

}
