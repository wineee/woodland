// SPDX-License-Identifier: GPL-2.0-only
/*
 * 触摸屏支持（参考 labwc src/input/touch.c）。
 *
 * 每个触点按落点分两路：
 *   - 客户端路：目标 surface 接受 touch，直接 wlr_seat_touch_notify_*。落点时记下
 *     "layout -> surface" 偏移，之后的 motion 都按它换算，手指滑出窗口也不会跳坐标。
 *   - 模拟路：落在 SSD / 菜单 / 切换器 / 桌面 / 不支持 touch 的客户端上，走
 *     woodland_cursor_handle_button，等价于一次鼠标左键。同一时刻只有一个触点
 *     驱动光标，其余模拟触点忽略，免得多指把按键状态搅乱。
 */
#include <stdlib.h>

#include <linux/input-event-codes.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_touch.h>
#include <wlr/util/log.h>

#include "layer.h"
#include "multitask_view.h"
#include "seat.h"
#include "session_lock.h"
#include "touch.h"
#include "util/listener.h"
#include "view.h"
#include "window_menu.h"
#include "window_switcher.h"
#include "woodland.h"

struct woodland_touch_device {
    struct wl_list link; // woodland_server.touch_devices
    struct woodland_server *server;
    struct wlr_touch *touch;
    struct wl_listener destroy;
};

struct woodland_touch_point {
    struct wl_list link; // woodland_server.touch_points
    int32_t id;
    struct wlr_touch *touch;
    bool emulated; // true：鼠标模拟路；false：客户端路
    double x_offset, y_offset;
};

static struct woodland_touch_point *point_find(struct woodland_server *server, int32_t id)
{
    struct woodland_touch_point *p;
    wl_list_for_each(p, &server->touch_points, link) {
        if (p->id == id) {
            return p;
        }
    }
    return NULL;
}

static bool emulation_busy(struct woodland_server *server)
{
    struct woodland_touch_point *p;
    wl_list_for_each(p, &server->touch_points, link) {
        if (p->emulated) {
            return true;
        }
    }
    return false;
}

static void point_free(struct woodland_touch_point *p)
{
    wl_list_remove(&p->link);
    free(p);
}

/* 模拟路的按键：先把光标挪到触点，再走与物理指针相同的按键处理 */
static void emulate_motion(struct woodland_server *server, struct wlr_touch *touch, double x,
                           double y, uint32_t time)
{
    wlr_cursor_warp_absolute(server->cursor, &touch->base, x, y);
    woodland_cursor_process_motion(server, time);
    wlr_seat_pointer_notify_frame(server->seat);
}

static void emulate_button(struct woodland_server *server, uint32_t time,
                           enum wl_pointer_button_state state)
{
    woodland_cursor_handle_button(server, time, BTN_LEFT, state);
    wlr_seat_pointer_notify_frame(server->seat);
}

/* 释放一个触点（up / cancel / 设备销毁共用）；调用后 p 已被释放 */
static void point_release(struct woodland_server *server, struct woodland_touch_point *p,
                          uint32_t time, bool cancel)
{
    if (p->emulated) {
        emulate_button(server, time, WL_POINTER_BUTTON_STATE_RELEASED);
    } else {
        struct wlr_touch_point *sp = wlr_seat_touch_get_point(server->seat, p->id);
        if (sp && cancel && sp->client) {
            wlr_seat_touch_notify_cancel(server->seat, sp->client);
        } else if (sp) {
            wlr_seat_touch_notify_up(server->seat, time, p->id);
        }
        wlr_seat_touch_notify_frame(server->seat);
    }
    point_free(p);
}

/*
 * 落点命中判定。返回客户端路要用的 surface（模拟路返回 NULL），并给出
 * layout 坐标与 surface 偏移。
 */
static struct wlr_surface *touch_target(struct woodland_server *server, double lx, double ly,
                                        double *x_off, double *y_off, struct woodland_view **view_out)
{
    double sx = 0, sy = 0;
    struct wlr_surface *surface = NULL;
    *view_out = NULL;

    if (woodland_session_lock_is_locked(server)) {
        woodland_session_lock_surface_at(server, lx, ly, &surface, &sx, &sy);
    } else {
        /* 这些状态下一律模拟：第一下触摸要像鼠标点击一样收起菜单 / 面板 */
        if (woodland_window_menu_is_active(server) || woodland_multitask_view_is_active(server) ||
            woodland_window_switcher_is_active(server) || woodland_dnd_active(server)) {
            return NULL;
        }
        struct woodland_view *view = woodland_view_at(server, lx, ly, &surface, &sx, &sy);
        /* 高于命中视图的 SSD（标题栏 / 边框 / 按钮）必须走模拟，才能拖动与缩放 */
        enum woodland_ssd_part part = WOODLAND_SSD_NONE;
        uint32_t edges = 0;
        if (woodland_cursor_ssd_at(server, view, lx, ly, &part, &edges)) {
            return NULL;
        }
        *view_out = view;
    }
    if (!surface || !wlr_surface_accepts_touch(surface, server->seat)) {
        return NULL;
    }
    *x_off = lx - sx;
    *y_off = ly - sy;
    return surface;
}

static void touch_handle_down(struct wl_listener *listener, void *data)
{
    struct woodland_server *server = wl_container_of(listener, server, touch_down);
    struct wlr_touch_down_event *event = data;

    struct woodland_touch_point *p = calloc(1, sizeof(*p));
    if (!p) {
        return;
    }
    p->id = event->touch_id;
    p->touch = event->touch;

    double lx, ly;
    wlr_cursor_absolute_to_layout_coords(server->cursor, &event->touch->base, event->x, event->y,
                                         &lx, &ly);
    struct woodland_view *view = NULL;
    struct wlr_surface *surface = touch_target(server, lx, ly, &p->x_offset, &p->y_offset, &view);

    if (surface) {
        wl_list_insert(&server->touch_points, &p->link);
        /* 触摸期间指针不该再占着焦点，否则客户端同时收到 pointer 与 touch */
        wlr_seat_pointer_notify_clear_focus(server->seat);
        /* 点击聚焦：与指针按下一致（view 得键盘焦点，layer 按 interactivity 决定） */
        if (view) {
            woodland_focus_view(view, true);
        } else {
            struct wlr_surface *lsurf = NULL;
            double lsx, lsy;
            struct wlr_layer_surface_v1 *layer =
                woodland_layer_surface_at(server, lx, ly, &lsurf, &lsx, &lsy);
            if (layer) {
                woodland_layer_try_focus(server, layer);
            }
        }
        wlr_seat_touch_notify_down(server->seat, surface, event->time_msec, event->touch_id,
                                   lx - p->x_offset, ly - p->y_offset);
        return;
    }

    if (emulation_busy(server)) {
        free(p); /* 已有触点在驱动光标：多余的模拟触点直接忽略 */
        return;
    }
    p->emulated = true;
    wl_list_insert(&server->touch_points, &p->link);
    emulate_motion(server, event->touch, event->x, event->y, event->time_msec);
    emulate_button(server, event->time_msec, WL_POINTER_BUTTON_STATE_PRESSED);
}

static void touch_handle_up(struct wl_listener *listener, void *data)
{
    struct woodland_server *server = wl_container_of(listener, server, touch_up);
    struct wlr_touch_up_event *event = data;
    struct woodland_touch_point *p = point_find(server, event->touch_id);
    if (p) {
        point_release(server, p, event->time_msec, false);
    }
}

static void touch_handle_motion(struct wl_listener *listener, void *data)
{
    struct woodland_server *server = wl_container_of(listener, server, touch_motion);
    struct wlr_touch_motion_event *event = data;
    struct woodland_touch_point *p = point_find(server, event->touch_id);
    if (!p) {
        return;
    }
    if (p->emulated) {
        emulate_motion(server, event->touch, event->x, event->y, event->time_msec);
        return;
    }
    double lx, ly;
    wlr_cursor_absolute_to_layout_coords(server->cursor, &event->touch->base, event->x, event->y,
                                         &lx, &ly);
    wlr_seat_touch_notify_motion(server->seat, event->time_msec, event->touch_id,
                                 lx - p->x_offset, ly - p->y_offset);
}

static void touch_handle_cancel(struct wl_listener *listener, void *data)
{
    struct woodland_server *server = wl_container_of(listener, server, touch_cancel);
    struct wlr_touch_cancel_event *event = data;
    struct woodland_touch_point *p = point_find(server, event->touch_id);
    if (p) {
        point_release(server, p, event->time_msec, true);
    }
    /* notify_cancel 会连带取消同一客户端的其余触点：把本地已失效的清掉 */
    struct woodland_touch_point *it, *tmp;
    wl_list_for_each_safe(it, tmp, &server->touch_points, link) {
        if (!it->emulated && !wlr_seat_touch_get_point(server->seat, it->id)) {
            point_free(it);
        }
    }
}

static void touch_handle_frame(struct wl_listener *listener, void *data)
{
    struct woodland_server *server = wl_container_of(listener, server, touch_frame);
    wlr_seat_touch_notify_frame(server->seat);
    (void)data;
}

/* ==================== 输出映射与设备 ==================== */

/* 触摸屏绑定的输出名可能晚于设备出现（热插拔），所以布局变化时重映射 */
static void touch_map_output(struct woodland_server *server, struct wlr_touch *touch)
{
    struct wlr_output *output = NULL;
    if (touch->output_name) {
        struct wlr_output_layout_output *lo;
        wl_list_for_each(lo, &server->output_layout->outputs, link) {
            if (lo->output->name && strcmp(lo->output->name, touch->output_name) == 0) {
                output = lo->output;
                break;
            }
        }
    }
    /* output 为 NULL 即取消映射：覆盖整个 layout，总好过落在错误的屏上 */
    wlr_cursor_map_input_to_output(server->cursor, &touch->base, output);
}

static void touch_handle_layout_change(struct wl_listener *listener, void *data)
{
    struct woodland_server *server = wl_container_of(listener, server, touch_layout_change);
    struct woodland_touch_device *dev;
    wl_list_for_each(dev, &server->touch_devices, link) {
        touch_map_output(server, dev->touch);
    }
    (void)data;
}

static void touch_device_handle_destroy(struct wl_listener *listener, void *data)
{
    struct woodland_touch_device *dev = wl_container_of(listener, dev, destroy);
    struct woodland_server *server = dev->server;

    struct woodland_touch_point *p, *tmp;
    wl_list_for_each_safe(p, tmp, &server->touch_points, link) {
        if (p->touch == dev->touch) {
            point_release(server, p, 0, true);
        }
    }
    wl_list_remove(&dev->destroy.link);
    wl_list_remove(&dev->link);
    free(dev);
    woodland_seat_update_capabilities(server);
    (void)data;
}

void woodland_touch_new_device(struct woodland_server *server, struct wlr_input_device *device)
{
    struct wlr_touch *touch = wlr_touch_from_input_device(device);
    struct woodland_touch_device *dev = calloc(1, sizeof(*dev));
    if (!dev) {
        return;
    }
    dev->server = server;
    dev->touch = touch;
    wlr_cursor_attach_input_device(server->cursor, device);
    touch_map_output(server, touch);
    woodland_listener_connect(&dev->destroy, &device->events.destroy,
                              touch_device_handle_destroy, "touch.destroy");
    wl_list_insert(&server->touch_devices, &dev->link);
    wlr_log(WLR_INFO, "touch device '%s' attached (output=%s)", device->name,
            touch->output_name ? touch->output_name : "-");
}

bool woodland_touch_init(struct woodland_server *server)
{
    woodland_listener_connect(&server->touch_down, &server->cursor->events.touch_down,
                              touch_handle_down, "touch.down");
    woodland_listener_connect(&server->touch_up, &server->cursor->events.touch_up,
                              touch_handle_up, "touch.up");
    woodland_listener_connect(&server->touch_motion, &server->cursor->events.touch_motion,
                              touch_handle_motion, "touch.motion");
    woodland_listener_connect(&server->touch_cancel, &server->cursor->events.touch_cancel,
                              touch_handle_cancel, "touch.cancel");
    woodland_listener_connect(&server->touch_frame, &server->cursor->events.touch_frame,
                              touch_handle_frame, "touch.frame");
    woodland_listener_connect(&server->touch_layout_change,
                              &server->output_layout->events.change,
                              touch_handle_layout_change, "touch.layout_change");
    return true;
}

static void remove_listener(struct wl_listener *l)
{
    if (l->link.prev) {
        wl_list_remove(&l->link);
        l->link.prev = NULL;
        l->link.next = NULL;
    }
}

void woodland_touch_finish(struct woodland_server *server)
{
    if (!server) {
        return;
    }
    remove_listener(&server->touch_layout_change);
    remove_listener(&server->touch_frame);
    remove_listener(&server->touch_cancel);
    remove_listener(&server->touch_motion);
    remove_listener(&server->touch_up);
    remove_listener(&server->touch_down);
    struct woodland_touch_point *p, *ptmp;
    wl_list_for_each_safe(p, ptmp, &server->touch_points, link) {
        point_free(p);
    }
}
