package com.kma.mapping

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.graphics.PixelFormat
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.text.InputType
import android.util.DisplayMetrics
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.WindowManager
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import kotlin.math.abs

/**
 * 悬浮窗映射服务。
 *
 * 生命周期与输入监听:
 *   - onCreate: 后台线程执行 RootShell.grantDevPermissions() → nativeStartInput
 *     → 服务一启动，物理按键就能被捕获用于"待选择"绑定（无需先开始映射）
 *   - "开始映射": nativeInitDriver(显示驱动状态) + nativeStartMapping
 *     失败时弹出诊断面板（含 native 诊断日志 + lsmod + /dev 节点权限）
 *   - "停止映射": nativeStopMapping（输入监听保留，绑定仍可用）
 *   - onDestroy: nativeStopMapping + nativeStopInput + 移除所有悬浮 view
 *
 * 对话框实现: Service 上下文无法直接弹 AlertDialog（BadTokenException），
 *   所有"对话框"改用 WindowManager 悬浮 view 实现，避免闪退。
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
    private var rootGranted = false
    private var driverName: String = ""
    private lateinit var statusText: TextView           // 面板上的状态显示
    private val mainHandler = Handler(Looper.getMainLooper())

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

        // 设置回调（即使输入监听还没启动也先注册，启动后立即生效）
        NativeBridge.setKeyCallback(keyCallback)

        // 后台线程: root 提权 → chmod /dev/input → 启动输入监听
        Thread {
            rootGranted = RootShell.hasRoot()
            if (rootGranted) {
                RootShell.grantDevPermissions()
            }
            val ndev = NativeBridge.nativeStartInput("")
            inputStarted = ndev > 0
            android.util.Log.i("Kma", "Root=$rootGranted 输入监听启动, 设备数=$ndev")
            mainHandler.post { refreshStatus() }
        }.start()
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
        // 统一移除所有悬浮 view（含面板、悬浮球、虚拟按键、对话框）
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
            text = "初始化中..."
            textSize = 12f
            setTextColor(0xFFCCCCCC.toInt())
            setPadding(0, 8, 0, 8)
        }
        val btnCreate = Button(this).apply { text = "创建新按键" }
        val btnDetect = Button(this).apply { text = "检测外设" }
        val btnDiag = Button(this).apply { text = "驱动诊断" }
        val btnStart = Button(this).apply { text = "开始映射" }
        val btnHide = Button(this).apply { text = "隐藏面板" }

        btnCreate.setOnClickListener { createNewButton() }
        btnDetect.setOnClickListener { showDeviceList() }
        btnDiag.setOnClickListener { showDriverDiagnostic() }
        btnStart.setOnClickListener {
            if (mappingStarted) { stopMapping(); btnStart.text = "开始映射" }
            else { startMapping(); btnStart.text = "停止映射" }
        }
        btnHide.setOnClickListener { hideMainPanel() }

        panel.addView(title)
        panel.addView(statusText)
        panel.addView(btnCreate)
        panel.addView(btnDetect)
        panel.addView(btnDiag)
        panel.addView(btnStart)
        panel.addView(btnHide)
        panel.setBackgroundColor(0xDD222222.toInt())

        addView(panel, params)
        mainPanel = panel
        refreshStatus()
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
            // 给出更具体的原因
            val reason = when {
                !rootGranted -> "未获得 root 权限（需 root 才能读取 /dev/input/event*）"
                else -> "输入监听未启动（可能 /dev/input/event* 不存在或无键鼠设备）"
            }
            showOverlayMessage("无法创建按键", reason + "\n\n请点「检测外设」或「驱动诊断」查看详情")
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

    /** 长按弹出类型选择菜单（overlay 实现，避免 AlertDialog BadTokenException） */
    private fun showTypePicker(button: KeyButton) {
        val items = NativeBridge.ActionType.NAMES.entries.toList()
        // 末尾加"删除按键"选项
        val labels = items.map { it.value }.toTypedArray() + arrayOf("删除按键")
        showOverlayMenu(
            "选择类型: ${KeyButton.keyName(button.keyCode)}",
            labels,
            onClose = { /* nothing */ }
        ) { which ->
            if (which == items.size) {
                // 删除
                removeButton(button)
                return@showOverlayMenu
            }
            val actionCode = items[which].key
            button.action = actionCode
            when (actionCode) {
                NativeBridge.ActionType.REPEAT -> {
                    showOverlayInput("连点间隔(ms)", button.extra.ifZero(50).toString()) { v ->
                        button.extra = v; refreshAndSync(button)
                    }
                }
                NativeBridge.ActionType.JOYSTICK -> {
                    showOverlayInput("摇杆半径", button.extra.ifZero(120).toString()) { v ->
                        button.extra = v; refreshAndSync(button)
                    }
                }
                else -> refreshAndSync(button)
            }
        }
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
        Thread {
            // 先确保有权限（用户可能刚授权 root）
            if (rootGranted) RootShell.grantDevPermissions()
            val nativeText = NativeBridge.nativeGetDevices()
            val devs = NativeBridge.parseDevices(nativeText)
            val sb = StringBuilder()
            sb.append("Root: ${if (rootGranted) "✓ 已授权" else "✗ 未授权"}\n")
            sb.append("输入监听: ${if (inputStarted) "✓ 运行中" else "✗ 未启动"}\n\n")

            // 1) 键鼠外设（/dev/input/event*）
            sb.append("=== 键鼠外设 (/dev/input/event*) ===\n")
            if (devs.isEmpty()) {
                sb.append("(未检测到键盘/鼠标)\n")
                if (rootGranted) {
                    sb.append("原始节点列表:\n")
                    sb.append(RootShell.listInputNodes())
                }
            } else {
                devs.forEach {
                    sb.append("• [${it.type}] ${it.name}\n  ${it.path}\n")
                }
            }
            sb.append("\n")

            // 2) 触摸驱动节点（/dev/twt /dev/*_touch 等）— 这是用户刷入的内核驱动节点
            sb.append("=== 触摸驱动节点 (/dev/*_touch /dev/twt 等) ===\n")
            val touchNodes = RootShell.exec(
                "ls -la /dev/ 2>/dev/null | grep -iE 'touch|twt|rt_|rtdev|qx|zero|aim|hakutaku|ovo|input_handle|inject' | grep -v 'input/' || echo '(无触摸驱动节点)'"
            )
            sb.append(touchNodes)
            sb.append("\n")

            // 3) 内核模块
            sb.append("=== 已加载的相关内核模块 ===\n")
            val mods = RootShell.listModules()
            val touchMods = mods.lines().filter {
                it.contains("twt", true) || it.contains("touch", true) ||
                it.contains("aim", true) || it.contains("rt_", true) ||
                it.contains("rt_dev", true) || it.contains("rt_hook", true) ||
                it.contains("qx", true) || it.contains("zero", true) ||
                it.contains("hakutaku", true) || it.contains("ovo", true)
            }
            sb.append(touchMods.joinToString("\n").ifEmpty { "(未找到相关模块)" })

            mainHandler.post {
                refreshStatus(devs.size)
                showOverlayMessageWithCopy("外设检测", sb.toString())
            }
        }.start()
    }

    // ── 驱动诊断 ────────────────────────────────────────
    private fun showDriverDiagnostic() {
        Thread {
            val sb = StringBuilder()
            sb.append("=== 环境检查 ===\n")
            sb.append("Root: ${if (rootGranted) "✓" else "✗"}\n")
            sb.append("屏幕: ${screenWidth}x${screenHeight}\n")
            sb.append("架构: ${Build.SUPPORTED_ABIS.joinToString(",")}\n")
            sb.append("Android: ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})\n")
            sb.append("设备: ${Build.MANUFACTURER} ${Build.MODEL}\n\n")

            sb.append("=== SELinux 状态 ===\n")
            sb.append(RootShell.exec("getenforce 2>/dev/null || echo '无法读取'"))
            sb.append("\n\n")

            sb.append("=== 内核模块 (lsmod 完整) ===\n")
            val mods = RootShell.listModules()
            // 显示完整 lsmod，但标注 touch 相关行
            mods.lines().forEach { line ->
                if (line.isBlank()) return@forEach
                val isTouchRelated = line.contains("twt", true) ||
                    line.contains("touch", true) ||
                    line.contains("aim", true) ||
                    line.contains("rt_", true) ||
                    line.contains("rt_dev", true) ||
                    line.contains("rt_hook", true) ||
                    line.contains("qx_", true) ||
                    line.contains("zero", true) ||
                    line.contains("hakutaku", true) ||
                    line.contains("ovo", true)
                sb.append(if (isTouchRelated) "▶ $line\n" else "$line\n")
            }
            sb.append("\n")

            sb.append("=== /dev 下所有 touch/twt/rt 相关节点 ===\n")
            // 用 root 列出 /dev 下所有可能节点（不只已知列表）
            sb.append(RootShell.exec(
                "ls -la /dev/ 2>/dev/null | grep -iE 'touch|twt|rt_|rtdev|qx|zero|aim|hakutaku|ovo|input_handle|inject|mapper' || echo '(无)'"
            ))
            sb.append("\n")

            sb.append("=== /proc/bus/input/devices ===\n")
            sb.append(RootShell.exec("cat /proc/bus/input/devices 2>/dev/null | head -80"))
            sb.append("\n\n")

            sb.append("=== native 驱动探测日志 ===\n")
            sb.append("(执行 nativeInitDriver 探测...)\n")
            // 实际执行一次探测并捕获诊断
            val drv = NativeBridge.nativeInitDriver(screenWidth, screenHeight, NativeBridge.DriverType.AUTO)
            sb.append("探测结果: ${if (drv.isEmpty()) "未对接 ✗" else "✓ $drv"}\n")
            sb.append(NativeBridge.nativeDiagnoseDriver())
            sb.append("\n")

            // 关键修复：探测成功后同步更新 driverName，避免状态栏仍显示"未对接"
            // 而 startMapping 又能成功的状态不一致问题
            if (drv.isNotEmpty()) {
                driverName = drv
            }

            if (drv.isEmpty()) {
                sb.append("=== dmesg 末尾 30 行 (root) ===\n")
                sb.append(RootShell.dmesgTail(30))
            }

            mainHandler.post {
                refreshStatus()
                showOverlayMessageWithCopy("驱动诊断", sb.toString())
            }
        }.start()
    }

    // ── 映射开始/停止 ───────────────────────────────────
    private fun startMapping() {
        // 1. 初始化驱动并反馈状态
        driverName = NativeBridge.nativeInitDriver(screenWidth, screenHeight, NativeBridge.DriverType.AUTO)
        if (driverName.isEmpty()) {
            val diag = NativeBridge.nativeDiagnoseDriver()
            statusText.text = "驱动: ✗ 未对接\n(点「驱动诊断」查看详情)"
            toast("驱动初始化失败")
            // 自动弹出诊断（带复制按钮，方便用户反馈）
            showOverlayMessageWithCopy(
                "驱动未对接",
                "驱动探测失败，诊断日志:\n\n$diag\n\n" +
                "可能原因:\n" +
                "1. twt 模块未加载 → root 执行 insmod twt.ko\n" +
                "2. /dev/*_touch 节点权限不足 → 已自动 chmod，若仍失败请检查 SELinux\n" +
                "3. TwT syscall 未被 hook → 确认 twt 模块版本与 magic 一致\n" +
                "4. 非 aarch64 架构 → TwT 仅支持 arm64\n\n" +
                "请点「复制全部」把诊断信息发给我"
            )
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
        refreshStatus()
        toast("映射已停止（绑定仍可用）")
    }

    // ── overlay 对话框（替代 AlertDialog，避免 BadTokenException）────
    /** 显示消息型 overlay 对话框 */
    private fun showOverlayMessage(title: String, message: String) {
        showOverlayMessageWithCopy(title, message, showCopy = false)
    }

    /** 显示消息型 overlay 对话框（带复制按钮） */
    private fun showOverlayMessageWithCopy(title: String, message: String, showCopy: Boolean = true) {
        val container = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(32, 24, 32, 24)
            setBackgroundColor(0xEE333333.toInt())
        }
        val titleView = TextView(this).apply {
            text = title; textSize = 16f; setTextColor(0xFFFFFFFF.toInt()); setPadding(0, 0, 0, 16)
        }
        val msgView = TextView(this).apply {
            text = message; textSize = 12f; setTextColor(0xFFDDDDDD.toInt())
            setLineSpacing(2f, 1f)
            setTextIsSelectable(true)
        }
        val scroll = ScrollView(this).apply { addView(msgView) }
        val btnRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            setPadding(0, 16, 0, 0)
        }
        val btnClose = Button(this).apply { text = "关闭" }
        btnRow.addView(btnClose, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        if (showCopy) {
            val btnCopy = Button(this).apply {
                text = "复制"
                setOnClickListener {
                    val cm = getSystemService(CLIPBOARD_SERVICE) as android.content.ClipboardManager
                    cm.setPrimaryClip(android.content.ClipData.newPlainText("kma_diag", message))
                    toast("已复制到剪贴板")
                }
            }
            btnRow.addView(btnCopy, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        }
        container.addView(titleView)
        container.addView(scroll, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f))
        container.addView(btnRow)

        val lp = WindowManager.LayoutParams(
            WindowManager.LayoutParams.MATCH_PARENT,
            WindowManager.LayoutParams.MATCH_PARENT,
            windowType(),
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE,
            PixelFormat.TRANSLUCENT
        )
        btnClose.setOnClickListener { removeView(container) }
        addView(container, lp)
    }

    /** 显示消息型 overlay 对话框（带复制按钮，方便用户分享诊断信息） */
    private fun showOverlayMessageWithCopy(title: String, message: String) {
        val container = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(32, 24, 32, 24)
            setBackgroundColor(0xEE333333.toInt())
        }
        val titleView = TextView(this).apply {
            text = title; textSize = 16f; setTextColor(0xFFFFFFFF.toInt()); setPadding(0, 0, 0, 16)
        }
        val msgView = TextView(this).apply {
            text = message; textSize = 12f; setTextColor(0xFFDDDDDD.toInt())
            setLineSpacing(2f, 1f)
            // 文本可选中复制
            setTextIsSelectable(true)
        }
        val scroll = ScrollView(this).apply { addView(msgView) }
        val btnRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            setPadding(0, 16, 0, 0)
        }
        val btnCopy = Button(this).apply { text = "复制全部" }
        val btnClose = Button(this).apply { text = "关闭" }
        btnRow.addView(btnCopy, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        btnRow.addView(btnClose, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        container.addView(titleView)
        container.addView(scroll, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f))
        container.addView(btnRow)

        val lp = WindowManager.LayoutParams(
            WindowManager.LayoutParams.MATCH_PARENT,
            WindowManager.LayoutParams.MATCH_PARENT,
            windowType(),
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE,
            PixelFormat.TRANSLUCENT
        )
        btnCopy.setOnClickListener {
            val clipboard = getSystemService(android.content.ClipboardManager::class.java)
            clipboard.setPrimaryClip(android.content.ClipData.newPlainText("kma_diag", message))
            toast("已复制到剪贴板")
        }
        btnClose.setOnClickListener { removeView(container) }
        addView(container, lp)
    }

    /** 显示菜单型 overlay 对话框 */
    private fun showOverlayMenu(title: String, items: Array<String>,
                                 onClose: () -> Unit, onSelected: (Int) -> Unit) {
        val container = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(32, 24, 32, 24)
            setBackgroundColor(0xEE333333.toInt())
        }
        val titleView = TextView(this).apply {
            text = title; textSize = 16f; setTextColor(0xFFFFFFFF.toInt()); setPadding(0, 0, 0, 16)
        }
        container.addView(titleView)
        items.forEachIndexed { idx, label ->
            val btn = Button(this).apply {
                text = label
                setOnClickListener {
                    removeView(container)
                    onSelected(idx)
                }
            }
            container.addView(btn)
        }
        val btnCancel = Button(this).apply {
            text = "取消"
            setOnClickListener { removeView(container); onClose() }
        }
        container.addView(btnCancel)

        val lp = WindowManager.LayoutParams(
            WindowManager.LayoutParams.WRAP_CONTENT,
            WindowManager.LayoutParams.WRAP_CONTENT,
            windowType(),
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE,
            PixelFormat.TRANSLUCENT
        ).apply { gravity = Gravity.CENTER }
        addView(container, lp)
    }

    /** 显示数字输入型 overlay 对话框 */
    private fun showOverlayInput(title: String, default: String, onConfirm: (Int) -> Unit) {
        val container = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(32, 24, 32, 24)
            setBackgroundColor(0xEE333333.toInt())
        }
        val titleView = TextView(this).apply {
            text = title; textSize = 16f; setTextColor(0xFFFFFFFF.toInt()); setPadding(0, 0, 0, 16)
        }
        val edit = EditText(this).apply {
            inputType = InputType.TYPE_CLASS_NUMBER
            setText(default)
        }
        val btnRow = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        val btnOk = Button(this).apply {
            text = "确定"
            setOnClickListener {
                val v = edit.text.toString().toIntOrNull() ?: 0
                removeView(container); onConfirm(v)
            }
        }
        val btnCancel = Button(this).apply {
            text = "取消"
            setOnClickListener { removeView(container) }
        }
        btnRow.addView(btnOk, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        btnRow.addView(btnCancel, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        container.addView(titleView)
        container.addView(edit)
        container.addView(btnRow)

        // 输入框需要焦点 → 不能 FLAG_NOT_FOCUSABLE
        val lp = WindowManager.LayoutParams(
            WindowManager.LayoutParams.WRAP_CONTENT,
            WindowManager.LayoutParams.WRAP_CONTENT,
            windowType(),
            0,
            PixelFormat.TRANSLUCENT
        ).apply { gravity = Gravity.CENTER }
        addView(container, lp)
    }

    // ── 工具 ────────────────────────────────────────────
    private fun refreshStatus(devCount: Int = -1) {
        val dc = if (devCount >= 0) devCount
                 else NativeBridge.parseDevices(NativeBridge.nativeGetDevices()).size
        val root = if (rootGranted) "Root:✓" else "Root:✗"
        val input = if (inputStarted) "输入:✓($dc 设备)" else "输入:✗"
        val drv = if (mappingStarted) "驱动:✓ $driverName (运行中)"
                  else if (driverName.isNotEmpty()) "驱动:$driverName(未启动)"
                  else "驱动:未对接"
        statusText?.text = "$root\n$input\n$drv"
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

    private fun toast(msg: String) {
        mainHandler.post { Toast.makeText(this, msg, Toast.LENGTH_SHORT).show() }
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
