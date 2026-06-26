// main.cpp — Kernel-key-mapping-ai 主程序
//
// 用法:
//   kma_mapper [选项]
//     -c <file>      配置文件 (默认 configs/default.conf)
//     -d auto|twt|input   驱动类型 (默认 auto)
//     -s <WxH>       屏幕分辨率, 如 1080x2400
//     -i <name>      只匹配名字含 name 的输入设备
//     -m <map>       添加一条按键映射 (可多次指定)
//                    格式: <按键名>:<动作>:<x>:<y>
//                    例: -m KEY_W:hold:540:1700 -m MOUSE_LEFT:hold:920:2050
//     -r <map>       删除一条按键映射 (按键名)
//     -S drag|gyro   鼠标移动模式
//     -l             仅列出当前映射后退出(不启动驱动)
//     -h             帮助
//
// 运行中信号:
//   SIGHUP  重新加载配置文件 (cfg_path)
//   SIGINT/SIGTERM  退出
//
// 工作流:
//   1. 加载配置（命令行可覆盖/追加）
//   2. 探测内核驱动（twt / rt / qx / zero / ...）
//   3. 扫描并打开物理键鼠设备
//   4. epoll 循环：读取键鼠事件 → 映射 → 注入触摸/陀螺仪
#include "driver_adapter.h"
#include "input_reader.h"
#include "mapper.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <thread>
#include <chrono>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>

static std::atomic<bool> g_running{true};
static std::atomic<bool> g_reload{false};
static std::string g_cfg_path;
static kma::Mapper* g_mapper = nullptr;
static std::mutex g_mapper_mtx;

static void onExitSignal(int) { g_running = false; }
static void onReloadSignal(int) { g_reload = true; }

static void usage(const char* prog) {
    fprintf(stderr,
        "Kernel-key-mapping-ai — 内核驱动键鼠映射\n"
        "用法: %s [选项]\n"
        "  -c <file>      配置文件 (默认 configs/default.conf)\n"
        "  -d <type>      驱动: auto|twt|input (默认 auto)\n"
        "  -s <WxH>       屏幕分辨率, 如 1080x2400\n"
        "  -i <name>      只匹配名字含 name 的输入设备\n"
        "  -m <map>       添加按键映射 <按键名>:<动作>:<x>:<y> (可多次)\n"
        "                 动作: click|hold|toggle  例: -m KEY_W:hold:540:1700\n"
        "  -r <key>       删除按键映射 (按键名)\n"
        "  -S drag|gyro   鼠标移动模式\n"
        "  -l             仅列出当前映射后退出\n"
        "  -h             帮助\n"
        "运行中: SIGHUP 重载配置, Ctrl+C 退出\n",
        prog);
}

// 解析 "KEY_W:hold:540:1700" → 添加到 mapper
static bool applyMapArg(kma::Mapper& m, const std::string& arg, bool is_remove) {
    if (is_remove) {
        if (!m.removeKeyMap(arg)) {
            fprintf(stderr, "[警告] 删除映射失败(未找到): %s\n", arg.c_str());
            return false;
        }
        return true;
    }
    // 拆分 a:b:x:y
    auto p1 = arg.find(':');
    if (p1 == std::string::npos) { fprintf(stderr, "[错误] 映射格式错误: %s\n", arg.c_str()); return false; }
    auto p2 = arg.find(':', p1 + 1);
    if (p2 == std::string::npos) { fprintf(stderr, "[错误] 映射格式错误: %s\n", arg.c_str()); return false; }
    auto p3 = arg.find(':', p2 + 1);
    if (p3 == std::string::npos) { fprintf(stderr, "[错误] 映射格式错误: %s\n", arg.c_str()); return false; }
    std::string name = arg.substr(0, p1);
    std::string act  = arg.substr(p1 + 1, p2 - p1 - 1);
    int x = atoi(arg.substr(p2 + 1, p3 - p2 - 1).c_str());
    int y = atoi(arg.substr(p3 + 1).c_str());
    if (!m.addKeyMap(name, act, x, y)) {
        fprintf(stderr, "[错误] 添加映射失败: %s (%s)\n", arg.c_str(), m.lastError().c_str());
        return false;
    }
    return true;
}

int main(int argc, char* argv[]) {
    std::string cfg_path = "configs/default.conf";
    std::string driver_arg = "auto";
    std::string screen_arg;
    std::string input_filter;
    std::string mouse_mode_arg;
    std::vector<std::pair<bool, std::string>> map_args; // (is_remove, arg)
    bool list_only = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-c" && i + 1 < argc) cfg_path = argv[++i];
        else if (a == "-d" && i + 1 < argc) driver_arg = argv[++i];
        else if (a == "-s" && i + 1 < argc) screen_arg = argv[++i];
        else if (a == "-i" && i + 1 < argc) input_filter = argv[++i];
        else if (a == "-m" && i + 1 < argc) map_args.emplace_back(false, argv[++i]);
        else if (a == "-r" && i + 1 < argc) map_args.emplace_back(true, argv[++i]);
        else if (a == "-S" && i + 1 < argc) mouse_mode_arg = argv[++i];
        else if (a == "-l") list_only = true;
        else if (a == "-h") { usage(argv[0]); return 0; }
    }

    g_cfg_path = cfg_path;

    using namespace kma;

    Mapper mapper;
    if (!mapper.loadConfig(cfg_path)) {
        fprintf(stderr, "[警告] 加载配置失败(%s): %s，使用默认\n",
                cfg_path.c_str(), mapper.lastError().c_str());
    }
    MapperConfig& cfg = const_cast<MapperConfig&>(mapper.config());

    // 命令行覆盖
    if (driver_arg == "twt") cfg.driver = DriverType::TWT;
    else if (driver_arg == "input") cfg.driver = DriverType::INPUT_HANDLE;
    else cfg.driver = DriverType::NONE;

    if (!screen_arg.empty()) {
        int w = 0, h = 0;
        if (sscanf(screen_arg.c_str(), "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
            cfg.screen_w = w; cfg.screen_h = h;
        }
    }
    if (!mouse_mode_arg.empty()) {
        cfg.mouse_mode = (mouse_mode_arg == "gyro") ? MouseMode::GYRO : MouseMode::DRAG;
    }

    // 应用 -m / -r
    for (const auto& ma : map_args) {
        applyMapArg(mapper, ma.second, ma.first);
    }

    // 仅列出映射
    if (list_only) {
        fprintf(stderr, "%s", mapper.dumpKeyMaps().c_str());
        return 0;
    }

    signal(SIGINT, onExitSignal);
    signal(SIGTERM, onExitSignal);
    signal(SIGHUP, onReloadSignal);

    fprintf(stderr, "[配置] 屏幕 %dx%d  驱动:%s  鼠标模式:%s  灵敏度:%.2f\n",
            cfg.screen_w, cfg.screen_h,
            driver_arg.c_str(),
            cfg.mouse_mode == MouseMode::GYRO ? "gyro" : "drag",
            cfg.sensitivity);
    fprintf(stderr, "%s", mapper.dumpKeyMaps().c_str());

    // 初始化驱动
    if (!mapper.initDriver()) {
        fprintf(stderr, "[错误] %s\n", mapper.lastError().c_str());
        return 1;
    }
    ITouchDriver* drv = mapper.driver();
    fprintf(stderr, "[驱动] 已对接: %s (陀螺仪:%s)\n",
            drv->name(), drv->hasGyro() ? "支持" : "不支持");

    // 初始化输入读取
    InputReader reader;
    int ndev = reader.scanAndOpen(input_filter);
    if (ndev <= 0) {
        fprintf(stderr, "[错误] %s\n", reader.lastError().c_str());
        return 1;
    }
    fprintf(stderr, "[输入] 已打开 %d 个设备:\n", ndev);
    for (const auto& d : reader.devices()) {
        fprintf(stderr, "  %s [%s] key=%d mouse=%d\n",
                d.path.c_str(), d.name.c_str(), d.is_keyboard, d.is_mouse);
    }

    g_mapper = &mapper;
    reader.setCallback([&mapper](const InputEvent& ev) {
        // 热重载期间短暂跳过，避免读到半更新的配置
        std::lock_guard<std::mutex> lk(g_mapper_mtx);
        (void)lk;
        mapper.onInputEvent(ev);
    });

    // 输入线程
    std::thread input_thread([&]() {
        reader.run();
    });

    fprintf(stderr, "[就绪] 映射运行中，SIGHUP 重载配置，Ctrl+C 退出\n");

    // 主循环：周期 tick（陀螺仪衰减等）+ 热重载检查
    while (g_running) {
        if (g_reload.exchange(false)) {
            std::lock_guard<std::mutex> lk(g_mapper_mtx);
            Mapper tmp;
            if (tmp.loadConfig(g_cfg_path)) {
                // 保留命令行覆盖的驱动/屏幕/鼠标模式
                MapperConfig& tc = const_cast<MapperConfig&>(tmp.config());
                tc.driver = cfg.driver;
                tc.screen_w = cfg.screen_w;
                tc.screen_h = cfg.screen_h;
                tc.mouse_mode = cfg.mouse_mode;
                // 用新配置的 keys 替换
                cfg.keys = tc.keys;
                fprintf(stderr, "[重载] 配置已重新加载 (%s)\n", g_cfg_path.c_str());
                fprintf(stderr, "%s", mapper.dumpKeyMaps().c_str());
            } else {
                fprintf(stderr, "[重载] 失败: %s\n", tmp.lastError().c_str());
            }
        }
        mapper.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    fprintf(stderr, "\n[退出] 清理中...\n");
    reader.stop();
    if (input_thread.joinable()) input_thread.join();
    return 0;
}
