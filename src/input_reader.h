// input_reader.h — 键鼠输入设备读取（/dev/input/event*）
//
// 职责：
//   1. 扫描 /dev/input/event*，通过 EVIOCGBIT 识别键盘设备（EV_KEY 含字母键）
//      和鼠标设备（EV_REL 含 REL_X/REL_Y，EV_KEY 含 BTN_LEFT 等）。
//   2. 用 epoll 同时监听多个设备 fd，解析 input_event。
//   3. 通过回调把"按键按下/抬起"、"鼠标相对位移"、"滚轮"上报给映射引擎。
//
// 说明：在 Android 上读取 /dev/input/event* 需要 root 或对应权限
//      （chmod 666 /dev/input/event* 或将应用加入 input 组）。
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace kma {

// 输入事件类型
// 注：枚举值刻意避开 linux/input.h 中的 KEY_DOWN/KEY_UP 等宏，防止宏污染
enum class InputEv : int {
    PRESS_DOWN,    // 键盘/鼠标按键按下，code = KEY_xxx / BTN_xxx
    PRESS_UP,      // 键盘/鼠标按键抬起
    REL_MOVE,      // 鼠标相对移动，dx/dy
    WHEEL_SCROLL,     // 滚轮，value（+1 向上，-1 向下）
};

struct InputEvent {
    InputEv kind;
    int code = 0;   // PRESS_DOWN/UP: 按键码；其它无效
    int dx = 0;     // REL_MOVE
    int dy = 0;
    int value = 0;  // WHEEL_SCROLL
};

// 设备信息
struct InputDevInfo {
    std::string path;     // /dev/input/eventN
    std::string name;     // 设备名
    bool is_keyboard = false;
    bool is_mouse = false;
    int fd = -1;
};

using InputCallback = std::function<void(const InputEvent&)>;

class InputReader {
public:
    InputReader();
    ~InputReader();

    InputReader(const InputReader&) = delete;
    InputReader& operator=(const InputReader&) = delete;

    // 扫描并打开所有键鼠设备。返回成功打开的设备数。
    // filter_name 非空时只打开名字含该串的设备（用于精确指定物理外设）。
    int scanAndOpen(const std::string& filter_name = "");

    // 注册事件回调
    void setCallback(InputCallback cb) { cb_ = std::move(cb); }

    // 阻塞等待并派发事件，直到 stop() 被调用。
    // 返回 false 表示无可监听设备或出错。
    bool run();

    // 在另一线程调用以停止 run()
    void stop();

    const std::vector<InputDevInfo>& devices() const { return devs_; }
    std::string lastError() const { return last_err_; }

private:
    // 判断设备能力
    bool probeDevice(int fd, InputDevInfo& info);
    std::vector<InputDevInfo> devs_;
    InputCallback cb_;
    int epoll_fd_ = -1;
    bool running_ = false;
    std::string last_err_;
};

} // namespace kma
