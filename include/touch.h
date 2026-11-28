// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_TOUCH_H
#define WOODLAND_TOUCH_H

#include <stdbool.h>

struct woodland_server;
struct wlr_input_device;

/**
 * 触摸屏：把 cursor 的 touch_* 事件分发给客户端（wl_touch），不支持触摸的
 * 目标（SSD、菜单、桌面、旧客户端）退化成单指鼠标模拟。
 *
 * init 必须在 server->cursor / output_layout 就位之后；finish 与之严格逆序。
 */
bool woodland_touch_init(struct woodland_server *server);
void woodland_touch_finish(struct woodland_server *server);

/** 新触摸设备接入：挂到 cursor、按 output_name 映射输出、跟踪销毁。 */
void woodland_touch_new_device(struct woodland_server *server, struct wlr_input_device *device);

#endif
