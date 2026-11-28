// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_INPUT_CONFIG_H
#define WOODLAND_INPUT_CONFIG_H

struct woodland_server;
struct wlr_input_device;

/**
 * 把 [input.touchpad] 配置落到 libinput 设备上。
 *
 * 只对 libinput 后端的触摸板生效（支持 tap 的 pointer 设备）；鼠标、触摸屏、
 * headless / 虚拟设备一律原样放过。设备热插拔时由 seat 逐个调用。
 */
void woodland_input_configure(struct woodland_server *server, struct wlr_input_device *device);

#endif
