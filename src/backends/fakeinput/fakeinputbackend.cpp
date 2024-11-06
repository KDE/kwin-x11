/*
    SPDX-FileCopyrightText: 2021 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "fakeinputbackend.h"
#include "fakeinputdevice.h"
#include "wayland/display.h"

#include "wayland/qwayland-server-fake-input.h"

namespace KWin
{

static const quint32 s_version = 5;

class FakeInputBackendPrivate : public QtWaylandServer::org_kde_kwin_fake_input
{
public:
    FakeInputBackendPrivate(FakeInputBackend *q, Display *display);

    FakeInputDevice *findDevice(Resource *resource);
    std::chrono::microseconds currentTime() const;

    FakeInputBackend *q;
    Display *display;
    std::map<Resource *, std::unique_ptr<FakeInputDevice>> devices;

protected:
    void org_kde_kwin_fake_input_bind_resource(Resource *resource) override;
    void org_kde_kwin_fake_input_destroy_resource(Resource *resource) override;
    void org_kde_kwin_fake_input_authenticate(Resource *resource, const QString &application, const QString &reason) override;
    void org_kde_kwin_fake_input_pointer_motion(Resource *resource, wl_fixed_t delta_x, wl_fixed_t delta_y) override;
    void org_kde_kwin_fake_input_button(Resource *resource, uint32_t button, uint32_t state) override;
    void org_kde_kwin_fake_input_axis(Resource *resource, uint32_t axis, wl_fixed_t value) override;
    void org_kde_kwin_fake_input_touch_down(Resource *resource, uint32_t id, wl_fixed_t x, wl_fixed_t y) override;
    void org_kde_kwin_fake_input_touch_motion(Resource *resource, uint32_t id, wl_fixed_t x, wl_fixed_t y) override;
    void org_kde_kwin_fake_input_touch_up(Resource *resource, uint32_t id) override;
    void org_kde_kwin_fake_input_touch_cancel(Resource *resource) override;
    void org_kde_kwin_fake_input_touch_frame(Resource *resource) override;
    void org_kde_kwin_fake_input_pointer_motion_absolute(Resource *resource, wl_fixed_t x, wl_fixed_t y) override;
    void org_kde_kwin_fake_input_keyboard_key(Resource *resource, uint32_t button, uint32_t state) override;
    void org_kde_kwin_fake_input_destroy(Resource *resource) override;
};

FakeInputBackendPrivate::FakeInputBackendPrivate(FakeInputBackend *q, Display *display)
    : q(q)
    , display(display)
{
}

void FakeInputBackendPrivate::org_kde_kwin_fake_input_bind_resource(Resource *resource)
{
    auto device = new FakeInputDevice(q);
    devices[resource] = std::unique_ptr<FakeInputDevice>(device);
    Q_EMIT q->deviceAdded(device);
}

void FakeInputBackendPrivate::org_kde_kwin_fake_input_destroy(Resource *resource)
{
    wl_resource_destroy(resource->handle);
}

void FakeInputBackendPrivate::org_kde_kwin_fake_input_destroy_resource(Resource *resource)
{
    auto it = devices.find(resource);
    if (it == devices.end()) {
        return;
    }

    const auto [r, device] = std::move(*it);
    for (const auto button : device->pressedButtons) {
        Q_EMIT device->pointerButtonChanged(button, InputDevice::PointerButtonReleased, currentTime(), device.get());
    }
    for (const auto key : device->pressedKeys) {
        Q_EMIT device->keyChanged(key, InputDevice::KeyboardKeyReleased, currentTime(), device.get());
    }
    if (!device->activeTouches.empty()) {
        Q_EMIT device->touchCanceled(device.get());
    }
    devices.erase(it);
    Q_EMIT q->deviceRemoved(device.get());
}

FakeInputDevice *FakeInputBackendPrivate::findDevice(Resource *resource)
{
    return devices[resource].get();
}

std::chrono::microseconds FakeInputBackendPrivate::currentTime() const
{
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch());
}

void FakeInputBackendPrivate::org_kde_kwin_fake_input_authenticate(Resource *resource, const QString &application, const QString &reason)
{
    FakeInputDevice *device = findDevice(resource);
    if (device) {
        // TODO: make secure
        device->setAuthenticated(true);
    }
}

void FakeInputBackendPrivate::org_kde_kwin_fake_input_pointer_motion(Resource *resource, wl_fixed_t delta_x, wl_fixed_t delta_y)
{
    FakeInputDevice *device = findDevice(resource);
    if (!device->isAuthenticated()) {
        return;
    }
    const QPointF delta(wl_fixed_to_double(delta_x), wl_fixed_to_double(delta_y));

    Q_EMIT device->pointerMotion(delta, delta, currentTime(), device);
    Q_EMIT device->pointerFrame(device);
}

void FakeInputBackendPrivate::org_kde_kwin_fake_input_button(Resource *resource, uint32_t button, uint32_t state)
{
    FakeInputDevice *device = findDevice(resource);
    if (!device->isAuthenticated()) {
        return;
    }

    InputDevice::PointerButtonState nativeState;
    switch (state) {
    case WL_POINTER_BUTTON_STATE_PRESSED:
        nativeState = InputDevice::PointerButtonPressed;
        if (device->pressedButtons.contains(button)) {
            return;
        }
        device->pressedButtons.insert(button);
        break;
    case WL_POINTER_BUTTON_STATE_RELEASED:
        nativeState = InputDevice::PointerButtonReleased;
        if (!device->pressedButtons.remove(button)) {
            return;
        }
        break;
    default:
        return;
    }

    Q_EMIT device->pointerButtonChanged(button, nativeState, currentTime(), device);
    Q_EMIT device->pointerFrame(device);
}

void FakeInputBackendPrivate::org_kde_kwin_fake_input_axis(Resource *resource, uint32_t axis, wl_fixed_t value)
{
    FakeInputDevice *device = findDevice(resource);
    if (!device->isAuthenticated()) {
        return;
    }

    InputDevice::PointerAxis nativeAxis;
    switch (axis) {
    case WL_POINTER_AXIS_HORIZONTAL_SCROLL:
        nativeAxis = InputDevice::PointerAxisHorizontal;
        break;

    case WL_POINTER_AXIS_VERTICAL_SCROLL:
        nativeAxis = InputDevice::PointerAxisVertical;
        break;

    default:
        return;
    }

    Q_EMIT device->pointerAxisChanged(nativeAxis, wl_fixed_to_double(value), 0, InputDevice::PointerAxisSourceUnknown, false, currentTime(), device);
    Q_EMIT device->pointerFrame(device);
}

void FakeInputBackendPrivate::org_kde_kwin_fake_input_touch_down(Resource *resource, uint32_t id, wl_fixed_t x, wl_fixed_t y)
{
    FakeInputDevice *device = findDevice(resource);
    if (!device->isAuthenticated()) {
        return;
    }
    if (device->activeTouches.contains(id)) {
        return;
    }
    device->activeTouches.insert(id);
    Q_EMIT device->touchDown(id, QPointF(wl_fixed_to_double(x), wl_fixed_to_double(y)), currentTime(), device);
}

void FakeInputBackendPrivate::org_kde_kwin_fake_input_touch_motion(Resource *resource, uint32_t id, wl_fixed_t x, wl_fixed_t y)
{
    FakeInputDevice *device = findDevice(resource);
    if (!device->isAuthenticated()) {
        return;
    }
    if (!device->activeTouches.contains(id)) {
        return;
    }
    Q_EMIT device->touchMotion(id, QPointF(wl_fixed_to_double(x), wl_fixed_to_double(y)), currentTime(), device);
}

void FakeInputBackendPrivate::org_kde_kwin_fake_input_touch_up(Resource *resource, uint32_t id)
{
    FakeInputDevice *device = findDevice(resource);
    if (!device->isAuthenticated()) {
        return;
    }
    if (device->activeTouches.remove(id)) {
        Q_EMIT device->touchUp(id, currentTime(), device);
    }
}

void FakeInputBackendPrivate::org_kde_kwin_fake_input_touch_cancel(Resource *resource)
{
    FakeInputDevice *device = findDevice(resource);
    if (!device->isAuthenticated()) {
        return;
    }
    device->activeTouches.clear();
    Q_EMIT device->touchCanceled(device);
}

void FakeInputBackendPrivate::org_kde_kwin_fake_input_touch_frame(Resource *resource)
{
    FakeInputDevice *device = findDevice(resource);
    if (!device->isAuthenticated()) {
        return;
    }
    Q_EMIT device->touchFrame(device);
}

void FakeInputBackendPrivate::org_kde_kwin_fake_input_pointer_motion_absolute(Resource *resource, wl_fixed_t x, wl_fixed_t y)
{
    FakeInputDevice *device = findDevice(resource);
    if (!device->isAuthenticated()) {
        return;
    }

    Q_EMIT device->pointerMotionAbsolute(QPointF(wl_fixed_to_double(x), wl_fixed_to_double(y)), currentTime(), device);
    Q_EMIT device->pointerFrame(device);
}

void FakeInputBackendPrivate::org_kde_kwin_fake_input_keyboard_key(Resource *resource, uint32_t key, uint32_t state)
{
    FakeInputDevice *device = findDevice(resource);
    if (!device->isAuthenticated()) {
        return;
    }

    InputDevice::KeyboardKeyState nativeState;
    switch (state) {
    case WL_KEYBOARD_KEY_STATE_PRESSED:
        nativeState = InputDevice::KeyboardKeyPressed;
        if (device->pressedKeys.contains(key)) {
            return;
        }
        device->pressedKeys.insert(key);
        break;

    case WL_KEYBOARD_KEY_STATE_RELEASED:
        if (!device->pressedKeys.remove(key)) {
            return;
        }
        nativeState = InputDevice::KeyboardKeyReleased;
        break;

    default:
        return;
    }

    Q_EMIT device->keyChanged(key, nativeState, currentTime(), device);
}

FakeInputBackend::FakeInputBackend(Display *display)
    : d(std::make_unique<FakeInputBackendPrivate>(this, display))
{
}

FakeInputBackend::~FakeInputBackend() = default;

void FakeInputBackend::initialize()
{
    d->init(*d->display, s_version);
}

} // namespace KWin

#include "moc_fakeinputbackend.cpp"
