// driver_adapter.h — 多内核驱动统一适配层
//
// 抽象出统一的触摸/陀螺仪注入接口，下方对接两类内核驱动：
//   1. TwtDriver        — TwT 驱动（syscall + ioctl 'T'，支持多 slot + 陀螺仪）
//                          对接来源：上传文件夹/对接例子v1.48.zip 中的 kernel.h
//   2. InputHandleDriver — aim_touch 风格驱动（rt / qx / zero / ovo / hakutaku 等）
//                          走 /dev/xxx_touch 设备节点 + ioctl 'A'，单 slot
//                          接口参考 FpsAimAssistant/kernel/touch_driver.c
//
// 选用策略：DriverManager::probe() 按优先级自动探测可用后端，
//          也支持指定后端。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace kma {

// ── 驱动后端类型 ──────────────────────────────────────
enum class DriverType : int {
    NONE = -1,
    TWT = 0,            // TwT 驱动（syscall + ioctl，多 slot + 陀螺仪）
    INPUT_HANDLE = 1,   // aim_touch 风格（rt/qx/zero/ovo/hakutaku，单 slot）
};

// ── 触摸驱动统一接口 ──────────────────────────────────
// 所有坐标均为屏幕绝对坐标，slot 为触摸点编号（0 起）。
class ITouchDriver {
public:
    virtual ~ITouchDriver() = default;

    virtual DriverType type() const = 0;
    virtual const char* name() const = 0;
    virtual bool isReady() const = 0;

    // 初始化触摸子系统。twt 的 touch_mode: 0/1；aim_touch 风格忽略 mode。
    virtual bool touchInit(int mode = 1) = 0;

    // slot 号按下 (x, y)。对单 slot 后端，若已有活跃点则先抬起再按下。
    virtual bool touchDown(int slot, int x, int y) = 0;

    // slot 号移动到 (x, y)。twt 用持续 TOUCH_DOWN 报送位置。
    virtual bool touchMove(int slot, int x, int y) = 0;

    // slot 号抬起。
    virtual bool touchUp(int slot) = 0;

    // 抬起所有活跃 slot。
    virtual void touchUpAll() = 0;

    // ── 陀螺仪（仅 TWT 后端真正实现，其它后端返回 false）──
    // rx/ry/rz: 三轴角速度（毫弧度/秒）
    virtual bool gyroInit(int method = 1) = 0;
    virtual bool gyroInject(float rx, float ry, float rz) = 0;
    virtual bool gyroReset() = 0;
    virtual bool hasGyro() const = 0;
};

// ── TwT 驱动适配 ──────────────────────────────────────
// 获取 fd 两种方式：
//   a) syscall(__NR_reboot) 携带 magic 0x114514/0x1919810/0x2778
//   b) /proc/self/fd 中查找 anon_inode 含 "TwT_driver"
class TwtDriver : public ITouchDriver {
public:
    TwtDriver();
    ~TwtDriver() override;

    DriverType type() const override { return DriverType::TWT; }
    const char* name() const override { return "TwT"; }
    bool isReady() const override { return fd_ >= 0; }

    bool touchInit(int mode = 1) override;
    bool touchDown(int slot, int x, int y) override;
    bool touchMove(int slot, int x, int y) override;
    bool touchUp(int slot) override;
    void touchUpAll() override;

    bool gyroInit(int method = 1) override;
    bool gyroInject(float rx, float ry, float rz) override;
    bool gyroReset() override;
    bool hasGyro() const override { return gyro_ready_; }

    // 尝试获取 fd 并完成对接。成功返回 true。
    bool open();

    // 最近一次错误描述（诊断用）
    std::string lastError() const { return last_err_; }

private:
    int fd_ = -1;
    bool touch_ready_ = false;
    bool gyro_ready_ = false;
    std::vector<int> active_slots_;  // 当前处于按下状态的 slot
    std::string last_err_;

    int syscallGetFd();
    int findFdFromProc();
};

// ── aim_touch 风格驱动适配（rt / qx / zero / ovo / hakutaku 等）──
// 走 /dev/xxx_touch 设备节点，ioctl 'A' 命令集。
// 内核驱动本身单 slot，本层做"抢占式"单点管理。
class InputHandleDriver : public ITouchDriver {
public:
    InputHandleDriver();
    ~InputHandleDriver() override;

    DriverType type() const override { return DriverType::INPUT_HANDLE; }
    const char* name() const override { return name_.c_str(); }
    bool isReady() const override { return fd_ >= 0; }

    bool touchInit(int mode = 1) override;
    bool touchDown(int slot, int x, int y) override;
    bool touchMove(int slot, int x, int y) override;
    bool touchUp(int slot) override;
    void touchUpAll() override;

    bool gyroInit(int method = 1) override { return false; }
    bool gyroInject(float, float, float) override { return false; }
    bool gyroReset() override { return false; }
    bool hasGyro() const override { return false; }

    // 尝试打开已知设备节点。screen_w/h 用于 AIM_TOUCH_INIT。
    bool open(int screen_w, int screen_h);

private:
    int fd_ = -1;
    int screen_w_ = 0;
    int screen_h_ = 0;
    int active_slot_ = -1;       // 当前占用 slot，-1 表示无
    int tracking_id_ = 0;        // 递增的 tracking_id
    std::string name_;           // 如 "rt_touch"

    // 反检测用的随机化
    int randomTrackingId();
    int randomPressure();
    int randomTouchMajor();
};

// ── 驱动管理器：探测 + 工厂 ───────────────────────────
class DriverManager {
public:
    // 按优先级探测：TWT -> INPUT_HANDLE(rt/qx/zero/...)
    // preferred != NONE 时优先尝试指定后端。
    // 返回创建的驱动指针（失败返回 nullptr），调用方负责释放。
    static ITouchDriver* probe(DriverType preferred = DriverType::NONE,
                               int screen_w = 0, int screen_h = 0);

    static const char* typeName(DriverType t);
};

// 驱动探测诊断日志（probe() 调用后可读取，含每步成功/失败原因）
const std::string& driverDiagnosticLog();
void clearDriverDiagnosticLog();

} // namespace kma
