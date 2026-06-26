// driver_adapter.cpp — 多内核驱动适配层实现
//
// 实现细节：
//   * TwtDriver 完全遵循上传文件夹 kernel.h 中的对接约定：
//       - MY_CALL(0x114514, 0x1919810, 0x2778, &fd) 通过劫持 reboot 系统调用取 fd
//       - ioctl 命令 TOUCH_INIT/DOWN/UP('T',6/7/8)、GYRO_INIT/CONFIG('T',9/10)
//       - touch_event_base { int slot; int x; int y; }
//       - gyro_config { uint32_t enable; uint32_t x; uint32_t y; }  其中 x/y 是 float memcpy
//   * InputHandleDriver 遵循 FpsAimAssistant/kernel/touch_driver.c 的 ioctl 'A' 约定，
//     适用于 rt/qx/zero/ovo/hakutaku 等同类驱动（设备节点不同，命令集一致）。
#include "driver_adapter.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/input.h>
#include <random>
#include <algorithm>

#ifdef __aarch64__
#define TWT_HAS_SYSCALL 1
#else
#define TWT_HAS_SYSCALL 0
#endif

namespace kma {

// ── TwT 驱动 ioctl 命令（来自 kernel.h）──────────────
#define TWT_MARK 'T'
struct TwtTouchEvent { int slot; int x; int y; };
struct TwtGyroConfig { uint32_t enable; uint32_t x; uint32_t y; };
#define TWT_TOUCH_INIT  _IOW(TWT_MARK, 6, struct TwtTouchEvent)
#define TWT_TOUCH_DOWN  _IOW(TWT_MARK, 7, struct TwtTouchEvent)
#define TWT_TOUCH_UP    _IOW(TWT_MARK, 8, struct TwtTouchEvent)
#define TWT_GYRO_INIT   _IOW(TWT_MARK, 9, int)
#define TWT_GYRO_CONFIG _IOWR(TWT_MARK, 10, struct TwtGyroConfig)

// ── aim_touch 风格驱动 ioctl 命令（来自 touch_driver.c）──
#define AIM_MAGIC 'A'
struct AimTouchPoint { int x; int y; int pressure; int touch_major; int tracking_id; };
struct AimTouchInit  { int screen_w; int screen_h; };
#define AIM_TOUCH_DOWN _IOW(AIM_MAGIC, 1, struct AimTouchPoint)
#define AIM_TOUCH_MOVE _IOW(AIM_MAGIC, 2, struct AimTouchPoint)
#define AIM_TOUCH_UP   _IO(AIM_MAGIC, 3)
#define AIM_TOUCH_INIT _IOW(AIM_MAGIC, 4, struct AimTouchInit)

// ── 已知 aim_touch 风格设备节点（按优先级）────────────
// 只保留明确命名，避免与系统标准设备（如 /dev/zero）冲突误判
static const char* kInputHandleNodes[] = {
    "/dev/aim_touch",
    "/dev/rt_touch",
    "/dev/qx_touch",
    "/dev/zero_touch",
    "/dev/ovo_touch",
    "/dev/hakutaku",
};

static std::mt19937& rng() {
    static std::mt19937 r{(uint32_t)(time(nullptr) ^ getpid())};
    return r;
}

// ════════════════════════════════════════════════════════════
//  TwtDriver
// ════════════════════════════════════════════════════════════

TwtDriver::TwtDriver() = default;
TwtDriver::~TwtDriver() {
    if (fd_ >= 0) {
        touchUpAll();
        close(fd_);
    }
}

// 通过 syscall(__NR_reboot) 携带 TwT magic 获取驱动 fd。
// TwT 内核模块 hook 了 reboot 系统调用，识别 magic 后把 fd 写入 arg 指向地址。
int TwtDriver::syscallGetFd() {
#if TWT_HAS_SYSCALL
    int fd = -1;
    long ret;
    register long _x0 __asm__("x0") = 0x114514;
    register long _x1 __asm__("x1") = 0x1919810;
    register long _x2 __asm__("x2") = 0x2778;
    register long _x3 __asm__("x3") = (long)&fd;
    register long _x8 __asm__("x8") = __NR_reboot;
    __asm__ __volatile__(
        "svc #0"
        : "=r"(_x0)
        : "r"(_x0), "r"(_x1), "r"(_x2), "r"(_x3), "r"(_x8)
        : "memory", "cc"
    );
    ret = _x0;
    (void)ret;
    return fd;
#else
    return -1;
#endif
}

// 方式二：从 /proc/self/fd 中查找 anon_inode 含 "TwT_driver" 的 fd
int TwtDriver::findFdFromProc() {
    DIR* dir = opendir("/proc/self/fd");
    if (!dir) return -1;
    int found = -1;
    struct dirent* ent;
    char path[64], link[256];
    while ((ent = readdir(dir)) != nullptr) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        snprintf(path, sizeof(path), "/proc/self/fd/%s", ent->d_name);
        ssize_t len = readlink(path, link, sizeof(link) - 1);
        if (len < 0) continue;
        link[len] = '\0';
        if (strstr(link, "TwT_driver") && strstr(link, "anon_inode:")) {
            found = atoi(ent->d_name);
            break;
        }
    }
    closedir(dir);
    return found;
}

bool TwtDriver::open() {
    fd_ = syscallGetFd();
    if (fd_ < 0) fd_ = findFdFromProc();
    return fd_ >= 0;
}

bool TwtDriver::touchInit(int mode) {
    if (fd_ < 0) return false;
    if (mode < 0 || mode > 1) return false;
    TwtTouchEvent te{};
    te.slot = mode;
    if (ioctl(fd_, TWT_TOUCH_INIT, &te) != 0) {
        if (errno == EALREADY) { touch_ready_ = true; return true; }
        return false;
    }
    touch_ready_ = true;
    return true;
}

bool TwtDriver::touchDown(int slot, int x, int y) {
    if (fd_ < 0 || slot < 0) return false;
    TwtTouchEvent te{slot, x, y};
    if (ioctl(fd_, TWT_TOUCH_DOWN, &te) != 0) return false;
    // 记录活跃 slot
    for (int s : active_slots_) if (s == slot) return true;
    active_slots_.push_back(slot);
    return true;
}

bool TwtDriver::touchMove(int slot, int x, int y) {
    // TwT 无独立 MOVE 命令，持续 TOUCH_DOWN 即更新该 slot 位置
    return touchDown(slot, x, y);
}

bool TwtDriver::touchUp(int slot) {
    if (fd_ < 0 || slot < 0) return false;
    TwtTouchEvent te{slot, 0, 0};
    if (ioctl(fd_, TWT_TOUCH_UP, &te) != 0) return false;
    active_slots_.erase(std::remove(active_slots_.begin(), active_slots_.end(), slot),
                        active_slots_.end());
    return true;
}

void TwtDriver::touchUpAll() {
    for (int s : active_slots_) {
        TwtTouchEvent te{s, 0, 0};
        ioctl(fd_, TWT_TOUCH_UP, &te);
    }
    active_slots_.clear();
}

bool TwtDriver::gyroInit(int method) {
    if (fd_ < 0) return false;
    if (method < 0 || method > 1) return false;
    int m = method;
    if (ioctl(fd_, TWT_GYRO_INIT, &m) != 0) {
        if (errno == EALREADY) { gyro_ready_ = true; return true; }
        return false;
    }
    gyro_ready_ = true;
    return true;
}

bool TwtDriver::gyroInject(float rx, float ry, float rz) {
    if (fd_ < 0 || !gyro_ready_) return false;
    (void)rz; // TwT 陀螺仪仅 x/y 两轴
    TwtGyroConfig cfg{};
    cfg.enable = 1;
    float twt_x = ry;  // 水平
    float twt_y = rx;  // 垂直
    memcpy(&cfg.x, &twt_x, sizeof(uint32_t));
    memcpy(&cfg.y, &twt_y, sizeof(uint32_t));
    return ioctl(fd_, TWT_GYRO_CONFIG, &cfg) == 0;
}

bool TwtDriver::gyroReset() {
    if (fd_ < 0) return false;
    TwtGyroConfig cfg{};
    return ioctl(fd_, TWT_GYRO_CONFIG, &cfg) == 0;
}

// ════════════════════════════════════════════════════════════
//  InputHandleDriver（aim_touch 风格，单 slot）
// ════════════════════════════════════════════════════════════

InputHandleDriver::InputHandleDriver() = default;
InputHandleDriver::~InputHandleDriver() {
    if (fd_ >= 0) {
        touchUpAll();
        close(fd_);
    }
}

bool InputHandleDriver::open(int screen_w, int screen_h) {
    screen_w_ = screen_w;
    screen_h_ = screen_h;
    for (const char* path : kInputHandleNodes) {
        int fd = ::open(path, O_WRONLY);
        if (fd >= 0) {
            fd_ = fd;
            // 提取名字用于日志
            const char* base = strrchr(path, '/');
            name_ = base ? base + 1 : path;
            // 发送 INIT（部分驱动需要，失败可忽略）
            AimTouchInit init{screen_w, screen_h};
            ioctl(fd_, AIM_TOUCH_INIT, &init);
            return true;
        }
    }
    return false;
}

bool InputHandleDriver::touchInit(int /*mode*/) { return fd_ >= 0; }

int InputHandleDriver::randomTrackingId() { return (int)(rng()() % 60000) + 1000; }
int InputHandleDriver::randomPressure()   { return (int)(rng()() % 80) + 80; }
int InputHandleDriver::randomTouchMajor() { return (int)(rng()() % 9) + 8; }

bool InputHandleDriver::touchDown(int slot, int x, int y) {
    if (fd_ < 0) return false;
    // 单 slot 抢占：若当前有别的 slot 活跃，先抬起
    if (active_slot_ >= 0 && active_slot_ != slot) {
        ioctl(fd_, AIM_TOUCH_UP, 0);
        active_slot_ = -1;
    }
    if (active_slot_ == slot) {
        // 已按下，直接走 MOVE
        return touchMove(slot, x, y);
    }
    AimTouchPoint pt{x, y, randomPressure(), randomTouchMajor(), randomTrackingId()};
    tracking_id_ = pt.tracking_id;
    if (ioctl(fd_, AIM_TOUCH_DOWN, &pt) != 0) return false;
    active_slot_ = slot;
    return true;
}

bool InputHandleDriver::touchMove(int slot, int x, int y) {
    if (fd_ < 0) return false;
    if (active_slot_ != slot) return touchDown(slot, x, y);
    AimTouchPoint pt{x, y, randomPressure(), randomTouchMajor(), tracking_id_};
    return ioctl(fd_, AIM_TOUCH_MOVE, &pt) == 0;
}

bool InputHandleDriver::touchUp(int slot) {
    if (fd_ < 0) return false;
    if (active_slot_ != slot) return true;  // 不是当前 slot，忽略
    bool ok = ioctl(fd_, AIM_TOUCH_UP, 0) == 0;
    active_slot_ = -1;
    return ok;
}

void InputHandleDriver::touchUpAll() {
    if (fd_ >= 0 && active_slot_ >= 0) {
        ioctl(fd_, AIM_TOUCH_UP, 0);
        active_slot_ = -1;
    }
}

// ════════════════════════════════════════════════════════════
//  DriverManager
// ════════════════════════════════════════════════════════════

ITouchDriver* DriverManager::probe(DriverType preferred, int screen_w, int screen_h) {
    auto tryTwt = []() -> ITouchDriver* {
        auto* d = new TwtDriver();
        if (d->open()) {
            // 尝试触摸方案二（方案一已被部分游戏特征）
            if (!d->touchInit(1)) d->touchInit(0);
            // 陀螺仪方案二
            d->gyroInit(1);
            return d;
        }
        delete d;
        return nullptr;
    };
    auto tryInputHandle = [screen_w, screen_h]() -> ITouchDriver* {
        auto* d = new InputHandleDriver();
        if (d->open(screen_w, screen_h)) return d;
        delete d;
        return nullptr;
    };

    if (preferred == DriverType::TWT) {
        if (auto* d = tryTwt()) return d;
        if (auto* d = tryInputHandle()) return d;
    } else if (preferred == DriverType::INPUT_HANDLE) {
        if (auto* d = tryInputHandle()) return d;
        if (auto* d = tryTwt()) return d;
    } else {
        // 自动：TwT 优先（多 slot + 陀螺仪，反检测最强）
        if (auto* d = tryTwt()) return d;
        if (auto* d = tryInputHandle()) return d;
    }
    return nullptr;
}

const char* DriverManager::typeName(DriverType t) {
    switch (t) {
    case DriverType::TWT: return "TwT";
    case DriverType::INPUT_HANDLE: return "InputHandle(aim_touch风格)";
    default: return "None";
    }
}

} // namespace kma
