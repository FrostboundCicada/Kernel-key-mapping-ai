// kma_jni.cpp — JNI 桥接层
//
// 把 C++ 映射核心(driver_adapter/input_reader/mapper)暴露给 Kotlin UI 层。
// 包名: com.kma.mapping  (Kotlin 侧调用 System.loadLibrary("kma"))
//
// 线程模型:
//   - Kotlin 侧通过 startNative() 启动后台线程跑 InputReader + tick 循环
//   - 悬浮窗的虚拟按键操作通过 addKeyMap/removeKeyMap 等同步调用
//   - 物理键监听回调 onKeyEvent(code, down) 回到 Kotlin(用于"待选择"绑定)
#include <jni.h>
#include <android/log.h>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>

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
    std::thread g_thread;
    std::atomic<bool> g_running{false};
    std::mutex g_mtx;
    JavaVM* g_vm = nullptr;
    jobject g_callback_ref = nullptr;   // 全局引用: InputCallback (Kotlin)
    jmethodID g_on_key_method = nullptr;

    // 物理按键回调 → 转发到 Kotlin
    void nativeInputCallback(const InputEvent& ev) {
        if (ev.kind != InputEv::PRESS_DOWN && ev.kind != InputEv::PRESS_UP) {
            // 鼠标移动等直接交给 mapper
            std::lock_guard<std::mutex> lk(g_mtx);
            g_mapper.onInputEvent(ev);
            return;
        }
        // 按键事件：先回调 Kotlin(用于绑定捕获)，再交 mapper 处理
        if (g_vm && g_callback_ref) {
            JNIEnv* env = nullptr;
            bool attached = false;
            if (g_vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
                g_vm->AttachCurrentThread(&env, nullptr);
                attached = true;
            }
            if (env && g_on_key_method) {
                env->CallVoidMethod(g_callback_ref, g_on_key_method,
                                    (jint)ev.code, (jboolean)(ev.kind == InputEv::PRESS_DOWN ? 1 : 0));
            }
            if (attached) g_vm->DetachCurrentThread();
        }
        std::lock_guard<std::mutex> lk(g_mtx);
        g_mapper.onInputEvent(ev);
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

// 初始化驱动 + 输入读取。screenW/H, driverType(0=auto,1=twt,2=input)
// 返回: 驱动名(成功) / 空串(失败)
JNIEXPORT jstring JNICALL
Java_com_kma_mapping_NativeBridge_nativeInit(JNIEnv* env, jobject /*thiz*/,
                                              jint screenW, jint screenH, jint driverType,
                                              jstring inputFilter) {
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

    const char* filter = inputFilter ? env->GetStringUTFChars(inputFilter, nullptr) : nullptr;
    int ndev = g_reader.scanAndOpen(filter ? filter : "");
    if (filter) env->ReleaseStringUTFChars(inputFilter, filter);

    if (ndev <= 0) {
        LOGE("输入设备打开失败: %s", g_reader.lastError().c_str());
        return env->NewStringUTF("");
    }
    g_reader.setCallback(nativeInputCallback);
    LOGI("驱动: %s 陀螺仪:%d  设备数:%d", drv->name(), drv->hasGyro(), ndev);
    return env->NewStringUTF(drv->name());
}

// 启动后台读取线程
JNIEXPORT jboolean JNICALL
Java_com_kma_mapping_NativeBridge_startNative(JNIEnv* /*env*/, jobject /*thiz*/) {
    if (g_running) return JNI_TRUE;
    g_running = true;
    g_thread = std::thread([]() {
        // tick 循环与输入读取在同一线程：先非阻塞读，再 tick
        std::thread input_th([](){ g_reader.run(); });
        while (g_running) {
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                g_mapper.tick();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
        g_reader.stop();
        if (input_th.joinable()) input_th.join();
    });
    return JNI_TRUE;
}

// 停止
JNIEXPORT void JNICALL
Java_com_kma_mapping_NativeBridge_stopNative(JNIEnv* /*env*/, jobject /*thiz*/) {
    g_running = false;
    if (g_thread.joinable()) g_thread.join();
}

// 添加按键映射: keyCode, action(枚举值), x, y, extra(radius/repeatMs)
JNIEXPORT jboolean JNICALL
Java_com_kma_mapping_NativeBridge_addKeyMap(JNIEnv* env, jobject /*thiz*/,
                                             jint keyCode, jint action, jint x, jint y,
                                             jint extra) {
    std::lock_guard<std::mutex> lk(g_mtx);
    KeyAction ka = (KeyAction)action;
    bool ok = g_mapper.addKeyMap(keyCode, ka, x, y);
    if (ok) {
        // 设置 radius/repeat_ms
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

// 获取所有映射的文本(供 UI 列出)
JNIEXPORT jstring JNICALL
Java_com_kma_mapping_NativeBridge_dumpKeyMaps(JNIEnv* env, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lk(g_mtx);
    return env->NewStringUTF(g_mapper.dumpKeyMaps().c_str());
}

// 设置鼠标模式: 0=drag 1=gyro
JNIEXPORT void JNICALL
Java_com_kma_mapping_NativeBridge_setMouseMode(JNIEnv* /*env*/, jobject /*thiz*/, jint mode) {
    std::lock_guard<std::mutex> lk(g_mtx);
    auto& cfg = const_cast<MapperConfig&>(g_mapper.config());
    cfg.mouse_mode = (mode == 1) ? MouseMode::GYRO : MouseMode::DRAG;
}

// 设置灵敏度
JNIEXPORT void JNICALL
Java_com_kma_mapping_NativeBridge_setSensitivity(JNIEnv* /*env*/, jobject /*thiz*/,
                                                  jfloat sens, jfloat gyroSens) {
    std::lock_guard<std::mutex> lk(g_mtx);
    auto& cfg = const_cast<MapperConfig&>(g_mapper.config());
    cfg.sensitivity = sens;
    cfg.gyro_sensitivity = gyroSens;
}

// 获取驱动是否支持陀螺仪
JNIEXPORT jboolean JNICALL
Java_com_kma_mapping_NativeBridge_hasGyro(JNIEnv* /*env*/, jobject /*thiz*/) {
    if (auto* d = g_mapper.driver()) return d->hasGyro() ? JNI_TRUE : JNI_FALSE;
    return JNI_FALSE;
}

} // extern "C"
