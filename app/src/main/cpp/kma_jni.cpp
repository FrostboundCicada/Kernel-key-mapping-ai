// kma_jni.cpp — JNI 桥接层
//
// 把 C++ 映射核心(driver_adapter/input_reader/mapper)暴露给 Kotlin UI 层。
// 包名: com.kma.mapping  (Kotlin 侧调用 System.loadLibrary("kma"))
//
// 线程模型（拆分，解决"绑定需先启动映射"的问题）:
//   - 输入监听线程: nativeStartInput() 启动，独立运行，KeyCallback 始终回调
//     → 服务一启动就能捕获物理键用于"待选择"绑定
//   - 映射 tick 线程: nativeStartMapping() 启动，仅在映射启用时跑
//   - g_mapping_enabled 控制是否把事件交给 mapper（绑定捕获不受影响）
//
// 接口拆分:
//   nativeStartInput / nativeStopInput      输入监听（绑定用）
//   nativeGetDevices                        硬件检测（独立扫描，不占用）
//   nativeInitDriver                        驱动初始化（"开始映射"时）
//   nativeStartMapping / nativeStopMapping  映射启停
#include <jni.h>
#include <android/log.h>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/input.h>

#include "driver_adapter.h"
#include "input_reader.h"
#include "mapper.h"

#define LOG_TAG "KmaJni"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

using namespace kma;

namespace {
    Mapper g_mapper;
    InputReader g_reader;
    std::thread g_input_thread;     // 输入监听线程（绑定捕获）
    std::thread g_tick_thread;      // 映射 tick 线程
    std::atomic<bool> g_input_running{false};
    std::atomic<bool> g_mapping_enabled{false};
    std::atomic<bool> g_tick_running{false};
    std::mutex g_mtx;               // 保护 mapper 配置访问
    JavaVM* g_vm = nullptr;
    jobject g_callback_ref = nullptr;
    jmethodID g_on_key_method = nullptr;

    // 物理按键回调 → 始终转发到 Kotlin(绑定捕获)；仅在映射启用时交 mapper
    void nativeInputCallback(const InputEvent& ev) {
        // 按键事件：始终回调 Kotlin（用于"待选择"绑定）
        if (ev.kind == InputEv::PRESS_DOWN || ev.kind == InputEv::PRESS_UP) {
            if (g_vm && g_callback_ref) {
                JNIEnv* env = nullptr;
                bool attached = false;
                if (g_vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
                    g_vm->AttachCurrentThread(&env, nullptr);
                    attached = true;
                }
                if (env && g_on_key_method) {
                    env->CallVoidMethod(g_callback_ref, g_on_key_method,
                                        (jint)ev.code,
                                        (jboolean)(ev.kind == InputEv::PRESS_DOWN ? 1 : 0));
                }
                if (attached) g_vm->DetachCurrentThread();
            }
        }
        // 仅映射启用时交给 mapper
        if (g_mapping_enabled.load()) {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_mapper.onInputEvent(ev);
        }
    }
}

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    g_vm = vm;
    return JNI_VERSION_1_6;
}

// 设置物理按键回调 (Kotlin InputCallback.onKeyEvent(code, down))
JNIEXPORT void JNICALL
Java_com_kma_mapping_NativeBridge_setKeyCallback(JNIEnv* env, jobject /*thiz*/, jobject callback) {
    if (g_callback_ref) env->DeleteGlobalRef(g_callback_ref);
    g_callback_ref = env->NewGlobalRef(callback);
    jclass cls = env->GetObjectClass(callback);
    g_on_key_method = env->GetMethodID(cls, "onKeyEvent", "(IZ)V");
    env->DeleteLocalRef(cls);
}

// ── 输入监听（绑定用，独立于映射）──────────────────────
// 启动输入监听线程。返回打开的设备数（0=失败）。服务启动即调用。
JNIEXPORT jint JNICALL
Java_com_kma_mapping_NativeBridge_nativeStartInput(JNIEnv* env, jobject /*thiz*/,
                                                    jstring inputFilter) {
    if (g_input_running.load()) return (jint)g_reader.devices().size();
    const char* filter = inputFilter ? env->GetStringUTFChars(inputFilter, nullptr) : nullptr;
    int ndev = g_reader.scanAndOpen(filter ? filter : "");
    if (filter) env->ReleaseStringUTFChars(inputFilter, filter);
    if (ndev <= 0) {
        LOGE("输入设备打开失败: %s", g_reader.lastError().c_str());
        return 0;
    }
    g_reader.setCallback(nativeInputCallback);
    g_input_running = true;
    g_input_thread = std::thread([](){ g_reader.run(); });
    LOGI("输入监听已启动, 设备数:%d", ndev);
    return (jint)ndev;
}

// 停止输入监听（服务销毁时）
JNIEXPORT void JNICALL
Java_com_kma_mapping_NativeBridge_nativeStopInput(JNIEnv* /*env*/, jobject /*thiz*/) {
    g_input_running = false;
    g_reader.stop();
    if (g_input_thread.joinable()) g_input_thread.join();
}

// ── 硬件检测（独立扫描，不占用设备）──────────────────────
// 返回设备列表文本，每行: path|name|is_keyboard|is_mouse
// 例: /dev/input/event3|Logitech USB Keyboard|1|0
JNIEXPORT jstring JNICALL
Java_com_kma_mapping_NativeBridge_nativeGetDevices(JNIEnv* env, jobject /*thiz*/) {
    std::string result;
    DIR* dir = opendir("/dev/input");
    if (!dir) return env->NewStringUTF("");
    struct dirent* ent;
    char path[64], name[256];
    while ((ent = readdir(dir)) != nullptr) {
        if (strncmp(ent->d_name, "event", 5) != 0) continue;
        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;

        memset(name, 0, sizeof(name));
        ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);

        // 能力探测
        unsigned char evbit[EV_MAX / 8 + 1] = {0};
        unsigned char relbit[REL_MAX / 8 + 1] = {0};
        unsigned char keybit[KEY_MAX / 8 + 1] = {0};
        ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), evbit);
        ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relbit)), relbit);
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit);
        close(fd);

        auto test = [](const unsigned char* b, int c) { return (b[c/8] >> (c%8)) & 1; };
        bool has_ev_key = test(evbit, EV_KEY);
        bool has_ev_rel = test(evbit, EV_REL);
        bool is_kb = has_ev_key && (test(keybit, KEY_Q) || test(keybit, KEY_SPACE));
        bool is_mouse = has_ev_rel && (test(relbit, REL_X) || test(keybit, BTN_LEFT));
        // 只上报键鼠类设备
        if (!is_kb && !is_mouse) continue;

        result += std::string(path) + "|" + std::string(name) + "|"
                + (is_kb ? "1" : "0") + "|" + (is_mouse ? "1" : "0") + "\n";
    }
    closedir(dir);
    return env->NewStringUTF(result.c_str());
}

// ── 驱动初始化（"开始映射"时调用）────────────────────────
// 返回驱动名(成功) / 空串(失败)。失败时诊断日志可通过 nativeDiagnoseDriver 读取。
JNIEXPORT jstring JNICALL
Java_com_kma_mapping_NativeBridge_nativeInitDriver(JNIEnv* env, jobject /*thiz*/,
                                                    jint screenW, jint screenH, jint driverType) {
    std::lock_guard<std::mutex> lk(g_mtx);
    // 释放旧驱动
    if (g_mapper.driver()) {
        // mapper 内部 owns_driver_ 会处理释放
    }
    MapperConfig cfg;
    cfg.screen_w = screenW;
    cfg.screen_h = screenH;
    cfg.driver = (driverType == 1) ? DriverType::TWT
               : (driverType == 2) ? DriverType::INPUT_HANDLE
               : DriverType::NONE;
    g_mapper.setConfig(cfg);
    if (!g_mapper.initDriver()) {
        LOGE("驱动初始化失败: %s", g_mapper.lastError().c_str());
        return env->NewStringUTF("");
    }
    ITouchDriver* drv = g_mapper.driver();
    LOGI("驱动已对接: %s 陀螺仪:%d", drv->name(), drv->hasGyro());
    return env->NewStringUTF(drv->name());
}

// 返回驱动探测诊断日志（含每步成功/失败原因 + errno）
JNIEXPORT jstring JNICALL
Java_com_kma_mapping_NativeBridge_nativeDiagnoseDriver(JNIEnv* env, jobject /*thiz*/) {
    return env->NewStringUTF(driverDiagnosticLog().c_str());
}

// 获取驱动是否支持陀螺仪（需先 initDriver）
JNIEXPORT jboolean JNICALL
Java_com_kma_mapping_NativeBridge_hasGyro(JNIEnv* /*env*/, jobject /*thiz*/) {
    if (auto* d = g_mapper.driver()) return d->hasGyro() ? JNI_TRUE : JNI_FALSE;
    return JNI_FALSE;
}

// ── 映射启停 ────────────────────────────────────────────
JNIEXPORT jboolean JNICALL
Java_com_kma_mapping_NativeBridge_nativeStartMapping(JNIEnv* /*env*/, jobject /*thiz*/) {
    if (g_tick_running.load()) return JNI_TRUE;
    if (!g_mapper.driver()) return JNI_FALSE;
    g_mapping_enabled = true;
    g_tick_running = true;
    g_tick_thread = std::thread([]() {
        while (g_tick_running.load()) {
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                g_mapper.tick();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
    });
    LOGI("映射已启动");
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_kma_mapping_NativeBridge_nativeStopMapping(JNIEnv* /*env*/, jobject /*thiz*/) {
    g_mapping_enabled = false;
    g_tick_running = false;
    if (g_tick_thread.joinable()) g_tick_thread.join();
    // 抬起所有触点 + 重置陀螺仪
    std::lock_guard<std::mutex> lk(g_mtx);
    if (auto* d = g_mapper.driver()) {
        d->touchUpAll();
        if (d->hasGyro()) d->gyroReset();
    }
    LOGI("映射已停止");
}

// ── 映射配置 ────────────────────────────────────────────
JNIEXPORT jboolean JNICALL
Java_com_kma_mapping_NativeBridge_addKeyMap(JNIEnv* env, jobject /*thiz*/,
                                             jint keyCode, jint action, jint x, jint y,
                                             jint extra) {
    std::lock_guard<std::mutex> lk(g_mtx);
    KeyAction ka = (KeyAction)action;
    bool ok = g_mapper.addKeyMap(keyCode, ka, x, y);
    if (ok) {
        auto& keys = const_cast<std::vector<KeyMap>&>(g_mapper.config().keys);
        for (auto& k : keys) {
            if (k.code == keyCode) {
                if (ka == KeyAction::REPEAT && extra > 0) k.repeat_ms = extra;
                if (ka == KeyAction::JOYSTICK && extra > 0) k.radius = extra;
                break;
            }
        }
    }
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_kma_mapping_NativeBridge_removeKeyMap(JNIEnv* /*env*/, jobject /*thiz*/, jint keyCode) {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_mapper.removeKeyMap(keyCode) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_kma_mapping_NativeBridge_clearKeyMaps(JNIEnv* /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_mapper.clearKeyMaps();
}

JNIEXPORT jstring JNICALL
Java_com_kma_mapping_NativeBridge_dumpKeyMaps(JNIEnv* env, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lk(g_mtx);
    return env->NewStringUTF(g_mapper.dumpKeyMaps().c_str());
}

JNIEXPORT void JNICALL
Java_com_kma_mapping_NativeBridge_setMouseMode(JNIEnv* /*env*/, jobject /*thiz*/, jint mode) {
    std::lock_guard<std::mutex> lk(g_mtx);
    auto& cfg = const_cast<MapperConfig&>(g_mapper.config());
    cfg.mouse_mode = (mode == 1) ? MouseMode::GYRO : MouseMode::DRAG;
}

JNIEXPORT void JNICALL
Java_com_kma_mapping_NativeBridge_setSensitivity(JNIEnv* /*env*/, jobject /*thiz*/,
                                                  jfloat sens, jfloat gyroSens) {
    std::lock_guard<std::mutex> lk(g_mtx);
    auto& cfg = const_cast<MapperConfig&>(g_mapper.config());
    cfg.sensitivity = sens;
    cfg.gyro_sensitivity = gyroSens;
}

} // extern "C"
