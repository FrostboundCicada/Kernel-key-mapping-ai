// mapper.cpp — 映射引擎实现
#include "mapper.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <linux/input.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <chrono>

namespace kma {

// ── 按键名/code 表（常用键，可扩展）──────────────────
struct KeyNameEntry { const char* name; int code; };
static const KeyNameEntry kKeyNames[] = {
    // 字母
    {"KEY_A", KEY_A}, {"KEY_B", KEY_B}, {"KEY_C", KEY_C}, {"KEY_D", KEY_D},
    {"KEY_E", KEY_E}, {"KEY_F", KEY_F}, {"KEY_G", KEY_G}, {"KEY_H", KEY_H},
    {"KEY_I", KEY_I}, {"KEY_J", KEY_J}, {"KEY_K", KEY_K}, {"KEY_L", KEY_L},
    {"KEY_M", KEY_M}, {"KEY_N", KEY_N}, {"KEY_O", KEY_O}, {"KEY_P", KEY_P},
    {"KEY_Q", KEY_Q}, {"KEY_R", KEY_R}, {"KEY_S", KEY_S}, {"KEY_T", KEY_T},
    {"KEY_U", KEY_U}, {"KEY_V", KEY_V}, {"KEY_W", KEY_W}, {"KEY_X", KEY_X},
    {"KEY_Y", KEY_Y}, {"KEY_Z", KEY_Z},
    // 数字
    {"KEY_0", KEY_0}, {"KEY_1", KEY_1}, {"KEY_2", KEY_2}, {"KEY_3", KEY_3},
    {"KEY_4", KEY_4}, {"KEY_5", KEY_5}, {"KEY_6", KEY_6}, {"KEY_7", KEY_7},
    {"KEY_8", KEY_8}, {"KEY_9", KEY_9},
    // 功能
    {"KEY_SPACE", KEY_SPACE}, {"KEY_ENTER", KEY_ENTER}, {"KEY_ESC", KEY_ESC},
    {"KEY_TAB", KEY_TAB}, {"KEY_LEFTSHIFT", KEY_LEFTSHIFT},
    {"KEY_RIGHTSHIFT", KEY_RIGHTSHIFT}, {"KEY_LEFTCTRL", KEY_LEFTCTRL},
    {"KEY_RIGHTCTRL", KEY_RIGHTCTRL}, {"KEY_LEFTALT", KEY_LEFTALT},
    {"KEY_RIGHTALT", KEY_RIGHTALT}, {"KEY_CAPSLOCK", KEY_CAPSLOCK},
    {"KEY_BACKSPACE", KEY_BACKSPACE}, {"KEY_INSERT", KEY_INSERT},
    {"KEY_DELETE", KEY_DELETE}, {"KEY_HOME", KEY_HOME}, {"KEY_END", KEY_END},
    {"KEY_PAGEUP", KEY_PAGEUP}, {"KEY_PAGEDOWN", KEY_PAGEDOWN},
    // F1-F12
    {"KEY_F1", KEY_F1}, {"KEY_F2", KEY_F2}, {"KEY_F3", KEY_F3}, {"KEY_F4", KEY_F4},
    {"KEY_F5", KEY_F5}, {"KEY_F6", KEY_F6}, {"KEY_F7", KEY_F7}, {"KEY_F8", KEY_F8},
    {"KEY_F9", KEY_F9}, {"KEY_F10", KEY_F10}, {"KEY_F11", KEY_F11}, {"KEY_F12", KEY_F12},
    // 方向
    {"KEY_UP", KEY_UP}, {"KEY_DOWN", KEY_DOWN},
    {"KEY_LEFT", KEY_LEFT}, {"KEY_RIGHT", KEY_RIGHT},
    // 鼠标键
    {"MOUSE_LEFT", BTN_LEFT}, {"MOUSE_RIGHT", BTN_RIGHT},
    {"MOUSE_MIDDLE", BTN_MIDDLE}, {"MOUSE_SIDE", BTN_SIDE},
    {"MOUSE_EXTRA", BTN_EXTRA},
};

int keyNameToCode(const std::string& name) {
    for (const auto& e : kKeyNames) {
        if (name == e.name) return e.code;
    }
    // 允许直接写数字
    if (!name.empty() && (name[0] >= '0' && name[0] <= '9')) {
        return atoi(name.c_str());
    }
    return -1;
}

const char* keyCodeToName(int code) {
    for (const auto& e : kKeyNames) {
        if (code == e.code) return e.name;
    }
    return "UNKNOWN";
}

// ════════════════════════════════════════════════════════════

Mapper::Mapper() = default;
Mapper::~Mapper() {
    if (owns_driver_ && driver_) {
        driver_->touchUpAll();
        if (driver_->hasGyro()) driver_->gyroReset();
        delete driver_;
    }
}

bool Mapper::loadConfig(const std::string& path) {
    std::ifstream f(path);
    if (!f) { last_err_ = "无法打开配置: " + path; return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    return parseConfigText(ss.str());
}

static std::string toUpper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::toupper);
    return s;
}

bool Mapper::parseConfigText(const std::string& text) {
    std::istringstream is(text);
    std::string line;
    while (std::getline(is, line)) {
        // 去注释
        auto pos = line.find('#');
        if (pos != std::string::npos) line = line.substr(0, pos);
        std::istringstream ls(line);
        std::string key;
        if (!(ls >> key)) continue;
        key = toUpper(key);

        if (key == "SCREEN") {
            ls >> cfg_.screen_w >> cfg_.screen_h;
        } else if (key == "DRIVER") {
            std::string v; ls >> v; v = toUpper(v);
            if (v == "TWT") cfg_.driver = DriverType::TWT;
            else if (v == "INPUT") cfg_.driver = DriverType::INPUT_HANDLE;
            else cfg_.driver = DriverType::NONE; // auto
        } else if (key == "MOUSE_MODE") {
            std::string v; ls >> v; v = toUpper(v);
            cfg_.mouse_mode = (v == "GYRO") ? MouseMode::GYRO : MouseMode::DRAG;
        } else if (key == "SENSITIVITY") {
            ls >> cfg_.sensitivity;
        } else if (key == "GYRO_SENSITIVITY") {
            ls >> cfg_.gyro_sensitivity;
        } else if (key == "KEY" || key == "MOUSE_LEFT" || key == "MOUSE_RIGHT" ||
                   key == "MOUSE_MIDDLE" || key == "MOUSE_SIDE" || key == "MOUSE_EXTRA") {
            // 两种写法：
            //   key KEY_W hold 540 1700
            //   mouse_left hold 540 1200
            std::string kname, act;
            int x, y;
            int code;
            if (key == "KEY") {
                if (!(ls >> kname >> act >> x >> y)) continue;
                code = keyNameToCode(toUpper(kname));
            } else {
                if (!(ls >> act >> x >> y)) continue;
                code = keyNameToCode(key);
            }
            act = toUpper(act);
            KeyAction ka = KeyAction::HOLD;
            if (act == "CLICK") ka = KeyAction::CLICK;
            else if (act == "TOGGLE") ka = KeyAction::TOGGLE;
            else ka = KeyAction::HOLD;
            if (code < 0) { last_err_ = "未知按键: " + kname; continue; }
            cfg_.keys.push_back({code, ka, x, y});
        }
    }
    return true;
}

// 字符串 → KeyAction，未知串默认 HOLD
static KeyAction parseAction(const std::string& s) {
    std::string u = toUpper(s);
    if (u == "CLICK")     return KeyAction::CLICK;
    if (u == "TOGGLE")    return KeyAction::TOGGLE;
    if (u == "REPEAT")    return KeyAction::REPEAT;
    if (u == "MOVE")      return KeyAction::MOVE;
    if (u == "JOYSTICK" || u == "JOY") return KeyAction::JOYSTICK;
    if (u == "WHEEL")     return KeyAction::WHEEL;
    if (u == "SENSITIVITY" || u == "SENS") return KeyAction::SENSITIVITY;
    return KeyAction::HOLD;
}
static const char* actionName(KeyAction a) {
    switch (a) {
    case KeyAction::CLICK:       return "click";
    case KeyAction::HOLD:        return "hold";
    case KeyAction::TOGGLE:      return "toggle";
    case KeyAction::REPEAT:      return "repeat";
    case KeyAction::MOVE:        return "move";
    case KeyAction::JOYSTICK:    return "joystick";
    case KeyAction::WHEEL:       return "wheel";
    case KeyAction::SENSITIVITY: return "sensitivity";
    }
    return "hold";
}

bool Mapper::addKeyMap(int code, KeyAction action, int x, int y) {
    if (code < 0) { last_err_ = "无效按键码"; return false; }
    // 覆盖已有同 code 映射，保留 repeat_ms/radius/wheel_dirs（除非显式重设）
    for (auto& k : cfg_.keys) {
        if (k.code == code) { k.action = action; k.x = x; k.y = y; return true; }
    }
    KeyMap km;
    km.code = code; km.action = action; km.x = x; km.y = y;
    cfg_.keys.push_back(km);
    return true;
}

bool Mapper::addKeyMap(const std::string& key_name, const std::string& action_str,
                       int x, int y) {
    int code = keyNameToCode(toUpper(key_name));
    if (code < 0) { last_err_ = "未知按键名: " + key_name; return false; }
    return addKeyMap(code, parseAction(action_str), x, y);
}

bool Mapper::removeKeyMap(int code) {
    for (auto it = cfg_.keys.begin(); it != cfg_.keys.end(); ++it) {
        if (it->code == code) { cfg_.keys.erase(it); return true; }
    }
    return false;
}

bool Mapper::removeKeyMap(const std::string& key_name) {
    int code = keyNameToCode(toUpper(key_name));
    if (code < 0) return false;
    return removeKeyMap(code);
}

void Mapper::clearKeyMaps() { cfg_.keys.clear(); }

std::string Mapper::dumpKeyMaps() const {
    std::ostringstream os;
    os << "按键映射 (" << cfg_.keys.size() << " 条):\n";
    for (const auto& k : cfg_.keys) {
        os << "  " << keyCodeToName(k.code) << "  " << actionName(k.action)
           << "  (" << k.x << "," << k.y << ")";
        if (k.action == KeyAction::REPEAT)    os << "  interval=" << k.repeat_ms;
        if (k.action == KeyAction::JOYSTICK)  os << "  r=" << k.radius;
        os << "\n";
    }
    return os.str();
}

bool Mapper::saveConfig(const std::string& path) const {
    std::ofstream f(path);
    if (!f) { return false; }
    f << "# Kernel-key-mapping-ai 配置 (由程序生成)\n";
    f << "screen " << cfg_.screen_w << " " << cfg_.screen_h << "\n";
    const char* drv = cfg_.driver == DriverType::TWT ? "twt"
                    : cfg_.driver == DriverType::INPUT_HANDLE ? "input" : "auto";
    f << "driver " << drv << "\n";
    f << "mouse_mode " << (cfg_.mouse_mode == MouseMode::GYRO ? "gyro" : "drag") << "\n";
    f << "sensitivity " << cfg_.sensitivity << "\n";
    f << "gyro_sensitivity " << cfg_.gyro_sensitivity << "\n\n";
    for (const auto& k : cfg_.keys) {
        f << keyCodeToName(k.code) << " " << actionName(k.action)
          << " " << k.x << " " << k.y << "\n";
    }
    return true;
}

bool Mapper::initDriver() {
    if (driver_) return true;
    driver_ = DriverManager::probe(cfg_.driver, cfg_.screen_w, cfg_.screen_h);
    if (!driver_) {
        last_err_ = "未探测到任何可用内核驱动（twt/rt/qx/zero...）";
        return false;
    }
    owns_driver_ = true;
    return true;
}

const KeyMap* Mapper::findKey(int code) const {
    for (const auto& k : cfg_.keys) if (k.code == code) return &k;
    return nullptr;
}

int Mapper::allocSlot() { return next_slot_++; }
void Mapper::releaseSlot(int /*slot*/) {
    // 简单实现：slot 号不复用（twt 支持任意 slot；aim_touch 单 slot 抢占）
}

int64_t Mapper::nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void Mapper::onInputEvent(const InputEvent& ev) {
    if (!driver_ || !driver_->isReady()) return;

    switch (ev.kind) {
    case InputEv::PRESS_DOWN:
    case InputEv::PRESS_UP: {
        const KeyMap* km = findKey(ev.code);
        if (!km) break;
        bool down = (ev.kind == InputEv::PRESS_DOWN);

        switch (km->action) {
        case KeyAction::CLICK:
            if (down) {
                int s = allocSlot();
                driver_->touchDown(s, km->x, km->y);
                driver_->touchUp(s);
                releaseSlot(s);
            }
            break;
        case KeyAction::HOLD:
        case KeyAction::MOVE:
        case KeyAction::JOYSTICK:
            if (down) {
                if (active_held_.count(ev.code)) break;
                int s = allocSlot();
                active_held_[ev.code] = s;
                driver_->touchDown(s, km->x, km->y);
            } else {
                auto it = active_held_.find(ev.code);
                if (it == active_held_.end()) break;
                driver_->touchUp(it->second);
                active_held_.erase(it);
            }
            break;
        case KeyAction::TOGGLE:
            if (!down) break;
            {
                auto it = active_held_.find(ev.code);
                if (it == active_held_.end()) {
                    int s = allocSlot();
                    active_held_[ev.code] = s;
                    driver_->touchDown(s, km->x, km->y);
                } else {
                    driver_->touchUp(it->second);
                    active_held_.erase(it);
                }
            }
            break;
        case KeyAction::REPEAT:
            if (down) {
                if (repeat_active_.count(ev.code)) break;
                int s = allocSlot();
                // 立即点击一次，再记录待 tick 连点
                driver_->touchDown(s, km->x, km->y);
                driver_->touchUp(s);
                repeat_active_[ev.code] = {s, nowMs()};
            } else {
                auto it = repeat_active_.find(ev.code);
                if (it == repeat_active_.end()) break;
                repeat_active_.erase(it);
            }
            break;
        case KeyAction::WHEEL:
            // 轮盘：按下显示方向选择，此处简化为按下中心点，松开时按位移方向触发
            // 完整轮盘 UI 在悬浮窗层处理，内核层仅占 slot
            if (down) {
                if (active_held_.count(ev.code)) break;
                int s = allocSlot();
                active_held_[ev.code] = s;
                driver_->touchDown(s, km->x, km->y);
            } else {
                auto it = active_held_.find(ev.code);
                if (it == active_held_.end()) break;
                driver_->touchUp(it->second);
                active_held_.erase(it);
            }
            break;
        case KeyAction::SENSITIVITY:
            // 灵敏度调节：按下增，松开不动（或双键增减）。此处按下时 +0.1
            if (down) {
                cfg_.sensitivity += 0.1f;
                if (cfg_.sensitivity > 10.0f) cfg_.sensitivity = 10.0f;
            }
            break;
        }
        break;
    }
    case InputEv::REL_MOVE: {
        if (cfg_.mouse_mode == MouseMode::DRAG) {
            if (!drag_active_) {
                drag_slot_ = allocSlot();
                drag_x_ = cfg_.screen_w / 2;
                drag_y_ = cfg_.screen_h / 2;
                driver_->touchDown(drag_slot_, drag_x_, drag_y_);
                drag_active_ = true;
            }
            drag_x_ += (int)(ev.dx * cfg_.sensitivity);
            drag_y_ += (int)(ev.dy * cfg_.sensitivity);
            drag_x_ = std::max(0, std::min(drag_x_, cfg_.screen_w - 1));
            drag_y_ = std::max(0, std::min(drag_y_, cfg_.screen_h - 1));
            driver_->touchMove(drag_slot_, drag_x_, drag_y_);
        } else { // GYRO
            if (!driver_->hasGyro()) {
                // 无陀螺仪后端，退化为拖拽
                if (!drag_active_) {
                    drag_slot_ = allocSlot();
                    drag_x_ = cfg_.screen_w / 2;
                    drag_y_ = cfg_.screen_h / 2;
                    driver_->touchDown(drag_slot_, drag_x_, drag_y_);
                    drag_active_ = true;
                }
                drag_x_ += (int)(ev.dx * cfg_.sensitivity);
                drag_y_ += (int)(ev.dy * cfg_.sensitivity);
                drag_x_ = std::max(0, std::min(drag_x_, cfg_.screen_w - 1));
                drag_y_ = std::max(0, std::min(drag_y_, cfg_.screen_h - 1));
                driver_->touchMove(drag_slot_, drag_x_, drag_y_);
                break;
            }
            gyro_vx_ = ev.dy * cfg_.gyro_sensitivity;
            gyro_vy_ = ev.dx * cfg_.gyro_sensitivity;
            driver_->gyroInject(gyro_vx_, gyro_vy_, 0.0f);
        }
        break;
    }
    case InputEv::WHEEL_SCROLL:
        break;
    }
}

void Mapper::tick() {
    // REPEAT 连点：检查所有活跃连点按键，到间隔则再点一次
    if (!repeat_active_.empty() && driver_ && driver_->isReady()) {
        int64_t now = nowMs();
        for (auto& [code, st] : repeat_active_) {
            const KeyMap* km = findKey(code);
            if (!km) continue;
            if (now - st.last_click_ms >= km->repeat_ms) {
                driver_->touchDown(st.slot, km->x, km->y);
                driver_->touchUp(st.slot);
                st.last_click_ms = now;
            }
        }
    }
    // gyro 衰减
    if (cfg_.mouse_mode == MouseMode::GYRO && driver_ && driver_->hasGyro()) {
        if (gyro_vx_ != 0.0f || gyro_vy_ != 0.0f) {
            gyro_vx_ *= 0.6f;
            gyro_vy_ *= 0.6f;
            if (std::fabs(gyro_vx_) < 0.5f) gyro_vx_ = 0.0f;
            if (std::fabs(gyro_vy_) < 0.5f) gyro_vy_ = 0.0f;
            if (gyro_vx_ == 0.0f && gyro_vy_ == 0.0f) {
                driver_->gyroReset();
            } else {
                driver_->gyroInject(gyro_vx_, gyro_vy_, 0.0f);
            }
        }
    }
}

} // namespace kma
