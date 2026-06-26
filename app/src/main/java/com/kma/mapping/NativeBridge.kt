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
    @JvmStatic external fun nativeInit(screenW: Int, screenH: Int, driverType: Int, inputFilter: String): String
    @JvmStatic external fun startNative(): Boolean
    @JvmStatic external fun stopNative()
    @JvmStatic external fun addKeyMap(keyCode: Int, action: Int, x: Int, y: Int, extra: Int): Boolean
    @JvmStatic external fun removeKeyMap(keyCode: Int): Boolean
    @JvmStatic external fun clearKeyMaps()
    @JvmStatic external fun dumpKeyMaps(): String
    @JvmStatic external fun setMouseMode(mode: Int)
    @JvmStatic external fun setSensitivity(sens: Float, gyroSens: Float)
    @JvmStatic external fun hasGyro(): Boolean
}
