package com.kma.mapping

import android.view.KeyEvent

/**
 * 一个虚拟按键的配置：屏幕位置 + 绑定的物理键 + 类型 + 额外参数。
 * 由悬浮窗创建、拖拽、绑定，最终通过 NativeBridge 同步到 native 层。
 */
data class KeyButton(
    var id: Int,                    // 唯一 id
    var x: Int,                     // 屏幕中心 X
    var y: Int,                     // 屏幕中心 Y
    var keyCode: Int = -1,          // 绑定的物理键码 (-1=未绑定)
    var action: Int = NativeBridge.ActionType.HOLD,  // 按键类型
    var extra: Int = 0              // 额外参数: repeat 间隔 / joystick 半径
) {
    /** 是否已绑定物理键 */
    val isBound get() = keyCode >= 0

    /** 显示用标签 */
    fun label(): String {
        val act = NativeBridge.ActionType.NAMES[action] ?: "?"
        return if (isBound) "${keyName(keyCode)}\n$act" else "未绑定\n$act"
    }

    companion object {
        /** Linux input 按键码 → 可读名（常用键） */
        fun keyName(code: Int): String {
            // 字母 A-Z = 30-57
            if (code in 30..57) {
                val letters = "ZXCVBNMASDFGHJKLQWERTYUIOP"
                val idx = code - 30
                return if (idx < letters.length) letters[idx].toString() else "KEY_$code"
            }
            // 鼠标键
            return when (code) {
                272 -> "鼠标左键"; 273 -> "鼠标右键"; 274 -> "鼠标中键"
                275 -> "鼠标侧1"; 276 -> "鼠标侧2"
                57 -> "Space"; 28 -> "Enter"; 1 -> "Esc"; 15 -> "Tab"
                42 -> "LShift"; 54 -> "RShift"; 29 -> "LCtrl"; 97 -> "RCtrl"
                else -> "KEY_$code"
            }
        }
    }
}
