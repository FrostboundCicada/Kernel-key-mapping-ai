# Kernel-key-mapping-ai

通过 **rt / qx / zero / twt** 等内核驱动模拟触摸事件，把物理**键鼠外设**映射为屏幕触控 / 陀螺仪视角，用于 Android 上的键鼠映射。

## 工作原理

```
物理键鼠 ──/dev/input/event*──► InputReader (epoll)
                                      │
                                      ▼
                                   Mapper (按键→触点, 鼠标→拖拽/陀螺仪)
                                      │
                                      ▼
                              DriverAdapter (统一接口)
                                      │
                ┌─────────────────────┴─────────────────────┐
                ▼                                           ▼
        TwtDriver                                  InputHandleDriver
   (syscall+ioctl 'T', 多slot+陀螺仪)         (ioctl 'A', 单slot, rt/qx/zero/ovo...)
                │                                           │
                ▼                                           ▼
         真实触摸屏 input_dev                      真实触摸屏 input_dev
```

- **不创建虚拟设备**：所有触摸事件都注入到真实触摸屏的 `input_dev`，反作弊扫描 `/proc/bus/input/devices` 无法区分。
- **多驱动适配**：自动探测可用内核驱动，twt 优先（多 slot + 陀螺仪，反检测最强）。

## 支持的内核驱动

| 驱动 | 对接方式 | 设备节点 / 获取方式 | 多slot | 陀螺仪 |
|------|----------|----------------------|--------|--------|
| **twt** | syscall(`__NR_reboot` magic `0x114514/0x1919810/0x2778`) + ioctl `'T'` | anon_inode 含 `TwT_driver` | ✅ | ✅ |
| **aim_touch** 风格 | ioctl `'A'` | `/dev/aim_touch` | ❌(单slot) | ❌ |
| **rt** | 同上 | `/dev/rt_touch` | ❌ | ❌ |
| **qx** | 同上 | `/dev/qx_touch` | ❌ | ❌ |
| **zero** | 同上 | `/dev/zero_touch` | ❌ | ❌ |
| **ovo / hakutaku** | 同上 | `/dev/ovo_touch` `/dev/hakutaku` | ❌ | ❌ |

> twt 对接约定来自上传的 `对接例子v1.48.zip` 中的 `kernel.h`：ioctl 命令 `TOUCH_INIT/DOWN/UP`(`'T',6/7/8`)、`GYRO_INIT/CONFIG`(`'T',9/10`)，结构体 `{int slot,x,y}`，陀螺仪 `{uint32 enable,x,y}`(x/y 为 float memcpy)。
> aim_touch 风格 ioctl 命令 `DOWN/MOVE/UP/INIT`(`'A',1/2/3/4`)，结构体 `{int x,y,pressure,touch_major,tracking_id}`。

## 构建

需要 C++17。Android NDK 交叉编译（目标 aarch64）：

```bash
mkdir build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 ..
make -j
```

或在 Linux 上直接编译测试逻辑：

```bash
mkdir build && cd build && cmake .. && make -j
```

## 运行

设备需 **Root**，且已加载对应内核驱动（如 `insmod aim_touch.ko && chmod 666 /dev/aim_touch`）。

```bash
./kma_mapper -c configs/default.conf
# 常用选项:
#   -c <file>   配置文件 (默认 configs/default.conf)
#   -d auto|twt|input   指定驱动
#   -s 1080x2400        屏幕分辨率
#   -i <name>           只匹配名字含 name 的输入设备(精确指定物理键鼠)
```

输出示例：
```
[配置] 屏幕 1080x2400  驱动:auto  鼠标模式:gyro  灵敏度:1.20
[驱动] 已对接: TwT (陀螺仪:支持)
[输入] 已打开 2 个设备:
  /dev/input/event3 [Logitech USB Keyboard] key=1 mouse=0
  /dev/input/event4 [Logitech USB Mouse] key=0 mouse=1
[就绪] 映射运行中，Ctrl+C 退出
```

## 配置文件

简洁文本格式，见 `configs/default.conf`：

```
screen 1080 2400
driver auto            # auto | twt | input
mouse_mode gyro        # drag | gyro  (gyro 仅 twt 后端有效)
sensitivity 1.2        # drag 模式像素缩放
gyro_sensitivity 8.0   # gyro 模式 1像素位移 → 角速度系数

# 按键映射: key <KEY名> <动作> <x> <y>
#   动作: click(短按一次) | hold(按住触摸) | toggle(按一次切换)
key KEY_W hold 540 1700
key KEY_A hold 360 1700
key KEY_S hold 540 1900
key KEY_D hold 720 1700
key KEY_SPACE click 540 1300

# 鼠标按键
mouse_left hold 920 2050    # 开火
mouse_right hold 540 1200   # 瞄准
```

支持的按键名：`KEY_A`~`KEY_Z`、`KEY_0`~`KEY_9`、`KEY_SPACE`、`KEY_ENTER`、`KEY_ESC`、`KEY_TAB`、`KEY_LEFTSHIFT`、方向键等；鼠标 `MOUSE_LEFT/RIGHT/MIDDLE/SIDE/EXTRA`。也可直接填数字按键码。

## 映射模式

- **按键 → 触点**：每个按键映射到固定屏幕坐标，`hold` 持续触摸、`click` 短按、`toggle` 切换。twt 多按键可同时生效（多 slot）；aim_touch 风格单点，后按下者抢占。
- **鼠标 drag**：鼠标相对位移驱动一个触摸点拖拽（压枪/拖动准星）。
- **鼠标 gyro**（仅 twt）：鼠标位移转陀螺仪角速度，FPS 视角转动，**无任何触摸事件**，反检测最强。松手后角速度衰减归零。

## 工程结构

```
src/
├── driver_adapter.{h,cpp}  # 驱动统一接口 + TwtDriver + InputHandleDriver + 探测工厂
├── input_reader.{h,cpp}    # /dev/input/event* 键鼠读取 + epoll
├── mapper.{h,cpp}          # 映射引擎 + 配置解析 + slot 管理
└── main.cpp                # 主程序
configs/default.conf        # 默认映射配置
CMakeLists.txt
```

## 权限与免责

- 读取 `/dev/input/event*` 需 root 或 input 组权限。
- 内核驱动需自行编译加载（参考各驱动文档），本项目不含驱动本体。
- 仅供学习内核 input 子系统与外设映射原理研究。
