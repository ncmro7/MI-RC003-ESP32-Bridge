# MI-RC003-ESP32-Bridge

基于 **ESP-IDF** 的 ESP32-S3 固件：把 **小米蓝牙遥控器 2 Pro（RC003）** 的按键与语音，
通过一个 USB 复合设备桥接到 Windows，并用 **浏览器 WebUSB** 直接完成配置。

> **配置站点（WebUSB）**：<https://ncmro7.github.io/MI-RC003-ESP32-Bridge/>
> **在线烧录**：<https://ncmro7.github.io/MI-RC003-ESP32-Bridge/flash/>

> [!WARNING]
> **⚠️ 本项目由 AI 辅助开发，尚未完善，可能存在较多问题，请谨慎使用。**
> 如需稳定可用的方案，建议使用
> [cuicui-V5/RemoteMapper-ESP32](https://github.com/cuicui-V5/RemoteMapper-ESP32)。

---

## 1. 这是什么

遥控器通过蓝牙（HOGP + 私有 ATVV 语音协议）连到 ESP32-S3，ESP32-S3 再以 **USB 复合设备**
的身份接入电脑，提供标准 UAC 麦克风、HID 键盘/多媒体/鼠标，并额外提供一个 WebUSB 厂商接口。
浏览器打开配置站点即可读写设备，**无需 Wi-Fi 热点、无需安装驱动**。

它解决的问题：

1. Windows 蓝牙键盘驱动会丢弃遥控器的 `音量+ (0x80)`、`音量- (0x81)`、`返回 (0xF1)` 等非标准键码。
2. 语音走私有 ATVV 协议，纯软件方案需要虚拟声卡。
3. 原方案必须让 ESP32 开热点、用户切换 Wi-Fi 才能配置，体验割裂。

### 主要特性

| 能力 | 说明 |
| :--- | :--- |
| BLE 直连 | 作为 BLE Central 连接 RC003（HOGP 按键 + ATVV 语音），支持扫描/绑定/重连 |
| USB 复合设备 | UAC 1.0 麦克风 + HID 键盘/多媒体/鼠标 + WebUSB 厂商接口 |
| 语音麦克风 | 16 kHz / 16-bit 单声道，IMA-ADPCM 解码 + AGC，按住语音键推流 |
| 多配置方案 | 5 套配置（默认 + 配置 1~4），各配置按键映射相互独立，支持「穿透继承」默认配置 |
| 动作类型 | 键盘（单击/按住/释放）、多媒体、鼠标按键（单击/按住）、鼠标移动、鼠标滚轮、语音、进入配置切换模式 |
| 手势 | 单击 / 长按 / 双击 / 连发，每种手势可独立配置 |
| 配置切换模式 | 遥控器上即可切换配置：长按电视键进入，方向键选择，LED 呼吸提示（见第 6 节） |
| 浏览器配置 | 纯静态站点，WebUSB 直连；可视化遥控器、可视化键盘、快捷预设、实时状态 |
| 实时同步 | 状态每秒刷新，设备侧配置变更自动重读，标签页切回立即刷新 |
| 一键烧录 | 网页在线烧录（ESP Web Tools）+ Windows 免安装 `flash.bat`（内置 esptool） |

---

## 2. 系统拓扑

```text
┌────────────────────────┐   BLE 5.0 (HOGP + ATVV)   ┌────────────────────────┐
│  小米蓝牙遥控器 2 Pro  │ ────────────────────────► │      ESP32-S3          │
│  (RC003)               │ ◄──────────────────────── │   (BLE Central)        │
└────────────────────────┘                           └───────────┬────────────┘
                                                                 │ USB 2.0 Full-Speed
                                                                 │ 复合设备
                                                                 ▼
                                                     ┌────────────────────────┐
                                                     │        PC / Windows    │
                                                     │ • UAC 1.0 麦克风       │
                                                     │ • HID 键盘 + 多媒体键  │
                                                     │ • WebUSB 配置接口 ◄────┼── 浏览器访问配置站点
                                                     └────────────────────────┘
```

USB 复合设备包含 4 个接口：

| 接口 | 类 | 端点 | 说明 |
| :--- | :--- | :--- | :--- |
| 0 / 1 | Audio (UAC 1.0) | ISO IN `0x81` | 16 kHz / 16-bit / 单声道麦克风 |
| 2 | HID | INT IN `0x82` | 键盘（Report ID 1）+ 多媒体（Report ID 2）+ 鼠标（Report ID 3） |
| 3 | Vendor (WebUSB) | BULK OUT `0x05` / BULK IN `0x83` | WebUSB 配置通道 |

> **烧录与调试**
> * 复合设备占用的是 ESP32-S3 的**原生 USB（GPIO19/20，OTG）**口，运行时它同时提供
>   麦克风、键盘和 WebUSB。
> * 若要通过该 USB 口**烧录固件**：按住开发板 `BOOT` 键，点按一下 `RST`，松开 `BOOT`，
>   芯片会进入 ROM 的 USB-Serial-JTAG 引导模式，然后执行 `idf.py -p COMx flash` 即可。
> * 也可继续使用板载 UART 口（GPIO43/44）烧录与查看日志，二者互不影响。

---

## 3. 硬件要求

* **ESP32-S3-WROOM-1 开发板**，支持以下板型：

  | 板型 | Flash | PSRAM | 固件构建 profile |
  | :--- | :--- | :--- | :--- |
  | N16R8（推荐） | 16MB | 8MB Octal | `n16r8` |
  | N8R2 | 8MB | 2MB Quad | `n8r2` |
  | N4R2 | 4MB | 2MB Quad | `n4r2` |

  请使用与板型匹配的 profile 编译和烧录。N4R2 的应用分区为 2.75MB；若今后固件超过该大小，需先精简功能或调整分区表。
* 板载 WS2812 RGB 指示灯（默认 GPIO48，可在 `main/app_config.h` 修改 `RGB_BUILTIN`）。
* **小米蓝牙语音遥控器 2 Pro（型号 RC003）**。
* 两根 Type-C 数据线（或一根）：一根接 **USB/OTG** 口（必须，用于复合设备与 WebUSB），
  UART 口可选用于查看串口日志。

---

## 4. 快速开始

### 4.1 烧录固件

**方式一：网页在线烧录（推荐）**

打开 <https://ncmro7.github.io/MI-RC003-ESP32-Bridge/flash/>，用桌面版 Chrome / Edge 直接烧录
（Web Serial）。设备需先进入 ROM 下载模式：按住 `BOOT` → 点按 `RST` → 松开 `BOOT`。

**方式二：Windows 免安装工具**

从 [Releases](https://github.com/ncmro7/MI-RC003-ESP32-Bridge/releases) 下载
`MI-RC003-Bridge-<版本>-win64.zip`，解压后双击 `flash.bat`。内置 esptool，**无需安装
Python / ESP-IDF**，自动探测串口并烧录。

### 4.2 连接配置站点

1. 用 USB 数据线把开发板接到电脑（推荐原生 USB/OTG 口）。
2. 桌面版 Chrome / Edge 打开 <https://ncmro7.github.io/MI-RC003-ESP32-Bridge/>。
3. 点击「连接设备」，选择 `MI-RC003 Remote Bridge`。
4. 首次使用：拿起遥控器，同时按住「主页键 + 菜单键」约 3 秒进入配对广播，
   在「蓝牙配对」页扫描并连接。

---

## 5. 配置站点（WebUSB）

配置页面位于 [`webusb-config/`](./webusb-config)，是一个**纯静态站点**，不依赖任何后端。

页面标签页：

* **设备状态**：固件版本、运行时间、BLE 状态、遥控器电量、当前配置、切换模式、内存占用，
  以及实时按键检测（按下键、最近动作、键值、时长）。
* **按键配置**：5 套配置方案的可视化遥控器编辑；点击任意按键可配置单击/长按/双击/连发，
  支持可视化键盘、多媒体分组、鼠标按键/移动/滚轮、语音快捷键预设；另有「配置切换模式…」
  按钮弹窗编辑配置切换映射。
* **蓝牙配对**：扫描、连接、重新连接、解除绑定。
* **运行日志**：查看 / 清空设备日志。
* **系统设置**：重启、恢复出厂、原始 JSON 导入导出。

### 5.1 本地运行

WebUSB 要求安全上下文，`http://localhost` 被视为安全来源：

```powershell
cd webusb-config
python -m http.server 8000
```

浏览器打开 <http://localhost:8000/>，点击「连接设备」，选择 `MI-RC003 Remote Bridge`。

> 固件 BOS 描述符中的着陆页地址默认为
> `https://ncmro7.github.io/MI-RC003-ESP32-Bridge/`；本地开发时可在 `main/version.h`
> 的 `WEBUSB_LANDING_URL` / `WEBUSB_LANDING_SCHEME` 中改回 `http://localhost:8000/`。

### 5.2 部署

将 `webusb-config/` 目录部署到任意 **HTTPS** 静态站点（如 GitHub Pages），并同步修改
`WEBUSB_LANDING_URL` 与 `WEBUSB_LANDING_SCHEME`（`1` = https）后重新编译固件。
推送到 `main` 后 GitHub Pages 会自动部署。

### 5.3 二次开发

设备通信已封装为无依赖的浏览器库
[`webusb-config/assets/mi-rc003.js`](./webusb-config/assets/mi-rc003.js)，第三方可只写自己的
HTML/JS 调用该 API，完全替换默认 UI。完整 API 参考见
[`webusb-config/api.md`](./webusb-config/api.md)（权威，随库维护）。

### 5.4 浏览器兼容性

* 桌面版 **Chrome / Edge**（Windows / macOS / Linux）支持 WebUSB。
* Android / iOS 浏览器**不支持** WebUSB。
* Windows 10/11 会自动为带 BOS 描述的厂商接口绑定 WinUSB，**无需 Zadig**。
* Linux 需要 udev 规则允许普通用户访问该设备。

---

## 6. 配置切换模式

在遥控器上直接切换配置方案，无需打开网页。

**进入**

* 出厂默认：**长按「电视键」**进入（短按电视键仍为 `F8`）。
* 也可在配置站点把任意按键的任意手势设为「进入配置切换模式」。
* 兜底：若当前配置未设置切换按键，可**快速连按「电视键」5 次**（1.5 秒内）进入。

**模式内**

* 指示灯以**当前配置颜色呼吸闪烁**，同时屏蔽普通按键输出。
* 按「确定键」→ 切回**默认配置**（固定，不可修改）。
* 按方向键 → 切换到对应配置并**自动退出**。默认映射：上→配置 1、右→配置 2、下→配置 3、左→配置 4；
  可在配置站点「配置切换模式…」弹窗中修改，**仅可修改四个方向键**，一个按键对应一个配置。
* 「返回键」始终可退出；电视键仅在「由连按 5 次进入」或「当前配置绑定为切换键」时可退出
  （进入满 2 秒后生效）。
* 约 **5 秒无操作**自动退出（模式内任意按键都会重新计时）。

---

## 7. 默认出厂按键映射

| 按键 | 键码 | 单击 | 长按 | 双击 |
| :--- | :--- | :--- | :--- | :--- |
| 电源键 | `0x66` | `Alt + Tab` | 系统休眠 | — |
| 语音键 | `0x04` | 麦克风推流 + `RAlt + ,` | 持续录音 | — |
| 方向上/下/左/右 | `0x52/51/50/4F` | 方向键 | — | — |
| 确定键 | `0x28` | 回车 | — | — |
| 返回键 | `0xF1` | 多媒体返回 | — | — |
| 主页键 | `0x24` | `Win + D` | — | — |
| 菜单键 | `0x5D` | 空格 | — | — |
| 音量 +/− | `0x80/0x81` | 音量调节（支持连发） | — | — |
| 电视键 | `0xC0` | `F8` | 进入配置切换模式 | — |

所有映射均可在配置站点中自由修改。支持 5 个配置方案与单击/长按/双击/连发，动作类型可选
键盘、多媒体，以及**鼠标按键（左/右/中/后退/前进）与鼠标移动/滚轮**。鼠标移动可选上/下/左/右
方向并设置速度，按住持续移动，松开即停；滚轮可选滚动方向与每次格数。鼠标按键提供单击与
按住两种（按住时松开自动释放）；「语音」动作可配置 Windows 语音快捷键（默认 `RAlt + ,`）。

> 说明：RC003 的 HOGP 输入报文是「Report ID 1 + 3 个小端 16-bit 键盘 usage」。
> 固件会将其解析并归一化为上表的内部键码（例如 HID usage `0x4A/0x65/0x35` 分别
> 归一化为主页/菜单/电视键），同时兼容标准 8 字节键盘报文。

各配置方案相互独立：只有在当前配置里设置过的按键才会生效；若想让某个手势沿用默认配置，
把该动作设为「穿透继承」。

---

## 8. 编译（ESP-IDF v6.1）

### 8.1 环境准备

安装 [ESP-IDF v6.1](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/)
并激活环境（Windows PowerShell）：

```powershell
. "C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1"
```

首次编译会自动通过 IDF Component Manager 下载依赖：
`espressif/esp_tinyusb`、`espressif/tinyusb`、`bblanchon/arduinojson`、`espressif/led_strip`。

### 8.2 编译与烧录

```powershell
idf.py set-target esp32s3   # 首次 / 更换硬件

# N16R8（默认）
idf.py build

# N8R2 / N4R2：通过 CMake 参数指定基础配置及板型覆盖
idf.py -D "SDKCONFIG=.sdkconfig.n8r2" -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.n8r2" build
idf.py -D "SDKCONFIG=.sdkconfig.n4r2" -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.n4r2" build

idf.py -p COMx flash monitor
```

也可使用一键发布脚本选择板型（默认 `n16r8`）：

```bat
build-firmware.bat -Profile n16r8
build-firmware.bat -Profile n8r2
build-firmware.bat -Profile n4r2
```

每次切换 profile 后，请重新执行 `idf.py build`；烧录工具与网页烧录目录会保存本次构建对应的固件。

---

## 9. 发布流程

发布产物分两步，均使用仓库内脚本（版本号唯一来源是 `main/version.h` 的 `FIRMWARE_VERSION`）：

```bat
build-firmware.bat            :: 编译 + 生成两份烧录固件
package-release.bat           :: 打包 Windows 免安装烧录工具到 dist\
```

| 产物 | 说明 |
| :--- | :--- |
| `build/merged-flash.bin` | Windows 免安装烧录工具用的合并固件 |
| `webusb-config/flash/firmware/merged-flash.bin` + `manifest.json` | 网页烧录固件（受版本控制） |
| `dist/MI-RC003-Bridge-<版本>-win64/` | 免安装 Windows 烧录包（内置 esptool + `flash.bat`） |
| `dist/MI-RC003-Bridge-<版本>-win64.zip` | 发布压缩包，上传 GitHub Release |
| `dist/SHA256SUMS.txt` | 发布包 SHA-256 |

最终用户拿到的 Windows 包结构：`flash.bat`、`flash.ps1`、`esptool.exe`、`使用说明.txt`、
`firmware/merged-flash.bin`。

> `dist/`、`build/`、`sdkconfig`、`managed_components/` 均已在 `.gitignore` 中忽略，不要提交。

---

## 10. WebUSB 通信协议

浏览器与设备之间通过厂商 BULK 端点传输带帧头的 JSON 数据。

**请求帧（主机 → 设备）**

```text
偏移  0     1     2     3        4-5          6..N
      'M'   'R'   cmd   rsvd     len (LE16)   payload (UTF-8 / JSON)
```

**响应帧（设备 → 主机）**

```text
偏移  0     1     2     3        4-5          6..N
      'M'   'R'   cmd   status   len (LE16)   payload
```

`status == 0` 表示成功。命令字：

| cmd | 名称 | 说明 |
| :--- | :--- | :--- |
| `0x01` | STATUS | 运行状态 / 内存 / BLE 状态 / 当前配置 / 切换模式 / `config_rev` |
| `0x02` `0x03` | LOGS_GET / CLEAR | 运行日志 |
| `0x10` `0x11` `0x12` `0x13` | KEYMAP_GET / SAVE / RESET / TELEMETRY | 多配置方案按键映射 |
| `0x15` `0x16` `0x17` | KEYMAP_BEGIN / DATA / COMMIT | 分块上传保存 |
| `0x18` | SET_LAYER | 切换设备当前配置（层） |
| `0x20` `0x21` `0x22` `0x23` `0x24` | BLE_SCAN / CONNECT / UNPAIR / INFO / RECONNECT | 蓝牙配对管理 |
| `0x31` | NVS_RESET | 恢复出厂设置 |
| `0x40` | SYSTEM_RESTART | 重启设备 |
| `0x50` | DEVICE_INFO | 设备信息 |

完整命令字、常量与返回字段见 [`webusb-config/api.md`](./webusb-config/api.md)。

---

## 11. 项目结构

```text
MI-RC003-ESP32-Bridge/
├── CMakeLists.txt
├── sdkconfig.defaults
├── partitions.csv
├── build-firmware.bat                         # 一键生成烧录固件（Windows + 网页）
├── package-release.bat                        # 打包 Windows 免安装烧录工具
├── AGENTS.md                                  # 编译/发布/WebUSB API 约定
├── tools/
│   ├── common.ps1                             # 脚本公共函数
│   ├── build-firmware.ps1                     # 生成 build/merged-flash.bin 与网页固件
│   ├── package-release.ps1                    # 打包 dist/ Windows 烧录工具
│   └── standalone/{flash.bat,flash.ps1}       # 最终用户免安装烧录脚本模板
├── main/
│   ├── main.c                     # 初始化与任务
│   ├── app_config.h / version.h
│   ├── ble/ble_remote_client.*    # NimBLE Central: HOGP + ATVV
│   ├── audio/                     # IMA-ADPCM / AGC / 滤波 / 环形缓冲
│   ├── keymap/                    # 多配置按键状态机 + 配置切换模式 + NVS 配置
│   ├── storage/config_store.*     # NVS 封装
│   ├── usb/
│   │   ├── usb_descriptors.*      # 设备/配置/BOS/WebUSB/HID 描述符
│   │   ├── uac_microphone.*       # 自定义 UAC 1.0 类驱动
│   │   ├── hid_bridge.*           # HID 键盘 + 多媒体 + 鼠标发送
│   │   ├── webusb_transport.*     # 厂商端点帧协议
│   │   └── usb_composite.*
│   ├── webusb/webusb_protocol.cpp # WebUSB 命令分发
│   ├── led/led_indicator.*        # RGB 指示灯（状态 / 呼吸）
│   └── log/app_log.*
└── webusb-config/                 # 浏览器配置站点（静态）
    ├── index.html
    ├── api.md                      # 浏览器库 API 参考（权威）
    ├── doc.md                      # 旧版说明
    ├── assets/{mi-rc003.js,app.js,style.css}
    └── flash/                     # 网页固件烧录（ESP Web Tools）
        ├── index.html
        ├── manifest.json
        └── firmware/merged-flash.bin
```

---

## 12. 常见问题

### 烧录工具检测不到串口

这通常是**设备正运行固件、原生 USB 口不提供串口**：

* 本项目运行时，原生 USB（OTG）口是 UAC + HID + WebUSB 复合设备，**不会出现 COM 口**。
  烧录前先让芯片进入 ROM 下载模式：按住 `BOOT` → 点按 `RST` → 松开 `BOOT`
  （设备管理器中会出现 `USB JTAG serial debug unit` 并分配 COM 口）。
* 工具会自动等待最多 90 秒，进入下载模式后即可自动开始烧录。
* 也可改用板载 **UART 口**（GPIO43/44，需 CH34x / CP210x 驱动）。

### Windows 设备管理器提示「代码 28 / 驱动程序未安装」

固件在 BOS 描述符中同时提供 **WebUSB** 与 **Microsoft OS 2.0（WINUSB 兼容 ID）** 描述符，
Windows 10/11 应自动为该厂商接口加载 `winusb.sys`。若仍出现代码 28：确认烧录的是最新固件
（PID 为 `0x8304`），在设备管理器中卸载残留旧设备后重新扫描；无需 Zadig。

### 串口日志出现 `NIMBLE_NVS: NVS data size mismatch`

之前烧录过 Arduino 版 RemoteMapper，其 NimBLE 绑定数据结构与本固件不同。固件已内置一次性
NVS 迁移；若仍有问题，可执行 `idf.py erase-flash` 后再烧录。

### 浏览器找不到设备

* 必须使用桌面版 Chrome / Edge；移动端浏览器不支持 WebUSB。
* 页面必须运行在 `https://` 或 `http://localhost`（安全上下文）。
* 关闭可能占用该设备的其他程序（如串口助手、Zadig）。
* 首次使用需在弹窗中选择 `MI-RC003 Remote Bridge`。

### 语音键没有声音 / 输入法无法采集

在 Windows 声音设置中把输入设备切换为 `MI-RC003 Microphone`，并确认输入法语音热键与遥控器
语音键配置一致（默认 `右Alt + ,`）。

---

## 13. 致谢

* [cuicui-V5/RemoteMapper-ESP32](https://github.com/cuicui-V5/RemoteMapper-ESP32)：硬件桥接架构、按键引擎与音频处理。
* [HD838A/remote-mic-app](https://github.com/HD838A/remote-mic-app)：RC003 的 HOGP/ATVV 协议细节与 UI 参考。
* [QL-4/RemoteMapper](https://github.com/QL-4/RemoteMapper)、[godarrenw/mi_remote_control](https://github.com/godarrenw/mi_remote_control)：协议逆向参考。
* [TinyUSB](https://github.com/hathach/tinyusb) 与 [esp_tinyusb](https://github.com/espressif/esp-usb)。

## 14. 开源协议

[MIT License](./LICENSE)
