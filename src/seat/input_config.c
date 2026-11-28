// SPDX-License-Identifier: GPL-2.0-only
#include <string.h>

#include <wlr/backend/libinput.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/util/log.h>

#include "input_config.h"
#include "woodland.h"
#include "woodland_config.h"

void woodland_input_configure(struct woodland_server *server, struct wlr_input_device *device)
{
    if (!server || !server->config || !device || !wlr_input_device_is_libinput(device)) {
        return;
    }
    struct libinput_device *dev = wlr_libinput_get_device_handle(device);
    /* tap 手指数 > 0 即触摸板；鼠标 / 触摸屏为 0 */
    if (!dev || libinput_device_config_tap_get_finger_count(dev) <= 0) {
        return;
    }
    const struct woodland_config_touchpad *tp = &server->config->touchpad;

    libinput_device_config_tap_set_enabled(
        dev, tp->tap_to_click ? LIBINPUT_CONFIG_TAP_ENABLED : LIBINPUT_CONFIG_TAP_DISABLED);
    if (libinput_device_config_scroll_has_natural_scroll(dev)) {
        libinput_device_config_scroll_set_natural_scroll_enabled(dev, tp->natural_scroll);
    }
    if (libinput_device_config_accel_is_available(dev)) {
        bool flat = tp->accel_profile && strcmp(tp->accel_profile, "flat") == 0;
        uint32_t profiles = libinput_device_config_accel_get_profiles(dev);
        enum libinput_config_accel_profile want = flat ? LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT
                                                       : LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
        if (profiles & want) {
            libinput_device_config_accel_set_profile(dev, want);
        }
        libinput_device_config_accel_set_speed(dev, tp->accel_speed);
    }
    wlr_log(WLR_DEBUG, "touchpad '%s': tap=%d natural=%d accel=%.2f/%s", device->name,
            tp->tap_to_click, tp->natural_scroll, tp->accel_speed,
            tp->accel_profile ? tp->accel_profile : "adaptive");
}
