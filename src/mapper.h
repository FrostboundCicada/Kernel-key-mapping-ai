// mapper.h — 键鼠 → 触摸/陀螺仪 映射引擎
//
// 按键类型（KeyAction）:
//   CLICK     单击：按下时点击一次屏幕坐标，松开不动作
//   HOLD      长按：按下持续触摸该坐标，松开抬起
//   TOGGLE    锁定切换：按一次切换触摸状态（再按一次抬起）
//   REPEAT    连点：按住时以 repeat_ms 间隔连续点击该坐标
//   MOVE      移动：拖拽触点（配合鼠标位移驱动该点，压枪/拖动准星）
//   JOYSTICK  摇杆：以 (x,y) 为中心、radius 为半径的虚拟摇杆，
//             按住后可向各方向拖动，松开回中
//   WHEEL     轮盘：按住显示方向选项，向 8 方向拖动选择对应坐标
//   SENSITIVITY 灵敏度调节：按住增减(toggle 方向)全局灵敏度
//
// 鼠标移动（全局 mouse_mode）:
//   drag  : 鼠标相对位移直接驱动一个触摸点拖拽
//   gyro  : 鼠标位移转陀螺仪角速度（仅 TWT 后端，FPS 视角转动）
//
// Slot 分配：
//   HOLD/TOGGLE/REPEAT/MOVE/JOYSTICK 按键占用一个 slot。CLICK 不占长时 slot。
//   TWT 支持多 slot；aim_touch 风格单 slot，后按下者抢占。
#pragma once

#include "driver_adapter.h"
#include "input_reader.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace kma {

enum class KeyAction : int {
    CLICK = 0,       // 单击
    HOLD = 1,        // 长按
    TOGGLE = 2,      // 锁定切换
    REPEAT = 3,      // 连点
    MOVE = 4,        // 移动(拖拽触点)
    JOYSTICK = 5,    // 摇杆
    WHEEL = 6,       // 轮盘
    SENSITIVITY = 7, // 灵敏度调节
};

struct KeyMap {
    int code = 0;          // KEY_xxx / BTN_LEFT / BTN_RIGHT
    KeyAction action = KeyAction::HOLD;
    int x = 0, y = 0;      // 主坐标（摇杆中心 / 轮盘中心 / 点击位置）
    int repeat_ms = 50;    // REPEAT 连点间隔（毫秒）
    int radius = 120;      // JOYSTICK 摇杆半径
    // WHEEL 8 方向目标坐标（顺时针 0=上 1=右上 ... 7=左上），未设为 -1
    int wheel_dirs[8] = {-1,-1,-1,-1,-1,-1,-1,-1};
};

enum class MouseMode : int { DRAG, GYRO };

struct MapperConfig {
    int screen_w = 1080;
    int screen_h = 2400;
    DriverType driver = DriverType::NONE;  // NONE=auto
    MouseMode mouse_mode = MouseMode::GYRO;
    float sensitivity = 1.0f;       // drag 模式像素缩放
    float gyro_sensitivity = 8.0f;  // gyro 模式：1 像素位移 → 角速度系数
    std::vector<KeyMap> keys;
};

class Mapper {
public:
    Mapper();
    ~Mapper();

    // 从文件加载配置，失败返回 false
    bool loadConfig(const std::string& path);
    bool parseConfigText(const std::string& text);

    // 用给定配置初始化（不用文件）
    void setConfig(const MapperConfig& cfg) { cfg_ = cfg; }

    // ── 运行时增删按键映射 ──────────────────────────────
    // 添加或覆盖一个按键映射。返回是否成功（按键码合法）。
    bool addKeyMap(int code, KeyAction action, int x, int y);
    bool addKeyMap(const std::string& key_name, const std::string& action_str,
                   int x, int y);
    // 删除指定按键的映射，返回是否删除成功
    bool removeKeyMap(int code);
    bool removeKeyMap(const std::string& key_name);
    // 清空所有按键映射
    void clearKeyMaps();
    // 列出所有按键映射（用于交互工具/日志）
    std::string dumpKeyMaps() const;
    // 把当前配置（含按键映射）写回文件
    bool saveConfig(const std::string& path) const;

    // 初始化驱动（按配置 driver 字段探测）。成功返回 true
    bool initDriver();

    // 输入事件回调（绑定到 InputReader）
    void onInputEvent(const InputEvent& ev);

    // 鼠标拖拽/陀螺仪的周期性更新（gyro 模式下衰减归零）
    void tick();

    ITouchDriver* driver() { return driver_; }
    const MapperConfig& config() const { return cfg_; }
    std::string lastError() const { return last_err_; }

private:
    MapperConfig cfg_;
    ITouchDriver* driver_ = nullptr;
    bool owns_driver_ = false;

    // slot 管理：按键码 → slot
    int next_slot_ = 0;
    std::unordered_map<int, int> active_held_;   // code → slot（hold/toggle/move/joystick 活跃）
    // 拖拽用 slot（全局鼠标 MOVE 模式 或 MOVE 类型按键）
    int drag_slot_ = -1;
    int drag_x_ = 0, drag_y_ = 0;
    bool drag_active_ = false;

    // REPEAT 连点状态：code → {slot, 上次点击时间戳(ms)}
    struct RepeatState { int slot; int64_t last_click_ms; };
    std::unordered_map<int, RepeatState> repeat_active_;

    // gyro 累积速度（用于平滑/衰减）
    float gyro_vx_ = 0, gyro_vy_ = 0;

    // 当前时间戳(ms)
    static int64_t nowMs();

    // 查找按键映射
    const KeyMap* findKey(int code) const;
    int allocSlot();
    void releaseSlot(int slot);

    std::string last_err_;
};

// 按键名 ↔ code 转换（KEY_W / BTN_LEFT 等）
int keyNameToCode(const std::string& name);
const char* keyCodeToName(int code);

} // namespace kma
