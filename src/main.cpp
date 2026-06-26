// main.cpp — Kernel-key-mapping-ai 主程序
//
// 用法:
//   kma_mapper [-c config.conf] [-d auto|twt|input] [-s WxH] [-i device_name]
//
// 工作流:
//   1. 加载配置（命令行可覆盖）
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
#include <atomic>

static std::atomic<bool> g_running{true};

static void onSignal(int) { g_running = false; }

static void usage(const char* prog) {
    fprintf(stderr,
        "Kernel-key-mapping-ai — 内核驱动键鼠映射\n"
        "用法: %s [选项]\n"
        "  -c <file>   配置文件 (默认 configs/default.conf)\n"
        "  -d <type>   驱动: auto|twt|input (默认 auto)\n"
        "  -s <WxH>    屏幕分辨率, 如 1080x2400\n"
        "  -i <name>   只匹配名字含 name 的输入设备\n"
        "  -h          帮助\n",
        prog);
}

int main(int argc, char* argv[]) {
    std::string cfg_path = "configs/default.conf";
    std::string driver_arg = "auto";
    std::string screen_arg;
    std::string input_filter;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-c" && i + 1 < argc) cfg_path = argv[++i];
        else if (a == "-d" && i + 1 < argc) driver_arg = argv[++i];
        else if (a == "-s" && i + 1 < argc) screen_arg = argv[++i];
        else if (a == "-i" && i + 1 < argc) input_filter = argv[++i];
        else if (a == "-h") { usage(argv[0]); return 0; }
    }

    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

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

    fprintf(stderr, "[配置] 屏幕 %dx%d  驱动:%s  鼠标模式:%s  灵敏度:%.2f\n",
            cfg.screen_w, cfg.screen_h,
            driver_arg.c_str(),
            cfg.mouse_mode == MouseMode::GYRO ? "gyro" : "drag",
            cfg.sensitivity);

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

    reader.setCallback([&mapper](const InputEvent& ev) {
        mapper.onInputEvent(ev);
    });

    // 输入线程
    std::thread input_thread([&]() {
        reader.run();
    });

    fprintf(stderr, "[就绪] 映射运行中，Ctrl+C 退出\n");

    // 主循环：周期 tick（陀螺仪衰减等）
    while (g_running) {
        mapper.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    fprintf(stderr, "\n[退出] 清理中...\n");
    reader.stop();
    if (input_thread.joinable()) input_thread.join();
    return 0;
}
