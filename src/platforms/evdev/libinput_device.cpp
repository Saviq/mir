/*
 * Copyright © 2015 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authored by: Andreas Pokorny <andreas.pokorny@canonical.com>
 */

#include "libinput_device.h"
#include "libinput_ptr.h"
#include "libinput_device_ptr.h"
#include "input_modifier_utils.h"
#include "evdev_device_detection.h"

#include "mir/input/input_sink.h"
#include "mir/input/input_report.h"
#include "mir/input/device_capability.h"
#include "mir/input/pointer_settings.h"
#include "mir/input/touch_pad_settings.h"
#include "mir/input/input_device_info.h"
#include "mir/events/event_builders.h"
#include "mir/geometry/displacement.h"
#include "mir/dispatch/dispatchable.h"
#include "mir/fd.h"

#include <libinput.h>
#include <linux/input.h>  // only used to get constants for input reports

#include <cstring>
#include <chrono>
#include <sstream>
#include <algorithm>

namespace md = mir::dispatch;
namespace mi = mir::input;
namespace mie = mi::evdev;
using namespace std::literals::chrono_literals;

namespace
{

void null_deleter(MirEvent *) {}

}

mie::LibInputDevice::LibInputDevice(std::shared_ptr<mi::InputReport> const& report, char const* path,
                                    LibInputDevicePtr dev)
    : report{report}, accumulated_touch_event{nullptr, null_deleter}, pointer_pos{0, 0}, modifier_state{0},
      button_state{0}
{
    add_device_of_group(path, std::move(dev));
}

void mie::LibInputDevice::add_device_of_group(char const* path, LibInputDevicePtr dev)
{
    paths.emplace_back(path);
    devices.emplace_back(std::move(dev));
    update_device_info();
}

bool mie::LibInputDevice::is_in_group(char const* path)
{
    return end(paths) != find(begin(paths), end(paths), std::string{path});
}

mie::LibInputDevice::~LibInputDevice() = default;

void mie::LibInputDevice::start(InputSink* sink, EventBuilder* builder)
{
    this->sink = sink;
    this->builder = builder;
}

void mie::LibInputDevice::stop()
{
    sink = nullptr;
    builder = nullptr;
}

void mie::LibInputDevice::process_event(libinput_event* event)
{
    if (!sink)
        return;

    switch(libinput_event_get_type(event))
    {
    case LIBINPUT_EVENT_KEYBOARD_KEY:
        sink->handle_input(*convert_event(libinput_event_get_keyboard_event(event)));
        break;
    case LIBINPUT_EVENT_POINTER_MOTION:
        sink->handle_input(*convert_motion_event(libinput_event_get_pointer_event(event)));
        break;
    case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE:
        sink->handle_input(*convert_absolute_motion_event(libinput_event_get_pointer_event(event)));
        break;
    case LIBINPUT_EVENT_POINTER_BUTTON:
        sink->handle_input(*convert_button_event(libinput_event_get_pointer_event(event)));
        break;
    case LIBINPUT_EVENT_POINTER_AXIS:
        sink->handle_input(*convert_axis_event(libinput_event_get_pointer_event(event)));
        break;
    // touch events are processed as a batch of changes over all touch pointts
    case LIBINPUT_EVENT_TOUCH_DOWN:
        add_touch_down_event(libinput_event_get_touch_event(event));
        break;
    case LIBINPUT_EVENT_TOUCH_UP:
        add_touch_up_event(libinput_event_get_touch_event(event));
        break;
    case LIBINPUT_EVENT_TOUCH_MOTION:
        add_touch_motion_event(libinput_event_get_touch_event(event));
        break;
    case LIBINPUT_EVENT_TOUCH_CANCEL:
        // Not yet provided by libinput.
        break;
    case LIBINPUT_EVENT_TOUCH_FRAME:
        sink->handle_input(get_accumulated_touch_event(0ns));
        accumulated_touch_event.reset();
        break;
    default:
        break;
    }
}

mir::EventUPtr mie::LibInputDevice::convert_event(libinput_event_keyboard* keyboard)
{
    std::chrono::nanoseconds const time = std::chrono::microseconds(libinput_event_keyboard_get_time_usec(keyboard));
    auto const action = libinput_event_keyboard_get_key_state(keyboard) == LIBINPUT_KEY_STATE_PRESSED ?
                      mir_keyboard_action_down :
                      mir_keyboard_action_up;
    auto const code = libinput_event_keyboard_get_key(keyboard);
    report->received_event_from_kernel(time.count(), EV_KEY, code, action);

    auto event = builder->key_event(time,
                                    action,
                                    xkb_keysym_t{0},
                                    code,
                                    mie::expand_modifiers(modifier_state));

    if (action == mir_keyboard_action_down)
        modifier_state |= mie::to_modifiers(code);
    else
        modifier_state &= ~mie::to_modifiers(code);

    return event;
}

mir::EventUPtr mie::LibInputDevice::convert_button_event(libinput_event_pointer* pointer)
{
    std::chrono::nanoseconds const time = std::chrono::microseconds(libinput_event_pointer_get_time_usec(pointer));
    auto const button = libinput_event_pointer_get_button(pointer);
    auto const action = (libinput_event_pointer_get_button_state(pointer) == LIBINPUT_BUTTON_STATE_PRESSED)?
        mir_pointer_action_button_down : mir_pointer_action_button_up;

    auto const do_not_swap_buttons = mir_pointer_button_primary;
    auto const pointer_button = mie::to_pointer_button(button, do_not_swap_buttons);
    auto const relative_x_value = 0.0f;
    auto const relative_y_value = 0.0f;
    auto const hscroll_value = 0.0f;
    auto const vscroll_value = 0.0f;

    report->received_event_from_kernel(time.count(), EV_KEY, pointer_button, action);

    if (action == mir_pointer_action_button_down)
        button_state = MirPointerButton(button_state | uint32_t(pointer_button));
    else
        button_state = MirPointerButton(button_state & ~uint32_t(pointer_button));

    auto event = builder->pointer_event(time, mie::expand_modifiers(modifier_state), action, button_state,
                                        pointer_pos.x.as_float(), pointer_pos.y.as_float(), hscroll_value,
                                        vscroll_value, relative_x_value, relative_y_value);

    return event;
}

mir::EventUPtr mie::LibInputDevice::convert_motion_event(libinput_event_pointer* pointer)
{
    std::chrono::nanoseconds const time = std::chrono::microseconds(libinput_event_pointer_get_time_usec(pointer));
    auto const action = mir_pointer_action_motion;
    auto const hscroll_value = 0.0f;
    auto const vscroll_value = 0.0f;

    report->received_event_from_kernel(time.count(), EV_REL, 0, 0);

    mir::geometry::Displacement const movement{
        libinput_event_pointer_get_dx(pointer),
        libinput_event_pointer_get_dy(pointer)};
    pointer_pos = pointer_pos + movement;

    sink->confine_pointer(pointer_pos);

    auto event = builder->pointer_event(time, mie::expand_modifiers(modifier_state), action, button_state,
                                        pointer_pos.x.as_float(), pointer_pos.y.as_float(), hscroll_value,
                                        vscroll_value, movement.dx.as_float(), movement.dy.as_float());

    return event;
}

mir::EventUPtr mie::LibInputDevice::convert_absolute_motion_event(libinput_event_pointer* pointer)
{
    // a pointing device that emits absolute coordinates
    std::chrono::nanoseconds const time = std::chrono::microseconds(libinput_event_pointer_get_time_usec(pointer));
    auto const action = mir_pointer_action_motion;
    auto const hscroll_value = 0.0f;
    auto const vscroll_value = 0.0f;

    report->received_event_from_kernel(time.count(), EV_ABS, 0, 0);
    auto const old_pointer_pos = pointer_pos;
    pointer_pos = mir::geometry::Point{
        libinput_event_pointer_get_absolute_x(pointer),
        libinput_event_pointer_get_absolute_y(pointer)};
    auto const movement = pointer_pos - old_pointer_pos;

    sink->confine_pointer(pointer_pos);

    auto event = builder->pointer_event(time, mie::expand_modifiers(modifier_state), action, button_state,
                                        pointer_pos.x.as_float(), pointer_pos.y.as_float(), hscroll_value,
                                        vscroll_value, movement.dx.as_float(), movement.dy.as_float());
    return event;
}

mir::EventUPtr mie::LibInputDevice::convert_axis_event(libinput_event_pointer* pointer)
{
    std::chrono::nanoseconds const time = std::chrono::microseconds(libinput_event_pointer_get_time_usec(pointer));
    auto const action = mir_pointer_action_motion;
    auto const relative_x_value = 0.0f;
    auto const relative_y_value = 0.0f;
    auto const hscroll_value = libinput_event_pointer_has_axis(pointer, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL)
        ? horizontal_scroll_speed * libinput_event_pointer_get_axis_value(pointer,
                                                                          LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL)
        : 0.0f;
    auto const vscroll_value = libinput_event_pointer_has_axis(pointer, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL)
        ? vertical_scroll_speed * libinput_event_pointer_get_axis_value(pointer,
                                                                        LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL)
        : 0.0f;

    report->received_event_from_kernel(time.count(), EV_REL, 0, 0);
    auto event = builder->pointer_event(time, mie::expand_modifiers(modifier_state), action, button_state,
                                        pointer_pos.x.as_float(), pointer_pos.y.as_float(), hscroll_value,
                                        vscroll_value, relative_x_value, relative_y_value);
    return event;
}

MirEvent& mie::LibInputDevice::get_accumulated_touch_event(std::chrono::nanoseconds timestamp)
{
    if (!accumulated_touch_event)
    {
        report->received_event_from_kernel(timestamp.count(), EV_SYN, 0, 0);
        accumulated_touch_event = builder->touch_event(timestamp, mie::expand_modifiers(modifier_state));
    }

    return *accumulated_touch_event;
}

void mie::LibInputDevice::add_touch_down_event(libinput_event_touch* touch)
{
    std::chrono::nanoseconds const time = std::chrono::microseconds(libinput_event_touch_get_time_usec(touch));
    auto& event = get_accumulated_touch_event(time);

    MirTouchId const id = libinput_event_touch_get_slot(touch);
    auto const action = mir_touch_action_down;
    // TODO make libinput indicate tool type
    auto const tool = mir_touch_tooltype_finger;
    float const pressure = libinput_event_touch_get_pressure(touch);
    auto const screen = sink->bounding_rectangle();
    uint32_t const width = screen.size.width.as_int();
    uint32_t const height = screen.size.height.as_int();
    float const x = libinput_event_touch_get_x_transformed(touch, width);
    float const y = libinput_event_touch_get_y_transformed(touch, height);
    float const major = libinput_event_touch_get_major_transformed(touch, width, height);
    float const minor = libinput_event_touch_get_minor_transformed(touch, width, height);
    // TODO why do we send size to clients?
    float const size = std::max(major, minor);

    // TODO extend for touch screens that provide orientation
    builder->add_touch(event, id, action, tool, x, y, pressure, major, minor, size);
}

void mie::LibInputDevice::add_touch_up_event(libinput_event_touch* touch)
{
    std::chrono::nanoseconds const time = std::chrono::microseconds(libinput_event_touch_get_time_usec(touch));
    auto& event = get_accumulated_touch_event(time);
    MirTouchId const id = libinput_event_touch_get_slot(touch);
    auto const action = mir_touch_action_up;
    auto const tool = mir_touch_tooltype_finger; // TODO make libinput indicate tool type

    float const pressure = 0.0f;
    float const major = 0.0f;
    float const minor = 0.0f;
    float const size = 0.0f;
    // TODO extend for touch screens that provide orientation and major/minor
    builder->add_touch(event, id, action, tool, libinput_event_touch_get_x(touch), libinput_event_touch_get_y(touch),
                       pressure, major, minor, size);
}

void mie::LibInputDevice::add_touch_motion_event(libinput_event_touch* touch)
{
    std::chrono::nanoseconds const time = std::chrono::microseconds(libinput_event_touch_get_time_usec(touch));
    auto& event = get_accumulated_touch_event(time);

    MirTouchId const id = libinput_event_touch_get_slot(touch);
    auto const action = mir_touch_action_change;
    auto const tool = mir_touch_tooltype_finger; // TODO make libinput indicate tool type
    float const pressure = libinput_event_touch_get_pressure(touch);
    auto const screen = sink->bounding_rectangle();
    uint32_t const width = screen.size.width.as_int();
    uint32_t const height = screen.size.height.as_int();
    float const x = libinput_event_touch_get_x_transformed(touch, width);
    float const y = libinput_event_touch_get_y_transformed(touch, height);
    float const major = libinput_event_touch_get_major_transformed(touch, width, height);
    float const minor = libinput_event_touch_get_minor_transformed(touch, width, height);
    // TODO why do we send size to clients?
    float const size = std::max(major, minor);

    // TODO extend for touch screens that provide orientation
    builder->add_touch(event, id, action, tool, x, y, pressure, major, minor, size);
}

mi::InputDeviceInfo mie::LibInputDevice::get_device_info()
{
    return info;
}

void mie::LibInputDevice::update_device_info()
{
    auto dev = device();
    std::string name = libinput_device_get_name(dev);
    std::stringstream unique_id(name);
    unique_id << '-' << libinput_device_get_sysname(dev) << '-' <<
        libinput_device_get_id_vendor(dev) << '-' <<
        libinput_device_get_id_product(dev);
    mi::DeviceCapabilities caps;

    for (auto const& path : paths)
        caps |= mie::detect_device_capabilities(path.c_str());

    info = mi::InputDeviceInfo{name, unique_id.str(), caps};
}

libinput_device_group* mie::LibInputDevice::group()
{
    return libinput_device_get_device_group(device());
}

libinput_device* mie::LibInputDevice::device() const
{
    return devices.front().get();
}

mir::UniqueModulePtr<mi::PointerSettings> mie::LibInputDevice::get_pointer_settings() const
{
    mir::UniqueModulePtr<PointerSettings> ret;
    if (!contains(info.capabilities, mi::DeviceCapability::pointer))
        return ret;

    auto dev = device();
    auto accel_speed = libinput_device_config_accel_get_speed(dev);
    auto left_handed = (libinput_device_config_left_handed_get(dev) == 1);

    ret = make_module_ptr<mi::PointerSettings>();
    ret->cursor_speed = accel_speed;
    ret->vertical_scroll_speed = vertical_scroll_speed;
    ret->horizontal_scroll_speed = horizontal_scroll_speed;
    ret->primary_button = left_handed? mir_pointer_button_secondary : mir_pointer_button_primary;
    return ret;
}

void mie::LibInputDevice::apply_settings(mir::input::PointerSettings const& settings)
{
    if (!contains(info.capabilities, mi::DeviceCapability::pointer))
        return;

    auto dev = device();
    libinput_device_config_accel_set_speed(dev, settings.cursor_speed);
    libinput_device_config_left_handed_set(dev, mir_pointer_button_primary != settings.primary_button);
    vertical_scroll_speed = settings.vertical_scroll_speed;
    horizontal_scroll_speed = settings.horizontal_scroll_speed;
}

mir::UniqueModulePtr<mi::TouchPadSettings> mie::LibInputDevice::get_touch_pad_settings() const
{
    mir::UniqueModulePtr<TouchPadSettings> ret;
    if (!contains(info.capabilities, mi::DeviceCapability::touchpad))
        return ret;

    auto dev = device();
    auto click_modes = libinput_device_config_click_get_method(dev);
    auto scroll_modes = libinput_device_config_scroll_get_method(dev);
        
    ret = make_module_ptr<TouchPadSettings>();
    
    ret->click_mode = mir_touch_pad_click_mode_none;
    if (click_modes & LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS)
        ret->click_mode |= mir_touch_pad_click_mode_area_to_click;
    if (click_modes & LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER)
        ret->click_mode |= mir_touch_pad_click_mode_finger_count;

    ret->scroll_mode = mir_touch_pad_scroll_mode_none;
    if (scroll_modes & LIBINPUT_CONFIG_SCROLL_2FG)
        ret->scroll_mode |= mir_touch_pad_scroll_mode_two_finger_scroll;
    if (scroll_modes & LIBINPUT_CONFIG_SCROLL_EDGE)
        ret->scroll_mode |= mir_touch_pad_scroll_mode_edge_scroll;
    if (scroll_modes & LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN)
        ret->scroll_mode |= mir_touch_pad_scroll_mode_button_down_scroll;

    ret->tap_to_click = libinput_device_config_tap_get_enabled(dev) == LIBINPUT_CONFIG_TAP_ENABLED;
    ret->disable_while_typing = libinput_device_config_dwt_get_enabled(dev) == LIBINPUT_CONFIG_DWT_ENABLED;
    ret->disable_with_mouse =
        libinput_device_config_send_events_get_mode(dev) == LIBINPUT_CONFIG_SEND_EVENTS_DISABLED_ON_EXTERNAL_MOUSE;
    ret->middle_mouse_button_emulation =
        libinput_device_config_middle_emulation_get_enabled(dev) == LIBINPUT_CONFIG_MIDDLE_EMULATION_ENABLED;

    return ret;
}

void mie::LibInputDevice::apply_settings(mi::TouchPadSettings const&)
{
}
