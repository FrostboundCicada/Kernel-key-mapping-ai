package com.kma.mapping

import java.util.concurrent.TimeUnit

/**
 * Root shell 包装器：通过 `su -c` 执行特权命令。
 *
 * 背景：Android 应用即使设备已 root，应用进程本身仍以普通 UID 运行，
 * 直接 `open("/dev/input/event*")` 会因权限不足失败（EACCES）。
 * 需通过 su 提权执行 `chmod 666` 把这些节点改为全局可读写，
 * 之后 native 层的 InputReader 才能正常打开。
 *
 * 同样地，twt / rt / qx / zero 等内核驱动创建的 /dev 节点也需要
 * chmod 后才能被应用访问。
 */
object RootShell {

    /** 是否已获得 root（su 可用且返回 uid=0） */
    fun hasRoot(): Boolean = try {
        val p = Runtime.getRuntime().exec(arrayOf("su", "-c", "id -u"))
        val finished = p.waitFor(5, TimeUnit.SECONDS)
        if (!finished) { p.destroy(); return false }
        p.inputStream.bufferedReader().readText().trim() == "0"
    } catch (e: Exception) { false }

    /** 执行命令，返回 stdout+stderr 合并文本 */
    fun exec(cmd: String, timeoutSec: Long = 10): String = try {
        val p = Runtime.getRuntime().exec(arrayOf("su", "-c", cmd))
        val out = p.inputStream.bufferedReader().readText()
        val err = p.errorStream.bufferedReader().readText()
        p.waitFor(timeoutSec, TimeUnit.SECONDS)
        out + if (err.isNotEmpty()) "\n[stderr] $err" else ""
    } catch (e: Exception) { "[exec 失败: ${e.message}]" }

    /** 授权 /dev/input/event* 和已知驱动节点为 666（全局可读写） */
    fun grantDevPermissions(): GrantResult {
        if (!hasRoot()) return GrantResult(false, "未获得 root（su 不可用）")
        val cmd = (
            "for d in /dev/input/event*; do [ -e \"\$d\" ] && chmod 666 \"\$d\"; done; " +
            "for d in /dev/twt* /dev/aim_touch /dev/rt_touch /dev/qx_touch /dev/zero_touch /dev/ovo_touch /dev/hakutaku; do " +
            "  [ -e \"\$d\" ] && chmod 666 \"\$d\"; " +
            "done; " +
            "echo GRANT_OK"
        )
        val out = exec(cmd)
        return if (out.contains("GRANT_OK")) GrantResult(true, out)
        else GrantResult(false, out)
    }

    /** 列出已加载的内核模块（用于检查 twt 等） */
    fun listModules(): String = exec("lsmod 2>/dev/null || cat /proc/modules 2>/dev/null")

    /** 检查指定模块是否已加载 */
    fun isModuleLoaded(name: String): Boolean = listModules().contains(name)

    /** 获取 /dev/input/event* 的 ls -la 输出（权限诊断用） */
    fun listInputNodes(): String = exec("ls -la /dev/input/event* 2>/dev/null")

    /** 获取 dmesg 末尾若干行（驱动加载诊断用，需 root） */
    fun dmesgTail(lines: Int = 30): String = exec("dmesg 2>/dev/null | tail -$lines")

    data class GrantResult(val ok: Boolean, val detail: String)
}
