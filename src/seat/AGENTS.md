# Seat 模块智能体指南

## 1. 模块职责与核心文件

`src/seat` 负责输入设备、Seat 交互状态与控制台会话（VT）管理：
- `src/seat/seat.c`：`wlr_seat` 初始化与管理、键盘/指针/触控设备热插拔、按键分发、XKB 键位转换与 VT 切换逻辑。

- `src/seat/touch.c`：触摸屏。cursor 的 `touch_*` 事件分两路——目标接受 touch 则走 `wlr_seat_touch_notify_*`（落点记 layout→surface 偏移）；落在 SSD/菜单/切换器/桌面/旧客户端上则退化为单指鼠标左键模拟（复用 `woodland_cursor_handle_button`，同一时刻仅一个模拟触点）。触摸屏按 `output_name` 映射输出，布局变化时重映射。
- `src/seat/input_config.c`：把 `[input.touchpad]`（tap/自然滚动/加速）落到 libinput 触摸板设备；鼠标、触摸屏、非 libinput 设备不受影响。

## 2. 关键硬约束

1. **VT 切换最高优先级**：
   - `Ctrl+Alt+F1..F12` 经 XKB 翻成 `XF86Switch_VT_1..12`，其判定次序排在锁屏和快捷键抑制（shortcuts-inhibit）之前，作为系统逃生通道。
2. **孤儿 Release 事件吞噬**：
   - VT 按键的 press 被合成器消费后，其对应的 release 必须丢弃并记录在 `server->vt_swallowed_keys` 位图中，防止切换回桌面时客户端收到孤立的 release。
3. **Session Inactive 守卫**：
   - 会话非激活时，渲染循环必须跳过 `handle_frame`；输入设备休眠与恢复由 wlroots session 内部自动处理，严禁手动监听 `session_active` 信号。
4. **快捷键转发链**：
   - 键盘事件优先经过合成器自身处理与全局快捷键分发（`woodland_protocol_shortcuts_key`），未消费按键才推送到键盘焦点的客户端。

## 3. 相关文档

- 陷阱参考：[docs/pitfalls/protocol-implementations.md](../../docs/pitfalls/protocol-implementations.md)（VT 切换语义与孤儿按键吞噬）

## 4. 触摸注意事项

- seat capabilities 统一经 `woodland_seat_update_capabilities()` 重算（含 touch），不要直接 `wlr_seat_set_capabilities`。
- 触摸设备销毁时必须释放其未抬起的触点（模拟路补发左键 release），否则按键状态会卡住。
- 触摸拖拽（`wlr_seat_start_touch_drag`）尚未接入，见 `docs/clipboard-dnd.md` §9.4。
