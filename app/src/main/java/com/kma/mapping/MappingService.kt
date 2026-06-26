package com.kma.mapping

import android.app.AlertDialog
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.graphics.PixelFormat
import android.os.Build
import android.os.IBinder
import android.provider.Settings
import android.util.DisplayMetrics
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.WindowManager
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import kotlin.math.abs

/**
 * 悬浮窗映射服务。
 *
 * 职责:
 *   1. 显示主控悬浮窗（含"创建新按键"按钮 + 开始/停止映射 + 隐藏）
 *   2. 管理虚拟按键 [KeyButtonView] 的创建/拖拽/删除
 *   3. 绑定流程：单击虚拟按键 → "待选择" → 捕获物理键 → 绑定
 *   4. 长按虚拟按键 → 弹出类型选择菜单（移动/按键/单击/连点/长按/锁定切换/摇杆/轮盘/灵敏度）
 *   5. 把映射通过 [NativeBridge] 同步到 native 层并启动输入读取
 */
class MappingService : Service() {

    private lateinit var wm: WindowManager
    private var mainPanel: View? = null
    private val buttonViews = mutableListOf<KeyButtonView>()
    private var nextId = 1
    private var screenWidth = 1080
    private var screenHeight = 2400
    private var mappingStarted = false

    // native 回调（物理按键）
    private val keyCallback = object : NativeBridge.KeyCallback {
        override fun onKeyEvent(code: Int, down: Boolean) {
            // 仅在"待绑定"且按下时捕获
            if (down) {
                val target = buttonViews.firstOrNull { it.waitingForBind }
                if (target != null) {
                    target.button.keyCode = code
                    target.waitingForBind = false
                    syncButton(target.button)
                    toast("已绑定: ${KeyButton.keyName(code)}")
                }
            }
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        wm = getSystemService(WINDOW_SERVICE) as WindowManager
        val dm = DisplayMetrics()
        wm.defaultDisplay.getRealMetrics(dm)
        screenWidth = dm.widthPixels
        screenHeight = dm.heightPixels
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        startForegroundCompat()
        showMainPanel()
        return START_STICKY
    }

    override fun onDestroy() {
        super.onDestroy()
        stopMapping()
        buttonViews.forEach { wm.removeView(it) }
        buttonViews.clear()
        mainPanel?.let { wm.removeView(it) }
    }

    // ── 主控悬浮窗 ──────────────────────────────────────
    private fun showMainPanel() {
        if (mainPanel != null) return
        val panel = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(24, 24, 24, 24)
        }
        val params = panelParams()

        val title = TextView(this).apply {
            text = "键鼠映射"
            textSize = 16f
            setTextColor(0xFFFFFFFF.toInt())
        }
        val btnCreate = Button(this).apply { text = "创建新按键" }
        val btnStart = Button(this).apply { text = "开始映射" }
        val btnHide = Button(this).apply { text = "隐藏面板" }

        btnCreate.setOnClickListener { createNewButton() }
        btnStart.setOnClickListener {
            if (mappingStarted) { stopMapping(); btnStart.text = "开始映射" }
            else { startMapping(); btnStart.text = "停止映射" }
        }
        btnHide.setOnClickListener {
            wm.removeView(panel); mainPanel = null
            showFloatButton()
        }

        panel.addView(title)
        panel.addView(btnCreate)
        panel.addView(btnStart)
        panel.addView(btnHide)
        panel.setBackgroundColor(0xCC222222.toInt())

        wm.addView(panel, params)
        mainPanel = panel
    }

    /** 隐藏后的小悬浮球，点击恢复面板 */
    private fun showFloatButton() {
        val ball = TextView(this).apply {
            text = "K"
            gravity = Gravity.CENTER
            setTextColor(0xFFFFFFFF.toInt())
            setBackgroundColor(0xCC2266BB.toInt())
        }
        val lp = WindowManager.LayoutParams(
            WindowManager.LayoutParams.WRAP_CONTENT,
            WindowManager.LayoutParams.WRAP_CONTENT,
            windowType(),
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE,
            PixelFormat.TRANSLUCENT
        ).apply { gravity = Gravity.TOP or Gravity.START; x = 0; y = 200 }

        var moved = false
        ball.setOnTouchListener(object : View.OnTouchListener {
            var dx = 0f; var dy = 0f; var sx = 0; var sy = 0
            override fun onTouch(v: View, e: MotionEvent): Boolean {
                when (e.action) {
                    MotionEvent.ACTION_DOWN -> {
                        dx = e.rawX; dy = e.rawY; sx = lp.x; sy = lp.y; moved = false
                    }
                    MotionEvent.ACTION_MOVE -> {
                        val mx = (e.rawX - dx).toInt(); val my = (e.rawY - dy).toInt()
                        if (abs(mx) > 10 || abs(my) > 10) { moved = true; lp.x = sx + mx; lp.y = sy + my; wm.updateViewLayout(v, lp) }
                    }
                    MotionEvent.ACTION_UP -> {
                        if (!moved) { wm.removeView(v); showMainPanel() }
                    }
                }
                return true
            }
        })
        wm.addView(ball, lp)
    }

    // ── 虚拟按键管理 ────────────────────────────────────
    private fun createNewButton() {
        val btn = KeyButton(id = nextId++, x = screenWidth / 2, y = screenHeight / 2)
        val view = KeyButtonView(this, btn)
        val lp = buttonParams(btn.x, btn.y)

        view.onClicked = { v ->
            // 单击：进入待绑定
            buttonViews.forEach { it.waitingForBind = false }
            v.waitingForBind = true
            toast("按下要绑定的物理键...")
        }
        view.onLongClicked = { v -> showTypePicker(v.button) }
        view.onDragged = { v, nx, ny ->
            v.button.x = nx; v.button.y = ny
            val clampedX = nx.coerceIn(v.width / 2, screenWidth - v.width / 2)
            val clampedY = ny.coerceIn(v.height / 2, screenHeight - v.height / 2)
            (v.layoutParams as WindowManager.LayoutParams).apply {
                x = clampedX - v.width / 2; y = clampedY - v.height / 2
            }
            wm.updateViewLayout(v, v.layoutParams)
        }
        view.setOnLongClickListener {
            showTypePicker(view.button); true
        }

        wm.addView(view, lp)
        buttonViews.add(view)
        view.waitingForBind = true
        toast("新按键已创建，按下物理键绑定")
    }

    /** 长按弹出类型选择菜单 */
    private fun showTypePicker(button: KeyButton) {
        val names = NativeBridge.ActionType.NAMES.values.toTypedArray()
        AlertDialog.Builder(this)
            .setTitle("选择按键类型: ${KeyButton.keyName(button.keyCode)}")
            .setItems(names) { _, which ->
                val actionCode = NativeBridge.ActionType.NAMES.keys.elementAt(which)
                button.action = actionCode
                // 类型特定参数
                when (actionCode) {
                    NativeBridge.ActionType.REPEAT -> {
                        askNumber("连点间隔(ms)", button.extra.ifZero(50)) { v -> button.extra = v; refreshAndSync(button) }
                        return@setItems
                    }
                    NativeBridge.ActionType.JOYSTICK -> {
                        askNumber("摇杆半径", button.extra.ifZero(120)) { v -> button.extra = v; refreshAndSync(button) }
                        return@setItems
                    }
                }
                refreshAndSync(button)
            }
            .setNeutralButton("删除") { _, _ -> removeButton(button) }
            .show()
    }

    private fun refreshAndSync(button: KeyButton) {
        buttonViews.firstOrNull { it.button.id == button.id }?.let {
            it.text = button.label()
        }
        syncButton(button)
    }

    private fun removeButton(button: KeyButton) {
        val idx = buttonViews.indexOfFirst { it.button.id == button.id }
        if (idx >= 0) {
            val v = buttonViews.removeAt(idx)
            wm.removeView(v)
            if (button.isBound) NativeBridge.removeKeyMap(button.keyCode)
        }
    }

    private fun syncButton(button: KeyButton) {
        if (!button.isBound) return
        NativeBridge.addKeyMap(button.keyCode, button.action, button.x, button.y, button.extra)
    }

    // ── 映射开始/停止 ───────────────────────────────────
    private fun startMapping() {
        // 同步所有已绑定按键到 native
        NativeBridge.setKeyCallback(keyCallback)
        val driverName = NativeBridge.nativeInit(screenWidth, screenHeight, NativeBridge.DriverType.AUTO, "")
        if (driverName.isEmpty()) {
            toast("驱动初始化失败（需 root + 内核驱动）")
            return
        }
        buttonViews.forEach { syncButton(it.button) }
        NativeBridge.startNative()
        mappingStarted = true
        toast("映射已启动: $driverName")
    }

    private fun stopMapping() {
        if (!mappingStarted) return
        NativeBridge.stopNative()
        mappingStarted = false
    }

    // ── 工具 ────────────────────────────────────────────
    private fun panelParams() = WindowManager.LayoutParams(
        WindowManager.LayoutParams.WRAP_CONTENT,
        WindowManager.LayoutParams.WRAP_CONTENT,
        windowType(),
        WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE,
        PixelFormat.TRANSLUCENT
    ).apply { gravity = Gravity.TOP or Gravity.START; x = 40; y = 120 }

    private fun buttonParams(x: Int, y: Int) = WindowManager.LayoutParams(
        160, 160,
        windowType(),
        WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE,
        PixelFormat.TRANSLUCENT
    ).apply { gravity = Gravity.TOP or Gravity.START; this.x = x - 80; this.y = y - 80 }

    private fun windowType() =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O)
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
        else
            @Suppress("DEPRECATION") WindowManager.LayoutParams.TYPE_PHONE

    private fun askNumber(title: String, default: Int, cb: (Int) -> Unit) {
        val input = android.widget.EditText(this).apply {
            inputType = android.text.InputType.TYPE_CLASS_NUMBER
            setText(default.toString())
        }
        AlertDialog.Builder(this).setTitle(title).setView(input)
            .setPositiveButton("确定") { _, _ -> cb(input.text.toString().toIntOrNull() ?: default) }
            .setNegativeButton("取消", null).show()
    }

    private fun toast(msg: String) {
        android.os.Handler(android.os.Looper.getMainLooper()).post {
            Toast.makeText(this, msg, Toast.LENGTH_SHORT).show()
        }
    }

    private fun startForegroundCompat() {
        val channelId = "kma_mapping"
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val ch = NotificationChannel(channelId, "映射服务", NotificationManager.IMPORTANCE_LOW)
            (getSystemService(NOTIFICATION_SERVICE) as NotificationManager).createNotificationChannel(ch)
        }
        val notif = Notification.Builder(this, channelId)
            .setContentTitle("键鼠映射运行中")
            .setContentText("悬浮窗已开启")
            .setSmallIcon(android.R.drawable.ic_menu_compass)
            .build()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            startForeground(1, notif, android.content.pm.ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE)
        } else {
            startForeground(1, notif)
        }
    }

    private fun Int.ifZero(v: Int) = if (this == 0) v else this
}
