# MI-RC003 Bridge · WebUSB 配置站点

这是一个纯静态网页，通过浏览器 **WebUSB API** 直接与 MI-RC003 Bridge 固件通信，
替代了原 RemoteMapper-ESP32 的 Wi-Fi 热点 + HTTP 网页配置方案。

## 功能

* 设备状态：固件版本、运行时间、BLE 状态、当前配置、音频帧数、内存占用
* 按键配置：5 个配置方案可视化编辑（单击 / 长按 / 双击 / 连发），支持键盘、多媒体、鼠标按键与鼠标移动/滚轮、**虚拟 Xbox 手柄**，并支持继承穿透；可视化键盘外提供「扩展按键」下拉框（F13-F24、撤销/剪切/复制/粘贴等常用键）；语音动作提供常用「语音快捷键」选择，鼠标滚轮以「方向 + 格数」配置；手柄动作可映射左右摇杆四方向与 L3/R3、方向键、A/B/X/Y、LB/RB、LT/RT、Select/Start/Xbox/截屏
* 多机型适配：页面读取设备当前遥控器 profile 及其按键列表，自动渲染对应遥控器（`dev.remoteInfo()` / `dev.setRemoteProfile()`）
* 配置切换：通过「按键配置」页的「配置切换模式…」按钮弹窗编辑全局切换映射（确定键锁定为默认配置，仅可修改四个方向键对应的配置）；固件默认长按电视键进入切换模式，也可在按键配置中把任意动作设为「进入配置切换模式」
* 蓝牙配对：扫描、连接、重新连接、解除绑定
* 实时同步：状态每秒刷新，设备侧配置变更（`config_rev`）自动重新读取，标签页切回 / 窗口聚焦时立即刷新
* 运行日志：查看 / 清空设备日志
* 系统设置：重启、恢复出厂、原始 JSON 导入导出

## 本地运行

WebUSB 要求页面处于安全上下文，`http://localhost` 属于安全来源：

```bash
cd webusb-config
python -m http.server 8000
```

然后打开 <http://localhost:8000/>，点击「连接设备」，选择 `MI-RC003 Remote Bridge`。

## 部署

将本目录上传到任意 **HTTPS** 静态托管（GitHub Pages / Cloudflare Pages / 自有服务器）即可。
若部署地址不是 `localhost:8000`，请同步修改固件 `main/version.h` 中的：

```c
#define WEBUSB_LANDING_URL     "your.site/path/"
#define WEBUSB_LANDING_SCHEME  1   // 0 = http, 1 = https
```

## 在线烧录页面

`flash/` 子目录是基于 [ESP Web Tools](https://esphome.github.io/esp-web-tools/) 的**浏览器固件烧录页**，
部署后访问 `https://<站点>/flash/`，用桌面版 Chrome / Edge 即可直接烧录固件（Web Serial）。

页面文件：`flash/index.html`、`flash/boards.json`、`flash/manifest-<板型>.json`、
`flash/firmware/merged-flash-<板型>.bin`。页面通过 `boards.json` 列出可用板型（N16R8 / N8R2 /
N4R2），用户选择后加载对应的 `manifest-<板型>.json` 再烧录。
其中 manifest、板型清单与固件均由根目录 `build-firmware.bat` 自动生成，请勿手工编辑。

## 通信协议

请求与响应均使用如下帧格式（详见仓库根目录 README）：

```text
'M' 'R'  cmd  status  len_lo  len_hi  payload...
```

`assets/mi-rc003.js` 封装了组帧、发送与响应解析（`dev.send(cmd, payload)`），
`assets/app.js` 只是基于该库的默认界面。第三方可只引入 `mi-rc003.js` 自行构建 UI。

- API 参考（权威，随库维护）：[`api.md`](./api.md)

## 兼容性

* 桌面版 Chrome / Edge（Windows / macOS / Linux）
* Windows 10/11 自动绑定 WinUSB，无需 Zadig
* Linux 需配置 udev 规则
* 移动端浏览器不支持 WebUSB
