// kma_config.cpp — 交互式按键映射配置工具
//
// 用途: 在终端交互式添加/删除/查看键鼠按键到屏幕坐标的映射，并保存为
//       kma_mapper 可读取的配置文件。无需 root，纯文本操作。
//
// 用法:
//   kma_config [-c config.conf]
//     进入后输入命令交互:
//       a / add       添加映射: 按键名 动作 x y
//       d / del       删除映射: 按键名
//       l / list      列出当前映射
//       c / clear     清空所有映射
//       s / save      保存到配置文件
//       q / quit      退出(未保存提示)
//       h / help      帮助
//
// 示例:
//   > add KEY_W hold 540 1700
//   > add MOUSE_LEFT hold 920 2050
//   > list
//   > save
#include "mapper.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <string>
#include <sstream>
#include <iostream>
#include <vector>

using namespace kma;

static void printHelp() {
    printf(
        "命令:\n"
        "  a|add <按键名> <动作> <x> <y>   添加/覆盖映射 (动作: click|hold|toggle)\n"
        "  d|del <按键名>                  删除映射\n"
        "  l|list                          列出当前映射\n"
        "  c|clear                         清空所有映射\n"
        "  s|save [文件]                   保存配置(默认原文件)\n"
        "  q|quit                          退出\n"
        "  h|help                          帮助\n"
        "按键名例: KEY_W KEY_A KEY_SPACE MOUSE_LEFT MOUSE_RIGHT KEY_LEFTSHIFT\n");
}

// 把一行输入拆成 token
static std::vector<std::string> splitTokens(const std::string& line) {
    std::vector<std::string> toks;
    std::istringstream is(line);
    std::string t;
    while (is >> t) toks.push_back(t);
    return toks;
}

int main(int argc, char* argv[]) {
    std::string cfg_path = "configs/default.conf";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-c" && i + 1 < argc) cfg_path = argv[++i];
        else if (a == "-h") {
            printf("用法: %s [-c config.conf]\n", argv[0]);
            return 0;
        }
    }

    Mapper mapper;
    if (!mapper.loadConfig(cfg_path)) {
        fprintf(stderr, "[提示] 未读到现有配置(%s)，将新建: %s\n",
                cfg_path.c_str(), mapper.lastError().c_str());
    }
    // 配置工具不初始化驱动，仅编辑配置
    MapperConfig& cfg = const_cast<MapperConfig&>(mapper.config());

    printf("Kernel-key-mapping-ai 配置工具  配置文件: %s\n", cfg_path.c_str());
    printf("屏幕: %dx%d  驱动: %s  鼠标模式: %s\n",
           cfg.screen_w, cfg.screen_h,
           DriverManager::typeName(cfg.driver),
           cfg.mouse_mode == MouseMode::GYRO ? "gyro" : "drag");
    printHelp();
    printf("\n当前映射:\n%s", mapper.dumpKeyMaps().c_str());

    std::string line;
    bool dirty = false;
    while (true) {
        printf("> ");
        std::cout.flush();
        if (!std::getline(std::cin, line)) break;
        auto toks = splitTokens(line);
        if (toks.empty()) continue;
        std::string cmd = toks[0];
        // 转小写
        for (auto& ch : cmd) ch = (char)tolower(ch);

        if (cmd == "q" || cmd == "quit" || cmd == "exit") {
            if (dirty) {
                printf("有未保存改动，确认退出? (y/n): ");
                std::string ans; std::getline(std::cin, ans);
                if (ans != "y" && ans != "Y") continue;
            }
            break;
        } else if (cmd == "h" || cmd == "help") {
            printHelp();
        } else if (cmd == "l" || cmd == "list") {
            printf("%s", mapper.dumpKeyMaps().c_str());
        } else if (cmd == "c" || cmd == "clear") {
            mapper.clearKeyMaps();
            dirty = true;
            printf("已清空所有映射\n");
        } else if (cmd == "a" || cmd == "add") {
            if (toks.size() < 5) {
                printf("用法: add <按键名> <动作> <x> <y>\n"); continue;
            }
            const std::string& name = toks[1];
            const std::string& act = toks[2];
            int x = atoi(toks[3].c_str());
            int y = atoi(toks[4].c_str());
            if (mapper.addKeyMap(name, act, x, y)) {
                printf("已添加: %s %s (%d,%d)\n", name.c_str(), act.c_str(), x, y);
                dirty = true;
            } else {
                printf("失败: %s\n", mapper.lastError().c_str());
            }
        } else if (cmd == "d" || cmd == "del") {
            if (toks.size() < 2) { printf("用法: del <按键名>\n"); continue; }
            if (mapper.removeKeyMap(toks[1])) {
                printf("已删除: %s\n", toks[1].c_str());
                dirty = true;
            } else {
                printf("未找到: %s\n", toks[1].c_str());
            }
        } else if (cmd == "s" || cmd == "save") {
            std::string path = cfg_path;
            if (toks.size() >= 2) path = toks[1];
            if (mapper.saveConfig(path)) {
                printf("已保存到: %s\n", path.c_str());
                cfg_path = path;
                dirty = false;
            } else {
                printf("保存失败: %s\n", path.c_str());
            }
        } else if (cmd == "screen" && toks.size() >= 3) {
            cfg.screen_w = atoi(toks[1].c_str());
            cfg.screen_h = atoi(toks[2].c_str());
            printf("屏幕设为 %dx%d\n", cfg.screen_w, cfg.screen_h);
            dirty = true;
        } else if (cmd == "mouse_mode" && toks.size() >= 2) {
            cfg.mouse_mode = (toks[1] == "gyro") ? MouseMode::GYRO : MouseMode::DRAG;
            printf("鼠标模式设为 %s\n", toks[1].c_str());
            dirty = true;
        } else {
            printf("未知命令，输入 h 查看帮助\n");
        }
    }
    return 0;
}
