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
#include <sys/stat.h>
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

// 扫描 /dev/ 发现所有可能的 touch 驱动节点（不依赖硬编码列表）。
// 匹配关键词: touch / twt / rt / qx / zero / aim / hakutaku / ovo / input_handle
// 排除系统标准设备（/dev/zero /dev/null 等）和 input/event 节点。
static void scanDevTouchNodes(std::vector<std::string>& out) {
    DIR* dir = opendir("/dev");
    if (!dir) return;
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        // 排除系统标准设备
        if (name == "null" || name == "zero" || name == "full" ||
            name == "random" || name == "urandom" || name == "kmsg" ||
            name == "console" || name == "tty" || name == "ptmx") continue;
        // 排除 input 子系统节点（这些是真实输入设备，不是注入驱动）
        if (name.rfind("input/", 0) == 0) continue;
        if (name.rfind("event", 0) == 0) continue;
        // 关键词匹配（大小写不敏感）
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        bool match = false;
        const char* keywords[] = {
            "touch", "twt", "rt_touch", "rt_dev", "rt_hook", "rtdev",
            "qx", "zero_touch", "aim", "hakutaku", "ovo", "input_handle",
            "kma", "mapper", "inject"
        };
        for (auto kw : keywords) {
            if (lower.find(kw) != std::string::npos) { match = true; break; }
        }
        if (!match) continue;
        std::string full = "/dev/" + name;
        // 确认是字符设备或可打开
        if (access(full.c_str(), F_OK) == 0) {
            out.push_back(full);
        }
    }
    closedir(dir);
}

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
// 返回 fd(>=0) 或 -1（失败时 last_err_ 记录 errno）。
// 尝试多种 magic 组合，兼容不同 twt 版本。
int TwtDriver::syscallGetFd() {
#if TWT_HAS_SYSCALL
    // 已知 magic 组合（不同 twt 版本可能不同）
    struct MagicCombo { long m1, m2, m3; const char* desc; };
    static const MagicCombo combos[] = {
        {0x114514, 0x1919810, 0x2778,  "经典 TwT (0x114514/0x1919810/0x2778)"},
        {0x114514, 0x1919810, 0x114,   "TwT 变体1 (0x114514/0x1919810/0x114)"},
        {0x1919810, 0x114514, 0x2778,  "TwT 变体2 (m1/m2 互换)"},
        {0x2778, 0x114514, 0x1919810,  "TwT 变体3"},
    };

    std::string all_errs;
    for (const auto& mc : combos) {
        int fd = -1;
        errno = 0;
        register long _x0 __asm__("x0") = mc.m1;
        register long _x1 __asm__("x1") = mc.m2;
        register long _x2 __asm__("x2") = mc.m3;
        register long _x3 __asm__("x3") = (long)&fd;
        register long _x8 __asm__("x8") = __NR_reboot;
        // x0 既是入参(magic1) 又是返回值，必须用 "+r"(read-write) 约束
        __asm__ __volatile__(
            "svc #0"
            : "+r"(_x0)
            : "r"(_x1), "r"(_x2), "r"(_x3), "r"(_x8)
            : "memory", "cc"
        );
        long ret = _x0;
        int saved_errno = errno;
        if (fd >= 0) {
            last_err_.clear();
            return fd;
        }
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "  [%s] ret=%ld errno=%d:%s",
                 mc.desc, ret, saved_errno, strerror(saved_errno));
        all_errs += buf;
        all_errs += "\n";
    }
    last_err_ = "所有 TwT magic 组合均未被 hook 接管:\n" + all_errs;
    return -1;
#else
    last_err_ = "非 aarch64 架构，不支持 TwT syscall 对接";
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
    if (found < 0) last_err_ = "/proc/self/fd 中未找到 anon_inode:...TwT_driver 链接";
    return found;
}

bool TwtDriver::open() {
    last_err_.clear();
    fd_ = syscallGetFd();
    if (fd_ < 0) {
        std::string syscall_err = last_err_;
        fd_ = findFdFromProc();
        if (fd_ < 0) {
            // 两种方式都失败，保留第一次的诊断信息（syscall 失败原因更有参考价值）
            if (!syscall_err.empty()) last_err_ = syscall_err;
            return false;
        }
    }
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

    // 1) 先尝试硬编码已知节点（精确匹配）
    for (const char* path : kInputHandleNodes) {
        int fd = ::open(path, O_WRONLY);
        if (fd >= 0) {
            fd_ = fd;
            const char* base = strrchr(path, '/');
            name_ = base ? base + 1 : path;
            AimTouchInit init{screen_w, screen_h};
            ioctl(fd_, AIM_TOUCH_INIT, &init);
            return true;
        }
    }
    // 2) 扫描 /dev/ 发现所有 touch 相关节点，逐个尝试 ioctl 'A' 探测
    std::vector<std::string> discovered;
    scanDevTouchNodes(discovered);
    for (const auto& path : discovered) {
        // 跳过已尝试的硬编码节点
        bool already = false;
        for (const char* k : kInputHandleNodes) {
            if (path == k) { already = true; break; }
        }
        if (already) continue;
        int fd = ::open(path.c_str(), O_WRONLY);
        if (fd < 0) {
            // 试试读写模式
            fd = ::open(path.c_str(), O_RDWR);
            if (fd < 0) continue;
        }
        // 用 AIM_TOUCH_INIT 探测是否是 aim_touch 风格驱动
        AimTouchInit init{screen_w, screen_h};
        int ret = ioctl(fd, AIM_TOUCH_INIT, &init);
        // 即使 INIT 返回错误，也尝试 DOWN/UP 探测（有些驱动不需要 INIT）
        if (ret == 0 || errno != ENOTTY) {
            fd_ = fd;
            const char* base = strrchr(path.c_str(), '/');
            name_ = base ? base + 1 : path;
            return true;
        }
        close(fd);
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

// 全局诊断缓冲（供 JNI 读取）
static std::string g_diag_log;

static void diagAppend(const std::string& s) {
    g_diag_log += s + "\n";
}

const std::string& driverDiagnosticLog() { return g_diag_log; }
void clearDriverDiagnosticLog() { g_diag_log.clear(); }

ITouchDriver* DriverManager::probe(DriverType preferred, int screen_w, int screen_h) {
    g_diag_log.clear();
    diagAppend("=== 驱动探测诊断 ===");
    diagAppend("架构: " + std::string(
#if TWT_HAS_SYSCALL
        "aarch64 (支持 TwT syscall)"
#else
        "非 aarch64 (不支持 TwT syscall)"
#endif
    ));
    {
        // 列出 /dev 下已知驱动节点
        std::string nodes = "已知驱动节点: ";
        bool any = false;
        for (const char* path : kInputHandleNodes) {
            if (access(path, F_OK) == 0) { nodes += std::string(path) + " "; any = true; }
        }
        diagAppend(any ? nodes : nodes + "(无)");
    }
    {
        // 扫描 /dev/ 发现所有 touch 相关节点
        std::vector<std::string> discovered;
        scanDevTouchNodes(discovered);
        diagAppend("扫描 /dev/ 发现的 touch 相关节点:");
        if (discovered.empty()) {
            diagAppend("  (无)");
        } else {
            for (const auto& p : discovered) {
                // 显示节点权限
                std::string perm;
                struct stat st;
                if (stat(p.c_str(), &st) == 0) {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%o", st.st_mode & 0777);
                    perm = std::string(" (权限=") + buf + ")";
                }
                diagAppend("  " + p + perm);
            }
        }
    }
    {
        // 读取 /proc/bus/input/devices 摘要（前 40 行，看有没有 twt/rt 相关输入设备）
        FILE* fp = fopen("/proc/bus/input/devices", "r");
        if (fp) {
            diagAppend("/proc/bus/input/devices 摘要:");
            char line[512];
            int n = 0;
            while (fgets(line, sizeof(line), fp) && n < 40) {
                // 只显示 Name 和 Handlers 行
                if (strstr(line, "Name=") || strstr(line, "Handlers=") ||
                    strstr(line, "twt") || strstr(line, "rt_") || strstr(line, "touch")) {
                    std::string s(line);
                    if (!s.empty() && s.back() == '\n') s.pop_back();
                    diagAppend("  " + s);
                    ++n;
                }
            }
            fclose(fp);
        } else {
            diagAppend("/proc/bus/input/devices: 无法读取");
        }
    }

    auto tryTwt = []() -> ITouchDriver* {
        diagAppend("[尝试 TwT]");
        auto* d = new TwtDriver();
        if (d->open()) {
            diagAppend("  TwT open() 成功");
            // 尝试触摸方案二（方案一已被部分游戏特征）
            if (!d->touchInit(1)) {
                diagAppend("  touchInit(1) 失败, 尝试 (0)");
                d->touchInit(0);
            }
            // 陀螺仪方案二
            d->gyroInit(1);
            return d;
        }
        diagAppend("  TwT open() 失败: " + d->lastError());
        delete d;
        return nullptr;
    };
    auto tryInputHandle = [screen_w, screen_h]() -> ITouchDriver* {
        diagAppend("[尝试 InputHandle 风格]");
        auto* d = new InputHandleDriver();
        if (d->open(screen_w, screen_h)) {
            diagAppend(std::string("  InputHandle open() 成功: ") + d->name());
            return d;
        }
        diagAppend("  InputHandle open() 失败 (无可用 /dev/*_touch 节点或权限不足)");
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
    diagAppend("=== 所有驱动均未对接 ===");
    diagAppend("提示: 请确认");
    diagAppend("  1. 内核模块已 insmod (lsmod 检查)");
    diagAppend("  2. /dev/ 下存在对应节点 (上方扫描结果)");
    diagAppend("  3. 节点权限 666 (root 执行 chmod 666 /dev/*_touch)");
    diagAppend("  4. SELinux 未拦截 (root 执行 setenforce 0 测试)");
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
