// mapper.h — 键鼠 → 触摸/陀螺仪 映射引擎
//
// 配置模型：
//   1. 按键映射：每个按键码(KEY_W / BTN_LEFT 等) → 一个映射项
//      - click : 按下时点击一次屏幕坐标，松开不动作
//      - hold  : 按下持续触摸该坐标，松开抬起
//      - toggle: 按下切换该坐标的触摸状态（再按一次抬起）
//   2. 鼠标移动：
//      - drag  : 鼠标相对位移直接驱动一个触摸点拖拽（用于压枪/拖动准星）
//      - gyro  : 鼠标位移转陀螺仪角速度（仅 TWT 后端，FPS 视角转动）
//   3. 鼠标左/右键同样可作为按键映射到屏幕坐标（开火/瞄准）
//
// Slot 分配：
//   每个 hold/toggle 按键占用一个 slot。click 不占用长时 slot。
//   TWT 支持多 slot；aim_touch 风格单 slot，后按下者抢占。
//
// 配置文件格式（简洁文本，每行一条）：
//   screen <w> <h>
//   driver auto|twt|input
//   mouse_mode drag|gyro
//   sensitivity <float>
//   gyro_sensitivity <float>
//   key <KEY_NAME> click|hold|toggle <x> <y>
//   mouse_left click|hold|toggle <x> <y>
//   mouse_right click|hold|toggle <x> <y>
//   # 注释行
#pragma once

#include "driver_adapter.h"
#include "input_reader.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace kma {

enum class KeyAction : int { CLICK, HOLD, TOGGLE };

struct KeyMap {
    int code;           // KEY_xxx / BTN_LEFT / BTN_RIGHT
    KeyAction action;
    int x, y;           // 屏幕坐标
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
    std::unordered_map<int, int> active_held_;   // code → slot（hold/toggle 活跃）
    // 拖拽用 slot
    int drag_slot_ = -1;
    int drag_x_ = 0, drag_y_ = 0;
    bool drag_active_ = false;

    // gyro 累积速度（用于平滑/衰减）
    float gyro_vx_ = 0, gyro_vy_ = 0;

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
