package com.kma.mapping

import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity

/**
 * 入口 Activity：申请悬浮窗权限，启动 [MappingService]。
 */
class MainActivity : AppCompatActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(48, 80, 48, 48)
        }
        val title = TextView(this).apply {
            text = "Kernel-key-mapping-ai\n键鼠映射（内核驱动版）"
            textSize = 20f
            setPadding(0, 0, 0, 48)
        }
        val hint = TextView(this).apply {
            text = "使用说明:\n1. 授予悬浮窗权限\n2. 点击开启悬浮窗\n3. 点「创建新按键」，按下物理键绑定\n4. 长按虚拟键选择类型\n5. 点「开始映射」\n\n需 Root + 内核驱动(twt/rt/qx/zero)"
            textSize = 14f
            setPadding(0, 0, 0, 48)
        }
        val btnOpen = Button(this).apply { text = "开启悬浮窗" }
        val btnStop = Button(this).apply { text = "停止服务" }

        btnOpen.setOnClickListener {
            if (!Settings.canDrawOverlays(this)) {
                requestOverlay()
            } else {
                startMappingService()
            }
        }
        btnStop.setOnClickListener {
            stopService(Intent(this, MappingService::class.java))
            toast("服务已停止")
        }

        root.addView(title)
        root.addView(hint)
        root.addView(btnOpen)
        root.addView(btnStop)
        setContentView(root)
    }

    private fun requestOverlay() {
        val intent = Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION, Uri.parse("package:$packageName"))
        overlayLauncher.launch(intent)
    }

    private val overlayLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) {
        if (Settings.canDrawOverlays(this)) startMappingService()
        else toast("需要悬浮窗权限")
    }

    private fun startMappingService() {
        val intent = Intent(this, MappingService::class.java)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) startForegroundService(intent)
        else startService(intent)
        toast("悬浮窗已开启")
        // 可选：移到后台让悬浮窗接管
    }

    private fun toast(msg: String) {
        Toast.makeText(this, msg, Toast.LENGTH_SHORT).show()
    }
}
