/*
    SPDX-FileCopyrightText: 2022 Nicolas Fella <nicolas.fella@gmx.de>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "stickykeys.h"
#include "keyboard_input.h"
#include "xkb.h"

#include <QTimer>

#include <KLazyLocalizedString>
#if KWIN_BUILD_NOTIFICATIONS
#include <KNotification>
#endif

struct Modifier
{
    Qt::Key key;
    KLazyLocalizedString lockedText;
};

static const std::array<Modifier, 5> modifiers = {
    Modifier{Qt::Key_Shift, kli18n("The Shift key has been locked and is now active for all of the following keypresses.")},
    Modifier{Qt::Key_Control, kli18n("The Control key has been locked and is now active for all of the following keypresses.")},
    Modifier{Qt::Key_Alt, kli18n("The Alt key has been locked and is now active for all of the following keypresses.")},
    Modifier{Qt::Key_Meta, kli18n("The Meta key has been locked and is now active for all of the following keypresses.")},
    Modifier{Qt::Key_AltGr, kli18n("The AltGr key has been locked and is now active for all of the following keypresses.")},
};

StickyKeysFilter::StickyKeysFilter()
    : m_configWatcher(KConfigWatcher::create(KSharedConfig::openConfig("kaccessrc")))
{
    const QLatin1String groupName("Keyboard");
    connect(m_configWatcher.get(), &KConfigWatcher::configChanged, this, [this, groupName](const KConfigGroup &group) {
        if (group.name() == groupName) {
            loadConfig(group);
        }
    });
    loadConfig(m_configWatcher->config()->group(groupName));

    for (int mod : std::as_const(m_modifiers)) {
        m_keyStates[mod] = None;
    }
}

KWin::Xkb::Modifier keyToModifier(Qt::Key key)
{
    if (key == Qt::Key_Shift) {
        return KWin::Xkb::Shift;
    } else if (key == Qt::Key_Alt) {
        return KWin::Xkb::Mod1;
    } else if (key == Qt::Key_Control) {
        return KWin::Xkb::Control;
    } else if (key == Qt::Key_AltGr) {
        return KWin::Xkb::Mod5;
    } else if (key == Qt::Key_Meta) {
        return KWin::Xkb::Mod4;
    }

    return KWin::Xkb::NoModifier;
}

void StickyKeysFilter::loadConfig(const KConfigGroup &group)
{
    KWin::input()->uninstallInputEventFilter(this);

    m_lockKeys = group.readEntry<bool>("StickyKeysLatch", true);
    m_showNotificationForLockedKeys = group.readEntry<bool>("kNotifyModifiers", false);
    m_disableOnTwoKeys = group.readEntry<bool>("StickyKeysAutoOff", false);

    if (!m_lockKeys) {
        // locking keys is deactivated, unlock all locked keys
        for (auto it = m_keyStates.keyValueBegin(); it != m_keyStates.keyValueEnd(); ++it) {
            if (it->second == KeyState::Locked) {
                it->second = KeyState::None;
                KWin::input()->keyboard()->xkb()->setModifierLocked(keyToModifier(static_cast<Qt::Key>(it->first)), false);
                KWin::input()->keyboard()->xkb()->forwardModifiers();
            }
        }
    }

    if (group.readEntry<bool>("StickyKeys", false)) {
        KWin::input()->prependInputEventFilter(this);
    } else {
        // sticky keys are deactivated, unlatch all latched/locked keys
        for (auto it = m_keyStates.keyValueBegin(); it != m_keyStates.keyValueEnd(); ++it) {
            if (it->second != KeyState::None) {
                it->second = KeyState::None;
                KWin::input()->keyboard()->xkb()->setModifierLatched(keyToModifier(static_cast<Qt::Key>(it->first)), false);
                KWin::input()->keyboard()->xkb()->forwardModifiers();
            }
        }
    }
}

bool StickyKeysFilter::keyEvent(KWin::KeyEvent *event)
{
    if (m_modifiers.contains(event->key())) {

        if (event->type() == QEvent::KeyPress) {
            m_pressedModifiers << event->key();
        } else {
            m_pressedModifiers.remove(event->key());
        }

        auto keyState = m_keyStates.find(event->key());

        if (keyState != m_keyStates.end()) {
            if (event->type() == QKeyEvent::KeyPress) {
                // An unlatched modifier was pressed, latch it
                if (keyState.value() == None) {
                    keyState.value() = Latched;
                    KWin::input()->keyboard()->xkb()->setModifierLatched(keyToModifier(static_cast<Qt::Key>(event->key())), true);
                }
                // A latched modifier was pressed, lock it
                else if (keyState.value() == Latched && m_lockKeys) {
                    keyState.value() = Locked;
                    KWin::input()->keyboard()->xkb()->setModifierLatched(keyToModifier(static_cast<Qt::Key>(event->key())), false);
                    KWin::input()->keyboard()->xkb()->setModifierLocked(keyToModifier(static_cast<Qt::Key>(event->key())), true);

                    if (m_showNotificationForLockedKeys) {
#if KWIN_BUILD_NOTIFICATIONS
                        KNotification *noti = new KNotification("modifierkey-locked");
                        noti->setComponentName("kaccess");

                        for (const auto mod : modifiers) {
                            if (mod.key == event->key()) {
                                noti->setText(mod.lockedText.toString());
                                break;
                            }
                        }

                        noti->sendEvent();
#endif
                    }
                }
                // A locked modifier was pressed, unlock it
                else if (keyState.value() == Locked && m_lockKeys) {
                    keyState.value() = None;
                    KWin::input()->keyboard()->xkb()->setModifierLocked(keyToModifier(static_cast<Qt::Key>(event->key())), false);
                }
            }
        }
    } else if (event->type() == QKeyEvent::KeyPress) {

        if (!m_pressedModifiers.isEmpty() && m_disableOnTwoKeys) {
            disableStickyKeys();
        }

        // a non-modifier key was pressed, unlatch all unlocked modifiers
        for (auto it = m_keyStates.keyValueBegin(); it != m_keyStates.keyValueEnd(); ++it) {

            if (it->second == Locked) {
                continue;
            }

            it->second = KeyState::None;

            KWin::input()->keyboard()->xkb()->setModifierLatched(keyToModifier(static_cast<Qt::Key>(it->first)), false);
        }
    }

    return false;
}

void StickyKeysFilter::disableStickyKeys()
{
    for (auto it = m_keyStates.keyValueBegin(); it != m_keyStates.keyValueEnd(); ++it) {
        it->second = KeyState::None;
        KWin::input()->keyboard()->xkb()->setModifierLatched(keyToModifier(static_cast<Qt::Key>(it->first)), false);
        KWin::input()->keyboard()->xkb()->setModifierLocked(keyToModifier(static_cast<Qt::Key>(it->first)), false);
    }

    KWin::input()->uninstallInputEventFilter(this);
}

bool StickyKeysFilter::pointerEvent(KWin::MouseEvent *event, quint32 nativeButton)
{
    if (event->type() == QEvent::MouseButtonRelease) {
        // unlatch all unlocked modifiers
        for (auto it = m_keyStates.keyValueBegin(); it != m_keyStates.keyValueEnd(); ++it) {

            if (it->second == Locked) {
                continue;
            }

            it->second = KeyState::None;

            KWin::input()->keyboard()->xkb()->setModifierLatched(keyToModifier(static_cast<Qt::Key>(it->first)), false);
            QTimer::singleShot(0, this, [] {
                KWin::input()->keyboard()->xkb()->forwardModifiers();
            });
        }
    }

    return false;
}

#include "moc_stickykeys.cpp"
