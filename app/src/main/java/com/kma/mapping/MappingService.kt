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
 * 生命周期与输入监听:
 *   - onCreate: 立即 setKeyCallback + nativeStartInput
 *     → 服务一启动，物理按键就能被捕获用于"待选择"绑定（无需先开始映射）
 *   - "开始映射": nativeInitDriver(显示驱动状态) + nativeStartMapping
 *   - "停止映射": nativeStopMapping（输入监听保留，绑定仍可用）
 *   - onDestroy: nativeStopMapping + nativeStopInput + 移除所有悬浮 view
 *
 * 悬浮窗管理: 所有 view 加入 allViews，onDestroy 统一清理，避免残留。
 */
class MappingService : Service() {

    private lateinit var wm: WindowManager
    private val allViews = mutableListOf<View>()        // 跟踪所有悬浮 view，统一清理
    private var mainPanel: View? = null
    private var floatBall: View? = null
    private val buttonViews = mutableListOf<KeyButtonView>()
    private var nextId = 1
    private var screenWidth = 1080
    private var screenHeight = 2400
    private var mappingStarted = false
    private var inputStarted = false
    private var driverName: String = ""
    private lateinit var statusText: TextView           // 面板上的状态显示

    // native 回调（物理按键，用于"待选择"绑定捕获）
    private val keyCallback = object : NativeBridge.KeyCallback {
        override fun onKeyEvent(code: Int, down: Boolean) {
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
        @Suppress("DEPRECATION") wm.defaultDisplay.getRealMetrics(dm)
        screenWidth = dm.widthPixels
        screenHeight = dm.heightPixels

        // 服务启动即开始输入监听 → 绑定随时可用
        NativeBridge.setKeyCallback(keyCallback)
        val ndev = NativeBridge.nativeStartInput("")
        inputStarted = ndev > 0
        android.util.Log.i("Kma", "输入监听启动, 设备数=$ndev")
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        startForegroundCompat()
        showMainPanel()
        return START_STICKY
    }

    override fun onDestroy() {
        super.onDestroy()
        // 停止映射 + 输入监听
        if (mappingStarted) NativeBridge.nativeStopMapping()
        if (inputStarted) NativeBridge.nativeStopInput()
        // 统一移除所有悬浮 view（含面板、悬浮球、虚拟按键）
        synchronized(allViews) {
            allViews.toList().forEach { runCatching { wm.removeView(it) } }
            allViews.clear()
        }
        buttonViews.clear()
        mainPanel = null
        floatBall = null
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
        statusText = TextView(this).apply {
            text = statusLine()
            textSize = 12f
            setTextColor(0xFFCCCCCC.toInt())
            setPadding(0, 8, 0, 8)
        }
        val btnCreate = Button(this).apply { text = "创建新按键" }
        val btnDetect = Button(this).apply { text = "检测外设" }
        val btnStart = Button(this).apply { text = "开始映射" }
        val btnHide = Button(this).apply { text = "隐藏面板" }

        btnCreate.setOnClickListener { createNewButton() }
        btnDetect.setOnClickListener { showDeviceList() }
        btnStart.setOnClickListener {
            if (mappingStarted) { stopMapping(); btnStart.text = "开始映射" }
            else { startMapping(); btnStart.text = "停止映射" }
        }
        btnHide.setOnClickListener { hideMainPanel() }

        panel.addView(title)
        panel.addView(statusText)
        panel.addView(btnCreate)
        panel.addView(btnDetect)
        panel.addView(btnStart)
        panel.addView(btnHide)
        panel.setBackgroundColor(0xDD222222.toInt())

        addView(panel, params)
        mainPanel = panel
    }

    private fun hideMainPanel() {
        mainPanel?.let { removeView(it); mainPanel = null }
        showFloatButton()
    }

    /** 隐藏后的小悬浮球，点击恢复面板 */
    private fun showFloatButton() {
        if (floatBall != null) return
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
                        if (!moved) { removeView(ball); floatBall = null; showMainPanel() }
                    }
                }
                return true
            }
        })
        addView(ball, lp)
        floatBall = ball
    }

    // ── 虚拟按键管理 ────────────────────────────────────
    private fun createNewButton() {
        if (!inputStarted) {
            toast("输入监听未启动（需 root 读取 /dev/input）")
            return
        }
        val btn = KeyButton(id = nextId++, x = screenWidth / 2, y = screenHeight / 2)
        val view = KeyButtonView(this, btn)
        val lp = buttonParams(btn.x, btn.y)

        view.onClicked = { v ->
            buttonViews.forEach { it.waitingForBind = false }
            v.waitingForBind = true
            toast("按下要绑定的物理键...")
        }
        view.onDragged = { v, nx, ny ->
            v.button.x = nx; v.button.y = ny
            val clampedX = nx.coerceIn(v.width / 2, screenWidth - v.width / 2)
            val clampedY = ny.coerceIn(v.height / 2, screenHeight - v.height / 2)
            (v.layoutParams as WindowManager.LayoutParams).apply {
                x = clampedX - v.width / 2; y = clampedY - v.height / 2
            }
            wm.updateViewLayout(v, v.layoutParams)
        }
        view.setOnLongClickListener { showTypePicker(view.button); true }

        addView(view, lp)
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
        buttonViews.firstOrNull { it.button.id == button.id }?.let { it.text = button.label() }
        syncButton(button)
    }

    private fun removeButton(button: KeyButton) {
        val idx = buttonViews.indexOfFirst { it.button.id == button.id }
        if (idx >= 0) {
            val v = buttonViews.removeAt(idx)
            removeView(v)
            if (button.isBound && mappingStarted) NativeBridge.removeKeyMap(button.keyCode)
        }
    }

    private fun syncButton(button: KeyButton) {
        if (!button.isBound) return
        // 始终同步到 native（即使映射未启动，配置已就绪，启动后生效）
        NativeBridge.addKeyMap(button.keyCode, button.action, button.x, button.y, button.extra)
    }

    // ── 硬件检测 ────────────────────────────────────────
    private fun showDeviceList() {
        val text = NativeBridge.nativeGetDevices()
        val devs = NativeBridge.parseDevices(text)
        val sb = StringBuilder()
        if (devs.isEmpty()) {
            sb.append("未检测到键鼠外设\n（需 root 读取 /dev/input/event*）")
        } else {
            sb.append("检测到 ${devs.size} 个外设:\n\n")
            devs.forEach {
                sb.append("• [${it.type}] ${it.name}\n  ${it.path}\n\n")
            }
        }
        // 同时刷新面板状态
        statusText.text = statusLine(devs.size)
        AlertDialog.Builder(this)
            .setTitle("外设检测")
            .setMessage(sb.toString())
            .setPositiveButton("确定", null)
            .show()
    }

    // ── 映射开始/停止 ───────────────────────────────────
    private fun startMapping() {
        // 1. 初始化驱动并反馈状态
        driverName = NativeBridge.nativeInitDriver(screenWidth, screenHeight, NativeBridge.DriverType.AUTO)
        if (driverName.isEmpty()) {
            statusText.text = "驱动: ✗ 未对接\n(需 root + 内核驱动 twt/rt/qx/zero)"
            toast("驱动初始化失败：未检测到内核驱动")
            return
        }
        // 2. 同步所有已绑定按键
        buttonViews.forEach { syncButton(it.button) }
        // 3. 启动映射
        if (!NativeBridge.nativeStartMapping()) {
            toast("映射启动失败")
            return
        }
        mappingStarted = true
        val gyro = if (NativeBridge.hasGyro()) "支持陀螺仪" else "无陀螺仪"
        statusText.text = "驱动: ✓ $driverName ($gyro)\n映射运行中"
        toast("映射已启动: $driverName")
    }

    private fun stopMapping() {
        if (!mappingStarted) return
        NativeBridge.nativeStopMapping()
        mappingStarted = false
        statusText.text = statusLine()
        toast("映射已停止（绑定仍可用）")
    }

    // ── 工具 ────────────────────────────────────────────
    private fun statusLine(devCount: Int = -1): String {
        val dc = if (devCount >= 0) devCount else NativeBridge.parseDevices(NativeBridge.nativeGetDevices()).size
        val input = if (inputStarted) "输入:✓($dc 设备)" else "输入:✗"
        val drv = if (mappingStarted) "驱动:✓ $driverName" else if (driverName.isNotEmpty()) "驱动:$driverName(未启动)" else "驱动:未对接"
        return "$input\n$drv"
    }

    private fun addView(v: View, lp: WindowManager.LayoutParams) {
        wm.addView(v, lp)
        synchronized(allViews) { allViews.add(v) }
    }

    private fun removeView(v: View) {
        runCatching { wm.removeView(v) }
        synchronized(allViews) { allViews.remove(v) }
    }

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
