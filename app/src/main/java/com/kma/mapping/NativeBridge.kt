package com.kma.mapping

/**
 * JNI 桥接：调用 native 层的驱动探测/映射/输入读取。
 * 物理按键事件通过 [onKeyEvent] 回调到 Kotlin，供悬浮窗"待选择"绑定使用。
 */
object NativeBridge {

    init {
        System.loadLibrary("kma")
    }

    /** 物理按键回调接口（code = Linux input 按键码, down = 按下/抬起） */
    interface KeyCallback {
        fun onKeyEvent(code: Int, down: Boolean)
    }

    /** 按键类型枚举，数值与 C++ KeyAction 一致 */
    object ActionType {
        const val CLICK = 0
        const val HOLD = 1
        const val TOGGLE = 2
        const val REPEAT = 3
        const val MOVE = 4
        const val JOYSTICK = 5
        const val WHEEL = 6
        const val SENSITIVITY = 7

        val NAMES = mapOf(
            CLICK to "单击", HOLD to "长按", TOGGLE to "锁定切换",
            REPEAT to "连点", MOVE to "移动", JOYSTICK to "摇杆",
            WHEEL to "轮盘", SENSITIVITY to "灵敏度调节"
        )
    }

    /** 驱动类型 */
    object DriverType { const val AUTO = 0; const val TWT = 1; const val INPUT_HANDLE = 2 }
    /** 鼠标模式 */
    object MouseMode { const val DRAG = 0; const val GYRO = 1 }

    // --- JNI 方法 ---
    @JvmStatic external fun setKeyCallback(cb: KeyCallback)

    /** 启动输入监听（绑定用，服务启动即调用）。返回打开的设备数 */
    @JvmStatic external fun nativeStartInput(inputFilter: String): Int
    /** 停止输入监听 */
    @JvmStatic external fun nativeStopInput()
    /** 输入监听状态详情（诊断用）：运行中/设备数/最近错误/已打开设备列表 */
    @JvmStatic external fun nativeInputStatus(): String

    /** 硬件检测：返回设备列表文本，每行 path|name|isKb|isMouse */
    @JvmStatic external fun nativeGetDevices(): String

    /** 驱动初始化（开始映射时）。返回驱动名/空串 */
    @JvmStatic external fun nativeInitDriver(screenW: Int, screenH: Int, driverType: Int): String
    /** 驱动探测诊断日志（nativeInitDriver 失败后读取，含每步原因 + errno） */
    @JvmStatic external fun nativeDiagnoseDriver(): String
    @JvmStatic external fun hasGyro(): Boolean

    /** 映射启停 */
    @JvmStatic external fun nativeStartMapping(): Boolean
    @JvmStatic external fun nativeStopMapping()

    @JvmStatic external fun addKeyMap(keyCode: Int, action: Int, x: Int, y: Int, extra: Int): Boolean
    @JvmStatic external fun removeKeyMap(keyCode: Int): Boolean
    @JvmStatic external fun clearKeyMaps()
    @JvmStatic external fun dumpKeyMaps(): String
    @JvmStatic external fun setMouseMode(mode: Int)
    @JvmStatic external fun setSensitivity(sens: Float, gyroSens: Float)

    /** 已连接的输入设备信息 */
    data class DeviceInfo(
        val path: String, val name: String,
        val isKeyboard: Boolean, val isMouse: Boolean,
        val keyCount: Int = 0, val hasRel: Boolean = false
    ) {
        val type: String get() = when {
            isKeyboard && isMouse -> "键鼠一体"
            isKeyboard -> "键盘"
            isMouse -> "鼠标"
            keyCount > 0 -> "按键设备($keyCount 键)"
            else -> "其它"
        }
    }

    /** 解析 nativeGetDevices() 返回的文本为设备列表 */
    fun parseDevices(text: String): List<DeviceInfo> {
        val list = mutableListOf<DeviceInfo>()
        text.trim().split("\n").forEach { line ->
            if (line.isBlank()) return@forEach
            val p = line.split("|")
            if (p.size >= 4) {
                list.add(DeviceInfo(
                    path = p[0],
                    name = p[1],
                    isKeyboard = p[2] == "1",
                    isMouse = p[3] == "1",
                    keyCount = p.getOrNull(4)?.toIntOrNull() ?: 0,
                    hasRel = p.getOrNull(5) == "1"
                ))
            }
        }
        return list
    }
}
