// input_reader.cpp — 键鼠输入设备读取实现
#include "input_reader.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <dirent.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <linux/input.h>

namespace kma {

InputReader::InputReader() {
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
}
InputReader::~InputReader() {
    stop();
    for (auto& d : devs_) if (d.fd >= 0) close(d.fd);
    if (epoll_fd_ >= 0) close(epoll_fd_);
}

bool InputReader::probeDevice(int fd, InputDevInfo& info) {
    // 使用完整位图，避免 1UL<<code 在 code>=64 时溢出（BTN_LEFT=272 等）
    unsigned char evbit[EV_MAX / 8 + 1] = {0};
    unsigned char relbit[REL_MAX / 8 + 1] = {0};
    unsigned char keybit[KEY_MAX / 8 + 1] = {0};
    if (ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), &evbit) < 0) return false;
    ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relbit)), &relbit);
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), &keybit);

    char name[256] = {0};
    ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
    info.name = name;

    auto test_bit_arr = [](const unsigned char* bits, int code) {
        return (bits[code / 8] >> (code % 8)) & 1;
    };

    bool has_ev_rel = test_bit_arr(evbit, EV_REL);
    bool has_ev_key = test_bit_arr(evbit, EV_KEY);
    bool has_rel_xy = test_bit_arr(relbit, REL_X) && test_bit_arr(relbit, REL_Y);
    bool has_mouse_btn = test_bit_arr(keybit, BTN_LEFT) ||
                         test_bit_arr(keybit, BTN_RIGHT) ||
                         test_bit_arr(keybit, BTN_MIDDLE);
    info.is_mouse = has_ev_rel && (has_rel_xy || has_mouse_btn);

    // 键盘识别（放宽）：检查字母 A~Z 全部 + 常见修饰键 + 数字键 + 功能键
    // 某些精简键盘可能只报告部分键，放宽到只要有"多个 KEY 事件"就认为是键盘
    bool has_letter = false;
    for (int c = KEY_A; c <= KEY_Z; ++c) {  // KEY_A=30 ~ KEY_Z=44
        if (test_bit_arr(keybit, c)) { has_letter = true; break; }
    }
    bool has_common_key = test_bit_arr(keybit, KEY_SPACE) ||
                          test_bit_arr(keybit, KEY_ENTER) ||
                          test_bit_arr(keybit, KEY_LEFTSHIFT) ||
                          test_bit_arr(keybit, KEY_RIGHTSHIFT) ||
                          test_bit_arr(keybit, KEY_LEFTCTRL) ||
                          test_bit_arr(keybit, KEY_TAB) ||
                          test_bit_arr(keybit, KEY_BACKSPACE) ||
                          test_bit_arr(keybit, KEY_ESC);
    // 统计 KEY 事件总数，超过 10 个基本可以确定是键盘
    int key_count = 0;
    for (int c = 0; c < KEY_MAX && key_count < 11; ++c) {
        if (test_bit_arr(keybit, c)) ++key_count;
    }
    info.is_keyboard = has_ev_key && (has_letter || has_common_key || key_count >= 10);

    return info.is_keyboard || info.is_mouse;
}

int InputReader::scanAndOpen(const std::string& filter_name) {
    DIR* dir = opendir("/dev/input");
    if (!dir) { last_err_ = "无法打开 /dev/input"; return 0; }

    int opened = 0;
    struct dirent* ent;
    // 第一遍：找键鼠设备
    std::vector<std::string> all_event_paths;
    while ((ent = readdir(dir)) != nullptr) {
        if (strncmp(ent->d_name, "event", 5) != 0) continue;
        std::string path = std::string("/dev/input/") + ent->d_name;
        all_event_paths.push_back(path);

        InputDevInfo info;
        info.path = path;
        int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) continue;

        if (!probeDevice(fd, info)) { close(fd); continue; }

        // 名字过滤
        if (!filter_name.empty() && info.name.find(filter_name) == std::string::npos) {
            close(fd); continue;
        }

        info.fd = fd;
        devs_.push_back(info);
        ++opened;

        // 加入 epoll
        struct epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = fd;
        epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
    }
    closedir(dir);

    // Fallback：如果没检测到键鼠，打开所有 event 设备
    // （某些键鼠可能未被 probeDevice 识别，或用户想监听触摸屏/其他设备）
    if (opened == 0 && filter_name.empty()) {
        for (const auto& path : all_event_paths) {
            int fd = open(path.c_str(), O_RDONLY);
            if (fd < 0) continue;
            InputDevInfo info;
            info.path = path;
            info.is_keyboard = false;
            info.is_mouse = false;
            char name[256] = {0};
            ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
            info.name = name;
            info.fd = fd;
            devs_.push_back(info);
            ++opened;
            struct epoll_event ev{};
            ev.events = EPOLLIN;
            ev.data.fd = fd;
            epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
        }
        if (opened > 0) {
            last_err_ = "未识别到键鼠，已打开所有 event 设备作为 fallback";
        } else {
            last_err_ = "未找到任何输入设备";
        }
    } else if (opened == 0) {
        last_err_ = "未找到匹配 '" + filter_name + "' 的设备";
    }
    return opened;
}

bool InputReader::run() {
    if (devs_.empty()) { last_err_ = "无设备可监听"; return false; }
    running_ = true;

    const int MAX_EV = 16;
    struct epoll_event events[MAX_EV];
    struct input_event evbuf[64];

    while (running_) {
        int n = epoll_wait(epoll_fd_, events, MAX_EV, 100);
        if (n < 0) {
            if (errno == EINTR) continue;
            last_err_ = std::string("epoll_wait: ") + strerror(errno);
            return false;
        }
        for (int i = 0; i < n && running_; ++i) {
            int fd = events[i].data.fd;
            ssize_t r = read(fd, evbuf, sizeof(evbuf));
            if (r <= 0) continue;
            int cnt = (int)(r / sizeof(struct input_event));
            for (int j = 0; j < cnt && running_; ++j) {
                const auto& e = evbuf[j];
                if (!cb_) continue;

                // 找到设备信息判断键鼠
                const InputDevInfo* info = nullptr;
                for (const auto& d : devs_) if (d.fd == fd) { info = &d; break; }
                if (!info) continue;

                if (e.type == EV_KEY && (info->is_keyboard || info->is_mouse)) {
                    InputEvent ie;
                    ie.kind = e.value ? InputEv::PRESS_DOWN : InputEv::PRESS_UP;
                    ie.code = e.code;
                    cb_(ie);
                } else if (e.type == EV_REL && info->is_mouse) {
                    if (e.code == REL_X || e.code == REL_Y) {
                        // 累积同一帧的 X/Y：这里简化为单事件上报
                        InputEvent ie;
                        ie.kind = InputEv::REL_MOVE;
                        if (e.code == REL_X) ie.dx = e.value;
                        else ie.dy = e.value;
                        cb_(ie);
                    } else if (e.code == REL_WHEEL) {
                        InputEvent ie;
                        ie.kind = InputEv::WHEEL_SCROLL;
                        ie.value = e.value;
                        cb_(ie);
                    }
                }
            }
        }
    }
    return true;
}

void InputReader::stop() { running_ = false; }

} // namespace kma
