package com.kma.mapping

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.util.AttributeSet
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.widget.TextView
import kotlin.math.abs

/**
 * 悬浮窗中的单个虚拟按键。
 *
 * 交互手势:
 *   - 单击          → 进入"待选择"绑定状态（等待物理键）
 *   - 长按          → 弹出类型选择菜单
 *   - 拖拽          → 移动按键位置（按下后移动距离 > 阈值则判定为拖拽）
 */
class KeyButtonView @JvmOverloads constructor(
    context: Context,
    val button: KeyButton,
    attrs: AttributeSet? = null
) : TextView(context, attrs) {

    /** 待绑定状态（点击后等待物理键按下） */
    var waitingForBind = false
        set(v) { field = v; refreshState() }

    /** 回调 */
    var onClicked: ((KeyButtonView) -> Unit)? = null
    var onLongClicked: ((KeyButtonView) -> Unit)? = null
    var onDragged: ((KeyButtonView, Int, Int) -> Unit)? = null

    private val size = 160
    private var downX = 0f
    private var downY = 0f
    private var startLeft = 0
    private var startTop = 0
    private var isDragging = false
    private val touchSlop = 12

    private val normalBg = Color.argb(120, 40, 120, 220)
    private val waitingBg = Color.argb(160, 220, 80, 40)

    init {
        gravity = Gravity.CENTER
        textSize = 11f
        setTextColor(Color.WHITE)
        setPadding(8, 8, 8, 8)
        refreshState()
    }

    private fun refreshState() {
        text = button.label()
        if (waitingForBind) {
            setBackgroundColor(waitingBg)
            text = "待选择\n按下物理键"
        } else {
            setBackgroundColor(normalBg)
        }
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        when (event.action) {
            MotionEvent.ACTION_DOWN -> {
                downX = event.rawX
                downY = event.rawY
                startLeft = left
                startTop = top
                isDragging = false
            }
            MotionEvent.ACTION_MOVE -> {
                val dx = (event.rawX - downX).toInt()
                val dy = (event.rawY - downY).toInt()
                if (abs(dx) > touchSlop || abs(dy) > touchSlop) {
                    isDragging = true
                    val nx = startLeft + dx
                    val ny = startTop + dy
                    onDragged?.invoke(this, nx + width / 2, ny + height / 2)
                }
            }
            MotionEvent.ACTION_UP -> {
                if (!isDragging) {
                    // 长按已处理则不再触发单击
                    onClicked?.invoke(this)
                }
                performClick()
            }
        }
        return true
    }

    override fun performClick(): Boolean {
        super.performClick()
        return true
    }
}
