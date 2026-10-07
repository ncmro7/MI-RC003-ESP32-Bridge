/*
 * MI-RC003 Bridge - default WebUSB configuration UI.
 *
 * This file is only the presentation layer. All device communication goes
 * through the reusable `MiRC003` client (assets/mi-rc003.js). Third parties can
 * replace this file entirely and build their own UI on the same API; see
 * api.md for the reference.
 */
(function () {
  "use strict";

  // WebUI version (independent of the firmware version). Bump on UI changes.
  const WEBUI_VERSION = "1.5";

  const dev = new MiRC003();
  const ACTION = MiRC003.ACTION;
  // Physical keys come from the device's active remote profile (see
  // dev.remoteInfo()); this default list is used until the device is read.
  let PHYSICAL_KEYS = MiRC003.PHYSICAL_KEYS;
  const MOD_BITS = MiRC003.MOD_BITS;
  const MOD_NAMES = {};
  MOD_BITS.forEach(([bit, name]) => { MOD_NAMES[bit] = name; });
  const MOUSE_BUTTONS = MiRC003.MOUSE_BUTTONS;
  const HID_GROUPS = MiRC003.HID_GROUPS;
  const HID_EXTRA_GROUPS = MiRC003.HID_EXTRA_GROUPS;
  const CONSUMER_GROUPS = MiRC003.CONSUMER_GROUPS;
  const GAMEPAD_CONTROLS = MiRC003.GAMEPAD_CONTROLS;
  const gamepadName = MiRC003.gamepadName;
  const Keymap = MiRC003.Keymap;

  // Remote profile state (multi-remote adaptation). The default is shown
  // before the device is read (or on firmware without REMOTE_INFO support).
  let remoteProfiles = [];
  let activeRemoteProfile = null;
  const DEFAULT_REMOTE_MODEL = { id: "rc003", name: "MIRC 2 Pro" };

  const ICON = {
    power: '<svg viewBox="0 0 24 24"><path d="M12 3v9M7.05 5.93a8 8 0 1 0 9.9 0"/></svg>',
    voice: '<svg viewBox="0 0 24 24"><rect x="9" y="3" width="6" height="11" rx="3"/><path d="M5.5 11a6.5 6.5 0 0 0 13 0M12 17.5V21M8.5 21h7"/></svg>',
    back: '<svg viewBox="0 0 24 24"><path d="M19 12H5M11 18l-6-6 6-6"/></svg>',
    home: '<svg viewBox="0 0 24 24"><path d="M4 11.2 12 4l8 7.2V20H4z"/></svg>',
    menu: '<svg viewBox="0 0 24 24"><path d="M5 7h14M5 12h14M5 17h14"/></svg>',
  };
  const DPAD_ICON = {
    up: '<svg viewBox="0 0 24 24"><path d="M12 6.5l6 11h-12z"/></svg>',
    down: '<svg viewBox="0 0 24 24"><path d="M12 17.5l-6-11h12z"/></svg>',
    left: '<svg viewBox="0 0 24 24"><path d="M6.5 12l11-6v12z"/></svg>',
    right: '<svg viewBox="0 0 24 24"><path d="M17.5 12l-11 6v-12z"/></svg>',
  };

  let keymap = null;
  let activeLayer = 0;          // configuration currently being edited
  let deviceActiveLayer = 0;    // configuration currently active on the device
  let dirty = false;            // unsaved keymap edits
  let logTimer = null;
  let statusTimer = null;
  let telemetryTimer = null;
  let savingKeymap = false;
  let lastConfigRev = null;     // device config revision seen by the UI

  const $ = (id) => document.getElementById(id);

  /* ------------------------- helpers ------------------------- */

  function setConnected(on) {
    $("conn-dot").classList.toggle("on", on);
    $("conn-text").textContent = on ? "已连接" : "未连接";
    $("btn-connect").disabled = on;
    $("btn-disconnect").disabled = !on;
    renderRemoteSelector();
  }

  function formatUptime(sec) {
    const h = Math.floor(sec / 3600);
    const m = Math.floor((sec % 3600) / 60);
    const s = sec % 60;
    return `${h}时${m}分${s}秒`;
  }
  function formatBytes(n) {
    if (n === undefined || n === null) return "-";
    return (n / 1024).toFixed(1) + " KB";
  }
  function escapeHtml(str) {
    return String(str).replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
  }

  let toastTimer = null;
  function toast(msg, isError) {
    const el = $("toast");
    el.textContent = msg;
    el.classList.toggle("error", !!isError);
    el.classList.remove("hidden");
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => el.classList.add("hidden"), 2600);
  }

  function updateDirtyIndicator() {
    const btn = $("btn-keymap-save");
    if (btn) btn.classList.toggle("dirty", dirty);
  }

  /* ------------------------- connection ------------------------- */

  async function connect() {
    try {
      await dev.connect();
      lastConfigRev = null;
      setConnected(true);
      toast("设备已连接");
      await loadDeviceInfo();
      await loadRemoteInfo();
      await refreshStatus();
      await loadKeymap();
      await refreshBleInfo();
      startAutoRefresh();
    } catch (err) {
      console.error(err);
      toast("连接失败: " + err.message, true);
    }
  }

  async function disconnect() {
    stopAutoRefresh();
    await dev.disconnect();
    setConnected(false);
    toast("已断开");
  }

  /* ------------------------- data ------------------------- */

  async function loadDeviceInfo() {
    try {
      $("device-info").textContent = JSON.stringify(await dev.deviceInfo(), null, 2);
    } catch (e) { /* ignore */ }
  }

  /* ------------------------- remote profile ------------------------- */

  async function loadRemoteInfo() {
    try {
      const info = await dev.remoteInfo();
      remoteProfiles = info.profiles || [];
      activeRemoteProfile = info.active || null;
      if (Array.isArray(info.keys) && info.keys.length) {
        PHYSICAL_KEYS = info.keys.map((k) => ({ vk: k.vk, name: k.name }));
      }
    } catch (e) { /* firmware without REMOTE_INFO support */ }
    renderRemoteSelector();
  }

  function renderRemoteSelector() {
    const sel = $("remote-profile");
    if (!sel) return;
    // Never leave the selector blank: fall back to the default model when the
    // device has not been read yet (or predates the REMOTE_INFO command).
    const list = remoteProfiles.length ? remoteProfiles : [DEFAULT_REMOTE_MODEL];
    const active = activeRemoteProfile || list[0].id;
    sel.innerHTML = list.map((p) =>
      `<option value="${escapeHtml(p.id)}" ${p.id === active ? "selected" : ""}>${escapeHtml(p.name)}</option>`
    ).join("");
    sel.disabled = !dev.isConnected() || list.length <= 1;
  }

  async function refreshStatus() {
    try {
      const s = await dev.status();
      $("st-firmware").textContent = s.firmware || "-";
      $("st-version").textContent = s.version || "-";
      $("st-build").textContent = s.build || "-";
      $("st-uptime").textContent = formatUptime(s.uptime_sec || 0);
      $("st-ble").textContent = ["未连接", "扫描中", "连接中", "已连接", "语音中"][s.ble_state] || s.ble_state;
      $("st-battery").textContent = (typeof s.battery === "number" && s.battery >= 0) ? s.battery + "%" : "未知";
      const devLayer = s.active_layer ?? 0;
      const activeMode = getLayer(devLayer);
      $("st-layer").textContent = activeMode ? configLabel(activeMode) : ("配置" + devLayer);
      if (devLayer !== deviceActiveLayer) {
        deviceActiveLayer = devLayer;
        if (keymap) renderLayerTabs();
      }
      $("st-frames").textContent = s.frames_decoded ?? 0;
      $("st-heap").textContent = formatBytes(s.free_heap);
      $("st-psram").textContent = formatBytes(s.free_psram);
      const swEl = $("st-switch");
      if (swEl) {
        swEl.textContent = s.switch_mode ? "开启" : "关闭";
        swEl.classList.toggle("on", !!s.switch_mode);
      }
      // Device-side configuration changes (e.g. factory reset or another
      // client) are detected via config_rev; reload unless the user has
      // unsaved edits.
      if (lastConfigRev === null) {
        lastConfigRev = s.config_rev;
      } else if (s.config_rev !== lastConfigRev) {
        lastConfigRev = s.config_rev;
        if (!dirty) loadKeymap(true);
      }
      if (s.version) {
        $("hdr-version").textContent = "固件 v" + s.version + (s.build ? " (" + s.build + ")" : "");
      }
    } catch (e) { /* ignore */ }
  }

  async function refreshTelemetry() {
    try {
      const t = await dev.telemetry();
      const pk = PHYSICAL_KEYS.find((k) => k.vk === t.pressed_vk);
      const nameEl = $("live-key-name");
      nameEl.textContent = pk ? pk.name : "—";
      nameEl.classList.toggle("active", !!pk);

      // Highlight the pressed key on the remote image (keymap tab).
      document.querySelectorAll(".remote .pressed").forEach((el) => el.classList.remove("pressed"));
      if (pk) {
        const el = document.querySelector(`.remote [data-vk="${pk.vk}"]`);
        if (el) el.classList.add("pressed");
      }

      const layer = getLayer(t.active_layer ?? 0);
      $("live-layer").textContent = layer ? configLabel(layer) : ("配置" + (t.active_layer ?? 0));
      const at = t.action_type ?? 0;
      $("live-action").textContent = ACTION[at] || ("类型" + at);
      $("live-codes").textContent = formatTelemetryCodes(t);
      $("live-duration").textContent = (t.duration_ms || 0) + " ms";
      $("telemetry").textContent = JSON.stringify(t, null, 2);
    } catch (e) { /* ignore */ }
  }

  function formatTelemetryCodes(t) {
    const mod = t.modifier || 0, key = t.key_code || 0, cons = t.consumer_code || 0;
    const parts = [];
    if (mod) parts.push("mod 0x" + mod.toString(16));
    if (key) parts.push("key 0x" + key.toString(16));
    if (cons) parts.push("cons 0x" + cons.toString(16));
    return parts.length ? parts.join(" ") : "—";
  }

  async function refreshBleInfo() {
    try {
      $("ble-info").textContent = JSON.stringify(await dev.bleInfo(), null, 2);
    } catch (e) { /* ignore */ }
  }

  async function scanBle() {
    const tbody = $("ble-table").querySelector("tbody");
    tbody.innerHTML = "<tr><td colspan='4'>扫描中...</td></tr>";
    try {
      const res = await dev.bleScan();
      const devices = res.devices || [];
      tbody.innerHTML = "";
      if (!devices.length) {
        tbody.innerHTML = "<tr><td colspan='4'>未发现设备，请让遥控器进入配对状态后重试。</td></tr>";
        return;
      }
      for (const d of devices) {
        const tr = document.createElement("tr");
        tr.innerHTML = `<td>${escapeHtml(d.name || "Unnamed")}</td><td>${d.mac}</td><td>${d.rssi} dBm</td>`;
        const td = document.createElement("td");
        const btn = document.createElement("button");
        btn.className = "btn small primary";
        btn.textContent = "连接";
        btn.onclick = async () => {
          btn.disabled = true;
          try {
            await dev.bleConnect({ mac: d.mac, type: d.type, name: d.name });
            toast("已发起连接: " + (d.name || d.mac));
            setTimeout(refreshBleInfo, 1500);
          } catch (e) {
            toast(e.message, true);
          } finally {
            btn.disabled = false;
          }
        };
        td.appendChild(btn);
        tr.appendChild(td);
        tbody.appendChild(tr);
      }
    } catch (e) {
      tbody.innerHTML = `<tr><td colspan='4'>${escapeHtml(e.message)}</td></tr>`;
    }
  }

  async function refreshLogs() {
    try {
      const res = await dev.getLogs();
      $("log-view").textContent = (res.logs || []).join("\n");
      $("log-view").scrollTop = $("log-view").scrollHeight;
    } catch (e) { /* ignore */ }
  }

  /* ------------------------- keymap ------------------------- */

  async function loadKeymap(preserveLayer) {
    try {
      keymap = await dev.getKeymap();
      deviceActiveLayer = keymap.active_layer || 0;
      if (!preserveLayer || !(keymap.layers || []).some((l) => l.id === activeLayer)) {
        activeLayer = deviceActiveLayer;
      }
      dirty = false;
      updateDirtyIndicator();
      renderLayerTabs();
      renderKeymapGrid();
      renderSwitchMap();
      $("raw-json").value = JSON.stringify(keymap, null, 2);
    } catch (e) {
      toast("读取按键配置失败: " + e.message, true);
    }
  }

  async function saveKeymap() {
    if (savingKeymap) return;
    savingKeymap = true;
    const btn = $("btn-keymap-save");
    const oldText = btn.textContent;
    btn.disabled = true;
    btn.textContent = "保存中...";
    try {
      const res = await dev.saveKeymap(keymap);
      if (res && res.error) {
        toast("保存失败: " + res.error, true);
        return;
      }
      toast("按键配置已保存");
      dirty = false;
      if (dev.isConnected()) {
        try { await dev.setLayer(activeLayer); } catch (e) { /* ignore */ }
      }
      await loadKeymap(true);
      lastConfigRev = null; // avoid a redundant reload on the next status poll
    } catch (e) {
      console.error(e);
      toast("保存失败: " + e.message, true);
    } finally {
      savingKeymap = false;
      btn.disabled = false;
      btn.textContent = oldText;
      updateDirtyIndicator();
    }
  }

  async function resetKeymap() {
    if (!confirm("确定恢复出厂按键配置？")) return;
    try {
      await dev.resetKeymap();
      toast("已恢复出厂配置");
      await loadKeymap();
      lastConfigRev = null;
    } catch (e) {
      toast(e.message, true);
    }
  }

  function getLayer(idx) {
    return keymap && keymap.layers ? keymap.layers.find((l) => l.id === idx) : null;
  }
  // Display label for a configuration slot. Normalizes legacy "层N" / "模式N"
  // names to "配置N" and strips any stray leading/trailing whitespace.
  function configLabel(layer) {
    const raw = String((layer && layer.name) || "").trim();
    if (!raw) return "配置" + (layer ? layer.id : 0);
    return raw
      .replace(/默认层/g, "默认配置")
      .replace(/默认模式/g, "默认配置")
      .replace(/^层\s*(\d+)$/, "配置$1")
      .replace(/^模式\s*(\d+)$/, "配置$1");
  }
  // LED indicator colour for a configuration slot, mirroring the firmware's
  // per-layer led_color (reported as "0xRRGGBB"). Falls back to green.
  function layerColor(layer) {
    const raw = layer ? layer.color : null;
    if (raw == null || raw === "") return "#30d158";
    let v = typeof raw === "string" ? parseInt(raw.replace(/^0x/i, ""), 16) : Number(raw);
    if (!Number.isFinite(v) || v === 0) return "#30d158";
    return "#" + (v & 0xffffff).toString(16).padStart(6, "0");
  }
  function getBinding(layer, vk) {
    if (!layer || !layer.bindings) return null;
    return layer.bindings.find((b) => b.source_vk === vk) || null;
  }
  function ensureBinding(layer, vk) {
    if (!layer.bindings) layer.bindings = [];
    let b = getBinding(layer, vk);
    if (!b) { b = { source_vk: vk }; layer.bindings.push(b); }
    return b;
  }

  function renderLayerTabs() {
    const host = $("layer-tabs");
    host.innerHTML = "";
    (keymap.layers || []).forEach((layer) => {
      const btn = document.createElement("button");
      let cls = "layer-tab";
      if (layer.id === activeLayer) cls += " active";
      if (layer.id === deviceActiveLayer) cls += " device-active";
      btn.className = cls;
      btn.textContent = configLabel(layer);
      const color = layerColor(layer);
      btn.style.setProperty("--layer-color", color);
      btn.title = (layer.id === deviceActiveLayer ? "设备当前生效的配置" : "点击切换到该配置") +
        " · 指示灯 " + color.toUpperCase();
      btn.onclick = () => {
        activeLayer = layer.id;
        renderLayerTabs();
        renderKeymapGrid();
        // Selecting a configuration also makes it active on the device.
        if (dev.isConnected()) {
          dev.setLayer(layer.id).then(() => {
            deviceActiveLayer = layer.id;
            renderLayerTabs();
            refreshStatus();
          }).catch(() => {});
        }
      };
      host.appendChild(btn);
    });
  }

  function actionSummary(b, prefix) {
    const type = b["has_" + prefix] ? (b[prefix + "_type"] ?? 0) : 0;
    if (!type) return null;
    let text = ACTION[type] || ("类型" + type);
    if (type === 1 || type === 2) {
      const mods = MOD_BITS.filter(([bit]) => (b[prefix + "_mod"] || 0) & bit).map(([, n]) => n);
      if (b[prefix + "_key"]) mods.push(Keymap.hidName(b[prefix + "_key"]));
      return mods.length ? `${text} (${mods.join(" + ")})` : text;
    }
    if (type === 4) return `${text} (0x${(b[prefix + "_cons"] || 0).toString(16)})`;
    if (type === 7) return `${text} (0x${(b[prefix + "_mod"] || 0).toString(16)}, 0x${(b[prefix + "_key"] || 0).toString(16)})`;
    if (type === 9) return `${text} → 配置 ${b[prefix + "_layer"] ?? 0}`;
    if (type === 11 || type === 12 || type === 13) {
      const btn = MOUSE_BUTTONS.find(([v]) => v === (b[prefix + "_key"] || 0));
      return `${text} (${btn ? btn[1] : "0x" + (b[prefix + "_key"] || 0).toString(16)})`;
    }
    if (type === 14) {
      const dx = b[prefix + "_dx"] || 0, dy = b[prefix + "_dy"] || 0;
      const dir = dx < 0 ? "左" : dx > 0 ? "右" : dy < 0 ? "上" : dy > 0 ? "下" : "—";
      const spd = Math.max(Math.abs(dx), Math.abs(dy));
      return `${text} (${dir}${spd ? " 速度" + spd : ""})`;
    }
    if (type === 15) return `${text} (${b[prefix + "_wheel"] || 0})`;
    if (type === 17 || type === 18) {
      const name = gamepadName(b[prefix + "_key"] || 0);
      const val = b[prefix + "_cons"] || 0;
      return val ? `${text} (${name} ${val})` : `${text} (${name})`;
    }
    return text;
  }

  function pkOf(vk) {
    return PHYSICAL_KEYS.find((k) => k.vk === vk);
  }
  function keyAction(layer, vk) {
    const b = getBinding(layer, vk);
    if (!b) return "未配置";
    const parts = [];
    const c = actionSummary(b, "click");
    const l = actionSummary(b, "long");
    const d = actionSummary(b, "double");
    if (c) parts.push(c);
    if (l) parts.push("长按 " + l);
    if (d) parts.push("双击 " + d);
    if (b.has_repeat) parts.push("连发");
    return parts.length ? parts.join(" · ") : "未配置";
  }
  function bindBtn(btn, layer, vk) {
    const pk = pkOf(vk);
    // The active remote profile may not expose every key of the RC003 layout
    // (multi-remote support): hide the ones it lacks instead of failing.
    if (!pk) { btn.disabled = true; btn.classList.add("hidden"); return btn; }
    btn.dataset.vk = String(vk);
    const show = () => {
      const el = $("remote-info");
      if (el) el.innerHTML = `<b>${pk.name}</b> · ${keyAction(layer, vk)}`;
    };
    const reset = () => {
      const el = $("remote-info");
      if (el) el.textContent = "将鼠标移到按键上查看映射";
    };
    btn.title = `${pk.name}：${keyAction(layer, vk)}`;
    btn.onclick = () => openEditor(pk);
    btn.addEventListener("mouseenter", show);
    btn.addEventListener("focus", show);
    btn.addEventListener("mouseleave", reset);
    btn.addEventListener("blur", reset);
    return btn;
  }

  function renderKeymapGrid() {
    const host = $("keymap-grid");
    host.innerHTML = "";
    const layer = getLayer(activeLayer);
    if (!layer) return;
    host.className = "remote";

    // Top: power (left) / voice (right)
    const top = document.createElement("div");
    top.className = "remote-top";
    [[0x66, ICON.power], [0x04, ICON.voice]].forEach(([vk, icon]) => {
      const btn = document.createElement("button");
      btn.type = "button";
      btn.className = "utility";
      btn.innerHTML = icon;
      top.appendChild(bindBtn(btn, layer, vk));
    });
    host.appendChild(top);

    // D-pad: four quarter-ring direction keys around a filled center OK
    const dpad = document.createElement("div");
    dpad.className = "dpad";
    [
      ["up", 0x52, DPAD_ICON.up],
      ["right", 0x4f, DPAD_ICON.right],
      ["down", 0x51, DPAD_ICON.down],
      ["left", 0x50, DPAD_ICON.left],
    ].forEach(([cls, vk, icon]) => {
      const btn = document.createElement("button");
      btn.type = "button";
      btn.className = "arc " + cls;
      btn.innerHTML = icon;
      dpad.appendChild(bindBtn(btn, layer, vk));
    });
    const ok = document.createElement("button");
    ok.type = "button";
    ok.className = "dp ok";
    dpad.appendChild(bindBtn(ok, layer, 0x28));
    host.appendChild(dpad);

    // Controls: back | volume (2 rows) / home / menu | TV
    const controls = document.createElement("div");
    controls.className = "remote-controls";
    const round = (vk, cls, inner) => {
      const btn = document.createElement("button");
      btn.type = "button";
      btn.className = cls;
      btn.innerHTML = inner;
      return bindBtn(btn, layer, vk);
    };
    const volume = document.createElement("div");
    volume.className = "volume";
    [["＋", 0x80], ["−", 0x81]].forEach(([glyph, vk]) => {
      const btn = document.createElement("button");
      btn.type = "button";
      btn.innerHTML = glyph;
      volume.appendChild(bindBtn(btn, layer, vk));
    });
    controls.append(
      round(0xf1, "round back", ICON.back),
      volume,
      round(0x24, "round home", ICON.home),
      round(0x5d, "round menu", ICON.menu),
      round(0xc0, "round tv", "<span>TV</span>")
    );
    host.appendChild(controls);
  }

  /* ------------------------- switch map ------------------------- */

  // Confirm key is locked to the default configuration by the firmware; only
  // the four directions can be remapped to a configuration.
  const SWITCH_LOCKED_KEY = 0x28;
  const SWITCH_EDITABLE_KEYS = [0x52, 0x4f, 0x51, 0x50]; // 上/右/下/左

  function switchDot(color) {
    return `<span class="switch-dot" style="background:${color || "#c7c7cc"}"></span>`;
  }

  function switchOptions() {
    const opts = [{ v: -1, t: "不参与切换", color: null }];
    (keymap.layers || []).forEach((l) => opts.push({ v: l.id, t: configLabel(l), color: layerColor(l) }));
    return opts;
  }

  // Custom dropdown that shows each configuration's LED colour as a dot.
  function makeSwitchPicker(current, disabled, onChange) {
    const opts = switchOptions();
    const sel = opts.find((o) => o.v === current) || opts[0];
    const wrap = document.createElement("div");
    wrap.className = "switch-picker" + (disabled ? " disabled" : "");

    const btn = document.createElement("button");
    btn.type = "button";
    btn.className = "switch-picker-btn";
    btn.disabled = !!disabled;
    btn.innerHTML = switchDot(sel.color) +
      `<span class="switch-picker-name">${escapeHtml(sel.t)}</span>` +
      `<span class="switch-caret">▾</span>`;
    wrap.appendChild(btn);
    if (disabled) return wrap;

    const menu = document.createElement("div");
    menu.className = "switch-picker-menu hidden";
    opts.forEach((o) => {
      const item = document.createElement("button");
      item.type = "button";
      item.className = "switch-picker-item" + (o.v === current ? " active" : "");
      item.innerHTML = switchDot(o.color) + `<span>${escapeHtml(o.t)}</span>`;
      item.onclick = (e) => {
        e.stopPropagation();
        menu.classList.add("hidden");
        if (o.v !== current) onChange(o.v);
      };
      menu.appendChild(item);
    });
    btn.onclick = (e) => {
      e.stopPropagation();
      const wasOpen = !menu.classList.contains("hidden");
      document.querySelectorAll(".switch-picker-menu").forEach((m) => m.classList.add("hidden"));
      if (!wasOpen) menu.classList.remove("hidden");
    };
    wrap.appendChild(menu);
    return wrap;
  }

  function renderSwitchMap() {
    const host = $("switch-map");
    if (!host) return;
    if (!keymap || !Array.isArray(keymap.layers)) {
      host.innerHTML = "<p class='hint'>请先连接设备并读取按键配置。</p>";
      return;
    }
    // The confirm key is locked by the firmware; drop any stale entry.
    MiRC003.Keymap.removeSwitchTarget(keymap, SWITCH_LOCKED_KEY);
    host.innerHTML = "";

    const addRow = (vk, disabled, current) => {
      const pk = pkOf(vk);
      const row = document.createElement("div");
      row.className = "switch-row" + (disabled ? " locked" : "");
      const label = document.createElement("span");
      label.className = "switch-key";
      label.textContent = pk ? pk.name : ("0x" + vk.toString(16));
      row.appendChild(label);
      row.appendChild(makeSwitchPicker(current, disabled, (val) => {
        MiRC003.Keymap.setSwitchTarget(keymap, vk, val);
        dirty = true;
        updateDirtyIndicator();
        toast("已修改，点击「保存到设备」生效");
        renderSwitchMap();
      }));
      host.appendChild(row);
    };

    addRow(SWITCH_LOCKED_KEY, true, 0);
    SWITCH_EDITABLE_KEYS.forEach((vk) =>
      addRow(vk, false, MiRC003.Keymap.getSwitchTarget(keymap, vk)));
  }

  /* ------------------------- action editor ------------------------- */

  let editingKey = null;
  let editingGesture = 0;   // 0=click 1=long 2=double 3=repeat

  // One-click presets applied to the currently selected gesture.
  const PRESETS = [
    { g: "键盘", items: [
      { label: "Enter", type: 1, key: 0x28 },
      { label: "Esc", type: 1, key: 0x29 },
      { label: "Tab", type: 1, key: 0x2b },
      { label: "空格", type: 1, key: 0x2c },
      { label: "↑", type: 1, key: 0x52 },
      { label: "↓", type: 1, key: 0x51 },
      { label: "←", type: 1, key: 0x50 },
      { label: "→", type: 1, key: 0x4f },
      { label: "Win+D", type: 1, key: 0x07, mod: 0x08 },
    ]},
    { g: "多媒体", items: [
      { label: "返回", type: 4, cons: 0x224 },
      { label: "主页", type: 4, cons: 0x223 },
      { label: "音量+", type: 4, cons: 0xe9 },
      { label: "音量-", type: 4, cons: 0xea },
      { label: "静音", type: 4, cons: 0xe2 },
      { label: "播放/暂停", type: 4, cons: 0xcd },
      { label: "上一曲", type: 4, cons: 0xb6 },
      { label: "下一曲", type: 4, cons: 0xb5 },
    ]},
    { g: "鼠标", items: [
      { label: "左键", type: 11, mouseBtn: 1 },
      { label: "右键", type: 11, mouseBtn: 2 },
      { label: "中键", type: 11, mouseBtn: 4 },
      { label: "移动↑", type: 14, dir: "up", speed: 8 },
      { label: "移动↓", type: 14, dir: "down", speed: 8 },
      { label: "移动←", type: 14, dir: "left", speed: 8 },
      { label: "移动→", type: 14, dir: "right", speed: 8 },
      { label: "滚轮↑", type: 15, wheel: 3 },
      { label: "滚轮↓", type: 15, wheel: -3 },
    ]},
    { g: "手柄", items: [
      { label: "A", type: 17, gp: 1 },
      { label: "B", type: 17, gp: 2 },
      { label: "X", type: 17, gp: 3 },
      { label: "Y", type: 17, gp: 4 },
      { label: "LB", type: 17, gp: 5 },
      { label: "RB", type: 17, gp: 6 },
      { label: "LT", type: 17, gp: 7 },
      { label: "RT", type: 17, gp: 8 },
      { label: "Select", type: 17, gp: 9 },
      { label: "Start", type: 17, gp: 10 },
      { label: "Xbox", type: 17, gp: 11 },
      { label: "方向键 上", type: 17, gp: 15 },
      { label: "方向键 下", type: 17, gp: 16 },
      { label: "方向键 左", type: 17, gp: 17 },
      { label: "方向键 右", type: 17, gp: 18 },
    ]},
    { g: "配置", items: [
      { label: "进入配置切换模式", type: 16 },
    ]},
  ];

  // Common PC voice-shortcut presets for the voice action (type 7).
  // Each entry is a modifier mask + HID key sent while the voice key is held.
  const VOICE_SHORTCUTS = [
    { label: "Windows 语音输入 (Win+H)", mod: 0x08, key: 0x0b },
    { label: "微信语音 (右Alt+,)", mod: 0x40, key: 0x36 },
    { label: "系统听写 (Win+;)", mod: 0x08, key: 0x33 },
  ];

  function setGesture(idx) {
    editingGesture = idx;
    document.querySelectorAll("#gesture-tabs .gesture-tab").forEach((t) => {
      t.classList.toggle("active", parseInt(t.dataset.gesture, 10) === idx);
    });
    document.querySelectorAll("#modal-body .action-block").forEach((blk, i) => {
      blk.classList.toggle("hidden", i !== idx);
    });
    const bar = $("preset-bar");
    if (bar) bar.classList.toggle("hidden", idx === 3);
  }

  function renderPresets() {
    const host = $("preset-bar");
    if (!host) return;
    host.innerHTML = PRESETS.map((grp) =>
      `<div class="preset-group"><span class="preset-label">${grp.g}</span>` +
      grp.items.map((p, i) => `<button type="button" class="preset" data-g="${grp.g}" data-i="${i}">${p.label}</button>`).join("") +
      `</div>`
    ).join("");
    host.querySelectorAll(".preset").forEach((btn) => {
      const grp = PRESETS.find((g) => g.g === btn.dataset.g);
      const p = grp && grp.items[parseInt(btn.dataset.i, 10)];
      if (p) btn.onclick = () => applyPreset(p);
    });
  }

  function applyPreset(p) {
    const block = document.querySelectorAll("#modal-body .action-block")[editingGesture];
    if (!block) return;
    const set = (sel, val) => { const el = block.querySelector(sel); if (el) el.value = String(val); };
    set(".f-type", p.type);
    if (p.key != null) { setPickerKey(block, "f-key", p.key); }
    if (p.cons != null) { setPickerKey(block, "f-cons", p.cons); }
    if (p.mouseBtn != null) set(".f-mousebtn", p.mouseBtn);
    if (p.gp != null) set(".f-gp-control", p.gp);
    if (p.gpValue != null) set(".f-gp-value", p.gpValue);
    if (p.dir != null) { set(".f-move-dir", p.dir); set(".f-move-speed", p.speed ?? 8); }
    if (p.wheel != null) {
      set(".f-wheel-dir", p.wheel < 0 ? "down" : "up");
      set(".f-wheel-amount", Math.abs(p.wheel) || 3);
    }
    block.querySelectorAll(".f-mod").forEach((c) => {
      c.checked = p.mod != null && (parseInt(c.value, 10) & p.mod) !== 0;
    });
    if (editingGesture !== 0) {
      const has = block.querySelector(".f-has");
      if (has) has.checked = true;
    }
    refreshFieldVisibility();
  }

  // Update every representation of the selected HID key inside one action
  // block: the hidden <select>, the visual keyboard highlight, the extended
  // keys dropdown and the "当前" label.
  function selectKeyInBlock(block, val) {
    const hidden = block.querySelector("select.f-key");
    if (hidden) hidden.value = String(val);

    const kb = block.querySelector(".kb-inline");
    if (kb) kb.querySelectorAll("[data-val]").forEach((b) => b.classList.toggle("active", Number(b.dataset.val) === val));

    const ext = block.querySelector("select.f-key-ext");
    if (ext) {
      const has = Array.from(ext.options).some((o) => o.value !== "" && Number(o.value) === val);
      ext.value = has ? String(val) : "";
    }

    const nameLabel = block.querySelector(".kb-selected-name");
    if (nameLabel) updateKeyLabel(block);
  }

  // "当前" label shows the key name, or the checked modifier names when no
  // plain key is selected (a modifier-only action).
  function updateKeyLabel(block) {
    if (!block) return;
    const label = block.querySelector(".kb-selected-name");
    if (!label) return;
    const val = parseInt(block.querySelector("select.f-key")?.value || "0", 10);
    if (val) { label.textContent = Keymap.hidName(val); return; }
    const mods = [];
    block.querySelectorAll(".f-mod").forEach((c) => {
      if (c.checked) mods.push(MOD_NAMES[parseInt(c.value, 10)]);
    });
    label.textContent = mods.length ? mods.join(" + ") : Keymap.hidName(0);
  }

  function setPickerKey(block, fieldCls, val) {
    if (fieldCls === "f-key") { selectKeyInBlock(block, val); return; }
    const sel = block.querySelector("select." + fieldCls);
    if (sel) sel.value = String(val);
  }

  // Action types grouped by function for the multi-level (optgroup) menu.
  // Mouse-button release (13) is internal-only; layer switching (9) was
  // replaced by the modal switch mode (16).
  const ACTION_GROUPS = [
    { label: "",      items: [[0, "无"]] },
    { label: "键盘",   items: [[1, "单击"], [2, "按住"]] },
    { label: "多媒体", items: [[4, "单击"], [5, "按住"]] },
    { label: "鼠标",   items: [[11, "按键-单击"], [12, "按键-按住"], [14, "移动"], [15, "滚轮"]] },
    { label: "游戏手柄", items: [[17, "单击"], [18, "按住"]] },
    { label: "语音",   items: [[7, "按住说话"]] },
    { label: "系统",   items: [[16, "进入配置切换模式"], [10, "穿透继承"]] },
  ];

  // "按住" (hold) action types: keep the key pressed while the physical key is
  // held. Mutually exclusive with the "长按" gesture.
  const HOLD_TYPES = new Set([2, 5, 12, 18]);

  function actionTypeOptions(selected, allowHold = true) {
    return ACTION_GROUPS.map((grp) => {
      const opts = grp.items
        .filter(([t]) => allowHold || !HOLD_TYPES.has(t))
        .map(([t, name]) =>
          `<option value="${t}" ${t === selected ? "selected" : ""}>${name}</option>`).join("");
      return opts ? (grp.label ? `<optgroup label="${grp.label}">${opts}</optgroup>` : opts) : "";
    }).join("");
  }

  // Gamepad controls that carry an analog value (0-255): shoulder/trigger and
  // stick directions. Plain buttons (A/B/X/Y/Select/Start/Xbox/DPad/L3/R3)
  // have no magnitude.
  const GAMEPAD_ANALOG = new Set([5, 6, 7, 8, 19, 20, 21, 22, 23, 24, 25, 26]);

  // ---- Visual keyboard layout for HID key picker ----
  // Full-size 104-key ANSI layout drawn on a 92-column grid (4 columns per
  // 1u key, 23u wide). Keys use their real widths (Tab 1.5u, Caps 1.75u,
  // Enter 2.25u, L/R Shift 2.25u/2.75u, Space 6.25u, bottom modifiers 1.25u)
  // so positions line up with a physical keyboard. The numpad "+" and "Enter"
  // are 1u wide but span two rows.
  // Tokens: [hidCode, label, widthU?, heightRows?] or a gap number in units.
  const KB_UNITS = 92;
  const KB_LAYOUT = [
    // Function row | PrtSc / ScrLk / Pause above the nav cluster
    [
      [0x29, "Esc"], 1,
      [0x3a, "F1"], [0x3b, "F2"], [0x3c, "F3"], [0x3d, "F4"], 0.5,
      [0x3e, "F5"], [0x3f, "F6"], [0x40, "F7"], [0x41, "F8"], 0.5,
      [0x42, "F9"], [0x43, "F10"], [0x44, "F11"], [0x45, "F12"], 0.5,
      [0x46, "PrtSc"], [0x47, "ScrLk"], [0x48, "Pause"], 4.5,
    ],
    // Number row | Ins / Home / PgUp | NumLk / * -
    [
      [0x35, "` ~"], [0x1e, "1 !"], [0x1f, "2 @"], [0x20, "3 #"], [0x21, "4 $"],
      [0x22, "5 %"], [0x23, "6 ^"], [0x24, "7 &"], [0x25, "8 *"], [0x26, "9 ("],
      [0x27, "0 )"], [0x2d, "- _"], [0x2e, "= +"], [0x2a, "Bksp", 2],
      0.5,
      [0x49, "Ins"], [0x4a, "Home"], [0x4b, "PgUp"], 0.5,
      [0x53, "NumLk"], [0x54, "/"], [0x55, "*"], [0x56, "-"],
    ],
    // Tab row | Del / End / PgDn | numpad 7 8 9 +
    [
      [0x2b, "Tab", 1.5], [0x14, "Q"], [0x1a, "W"], [0x08, "E"], [0x15, "R"],
      [0x17, "T"], [0x1c, "Y"], [0x18, "U"], [0x0c, "I"], [0x12, "O"],
      [0x13, "P"], [0x2f, "[ {"], [0x30, "] }"], [0x31, "\\ |", 1.5],
      0.5,
      [0x4c, "Del"], [0x4d, "End"], [0x4e, "PgDn"], 0.5,
      [0x5f, "7"], [0x60, "8"], [0x61, "9"], [0x57, "+", 1, 2],
    ],
    // Caps row | (empty nav column) | numpad 4 5 6 (+ continues)
    [
      [0x39, "Caps", 1.75], [0x04, "A"], [0x16, "S"], [0x07, "D"], [0x09, "F"],
      [0x0a, "G"], [0x0b, "H"], [0x0d, "J"], [0x0e, "K"], [0x0f, "L"],
      [0x33, '; :'], [0x34, "' \""], [0x28, "Enter", 2.25],
      4,
      [0x5c, "4"], [0x5d, "5"], [0x5e, "6"], 1,
    ],
    // Shift row | ↑ | numpad 1 2 3 Enter
    [
      [0xe1, "LShift", 2.25], [0x1d, "Z"], [0x1b, "X"], [0x06, "C"], [0x19, "V"],
      [0x05, "B"], [0x11, "N"], [0x10, "M"], [0x36, ", <"], [0x37, ". >"],
      [0x38, "/ ?"], [0xe5, "RShift", 2.75],
      1.5, [0x52, "↑"], 1.5,
      [0x59, "1"], [0x5a, "2"], [0x5b, "3"], [0x58, "Enter", 1, 2],
    ],
    // Space row | ← ↓ → | numpad 0 .
    [
      [0xe0, "LCtrl", 1.25], [0xe3, "LWin", 1.25], [0xe2, "LAlt", 1.25],
      [0x2c, "Space", 6.25],
      [0xe6, "RAlt", 1.25], [0xe7, "RWin", 1.25], [0x65, "Menu", 1.25],
      [0xe4, "RCtrl", 1.25],
      0.5,
      [0x50, "←"], [0x51, "↓"], [0x4f, "→"],
      0.5,
      [0x62, "0", 2], [0x63, "."], 1,
    ],
  ];

  // Legend markup for one keycap. Symbol keys (label "1 !") show the shifted
  // glyph above the base glyph, like a real keycap; others render one label.
  function kbLabelHtml(label) {
    const parts = String(label).split(" ");
    if (parts.length === 2) {
      return `<span class="kb-leg kb-leg-shift">${escapeHtml(parts[1])}</span>` +
        `<span class="kb-leg kb-leg-base">${escapeHtml(parts[0])}</span>`;
    }
    const long = label.length > 3 ? " long" : "";
    return `<span class="kb-key-label${long}">${escapeHtml(label)}</span>`;
  }

  function renderVisualKB(selected) {
    // Tall numpad keys ("+" row2→3, "Enter" row4→5) omit their lower half
    // from the next row's token list; grid-row span handles the height.
    let html = '<div class="kb-full"><div class="kb-rows">';
    KB_LAYOUT.forEach((row, ri) => {
      let col = 1;
      row.forEach((tok) => {
        if (typeof tok === "number") { col += Math.round(tok * 4); return; }
        const [val, label, w, h] = tok;
        const width = Math.round((w || 1) * 4);
        const height = h || 1;
        const cls = "kb-key" + (val === selected ? " active" : "");
        html += `<button type="button" class="${cls}" data-val="${val}" style="grid-row:${ri + 1}/span ${height};grid-column:${col}/span ${width}">` +
          kbLabelHtml(label) + `</button>`;
        col += width;
      });
    });
    html += "</div></div>";
    return html;
  }

  function renderActionFields(prefix, b) {
    const has = b["has_" + prefix] ? "checked" : "";
    const type = b[prefix + "_type"] ?? 0;
    const mod = b[prefix + "_mod"] ?? 0;
    const key = b[prefix + "_key"] ?? 0;
    const cons = b[prefix + "_cons"] ?? 0;
    const wheel = b[prefix + "_wheel"] ?? 0;
    const mdx = b[prefix + "_dx"] ?? 0;
    const mdy = b[prefix + "_dy"] ?? 0;
    let moveDir = "up";
    if (mdx < 0) moveDir = "left";
    else if (mdx > 0) moveDir = "right";
    else if (mdy < 0) moveDir = "up";
    else if (mdy > 0) moveDir = "down";
    const moveSpeed = Math.max(Math.abs(mdx), Math.abs(mdy)) || 8;
    const ms = b[prefix + "_ms"] ?? (prefix === "long" ? 600 : 250);
    // Wheel is stored as a signed delta but edited as direction + amount.
    const wheelDir = wheel < 0 ? "down" : "up";
    const wheelAmount = Math.abs(wheel) || 3;
    // Voice shortcut: a preset is selected, otherwise fall back to "custom"
    // (which reveals the modifier + visual keyboard pickers).
    const voicePresetIdx = VOICE_SHORTCUTS.findIndex((s) => s.mod === mod && s.key === key);
    const voiceOptions = VOICE_SHORTCUTS.map((s, i) =>
      `<option value="${i}" ${i === voicePresetIdx ? "selected" : ""}>${s.label}</option>`).join("") +
      `<option value="custom" ${voicePresetIdx < 0 ? "selected" : ""}>自定义…</option>`;

    // Hidden <select> holds the actual key value; the visual keyboard and the
    // extended dropdown both update it. It carries the full usage list so an
    // extended key (e.g. F13) or a modifier key survives a read-back.
    const modifierOptions = `<optgroup label="修饰键">` +
      MOD_BITS.map(([, name], i) => {
        const usage = 0xe0 + i;
        return `<option value="${usage}" ${usage === key ? "selected" : ""}>${name}</option>`;
      }).join("") + `</optgroup>`;
    const usageOptions = `<option value="0" ${key === 0 ? "selected" : ""}>— 未选择 —</option>` +
      modifierOptions +
      HID_GROUPS.concat(HID_EXTRA_GROUPS).map(([g, items]) =>
      `<optgroup label="${g}">` + items.map(([v, n]) =>
        `<option value="${v}" ${v === key ? "selected" : ""}>${n}</option>`).join("") + `</optgroup>`
    ).join("");
    const consumerOptions = CONSUMER_GROUPS.map(([g, items]) =>
      `<optgroup label="${g}">` + items.map(([v, n]) =>
        `<option value="${v}" ${v === cons ? "selected" : ""}>${n}</option>`).join("") + `</optgroup>`
    ).join("");
    const mouseButtonOptions = MOUSE_BUTTONS.map(([v, n]) =>
      `<option value="${v}" ${v === key ? "selected" : ""}>${n}</option>`).join("");
    // Gamepad control groups (used when the action type is a gamepad one).
    const gpControl = b[prefix + "_key"] ?? 0;
    const gpValue = b[prefix + "_cons"] ?? 0;
    const gpGroupOrder = [];
    const gpGroupMap = {};
    GAMEPAD_CONTROLS.forEach((c) => {
      if (!gpGroupMap[c.group]) { gpGroupMap[c.group] = []; gpGroupOrder.push(c.group); }
      gpGroupMap[c.group].push(c);
    });
    const gamepadOptions = gpGroupOrder.map((g) =>
      `<optgroup label="${g}">` + gpGroupMap[g].map((c) =>
        `<option value="${c.id}" ${c.id === gpControl ? "selected" : ""}>${c.name}</option>`).join("") + `</optgroup>`
    ).join("");
    const modChecks = MOD_BITS.map(([bit, name]) =>
      `<label class="check"><input type="checkbox" class="f-mod" value="${bit}" ${(mod & bit) ? "checked" : ""}/>${name}</label>`
    ).join("");
    // Extra keys that are not on the visual keyboard (F13+, keypad, IME, ...).
    const extraOptions = HID_EXTRA_GROUPS.map(([g, items]) =>
      `<optgroup label="${g}">` + items.map(([v, n]) =>
        `<option value="${v}">${n}</option>`).join("") + `</optgroup>`
    ).join("");

    const modNames = MOD_BITS.filter(([bit]) => mod & bit).map(([, n]) => n);
    const selectedKeyName = key ? Keymap.hidName(key)
      : (modNames.length ? modNames.join(" + ") : Keymap.hidName(0));

    return `
      <div class="action-block" data-prefix="${prefix}">
        <h4>${prefix === "click" ? "单击" : prefix === "long" ? "长按" : "双击"}
          ${prefix !== "click" ? `<label class="check" style="float:right"><input type="checkbox" class="f-has" ${has}/> 启用</label>` : ""}
        </h4>
        <div class="inline">
          <div class="field"><label>动作类型</label><select class="f-type">${actionTypeOptions(type, prefix !== "long" && !b.has_long)}</select></div>
          ${prefix !== "click" ? `<div class="field"><label>触发时间 (ms)</label><input type="number" class="f-ms" value="${ms}" min="50" max="3000"/></div>` : ""}
        </div>
        <div class="f-voice">
          <div class="field">
            <label>语音快捷键（按住语音键期间发送）</label>
            <select class="f-voice-shortcut">${voiceOptions}</select>
          </div>
        </div>
        <div class="f-keyboard">
          <div class="field"><label>修饰键（可多选）</label><div class="mods">${modChecks}</div></div>
          <div class="field">
            <label>按键 — 当前: <b class="kb-selected-name">${escapeHtml(selectedKeyName)}</b></label>
            <select class="f-key" style="display:none">${usageOptions}</select>
            <div class="kb-inline" data-prefix="${prefix}">${renderVisualKB(key)}</div>
          </div>
          <div class="field">
            <label>扩展按键（键盘上没有，Windows 支持）</label>
            <select class="f-key-ext">
              <option value="">— 从扩展列表选择 —</option>
              ${extraOptions}
            </select>
          </div>
        </div>
        <div class="f-consumer">
          <div class="field"><label>多媒体键</label><select class="f-cons">${consumerOptions}</select></div>
        </div>
        <div class="f-mouse">
          <div class="field"><label>鼠标按键</label><select class="f-mousebtn">${mouseButtonOptions}</select></div>
        </div>
        <div class="f-move">
          <div class="inline">
            <div class="field"><label>移动方向</label><select class="f-move-dir">
              <option value="up" ${moveDir === "up" ? "selected" : ""}>上</option>
              <option value="down" ${moveDir === "down" ? "selected" : ""}>下</option>
              <option value="left" ${moveDir === "left" ? "selected" : ""}>左</option>
              <option value="right" ${moveDir === "right" ? "selected" : ""}>右</option>
            </select></div>
            <div class="field"><label>移动速度 (1-127)</label><input type="number" class="f-move-speed" value="${moveSpeed}" min="1" max="127"/></div>
          </div>
          <p class="hint">按住按键时按此方向持续移动，松开即停；速度越大移动越快。</p>
        </div>
        <div class="f-wheel">
          <div class="inline">
            <div class="field"><label>滚轮方向</label><select class="f-wheel-dir">
              <option value="up" ${wheelDir === "up" ? "selected" : ""}>向上</option>
              <option value="down" ${wheelDir === "down" ? "selected" : ""}>向下</option>
            </select></div>
            <div class="field"><label>每次格数 (1-127)</label><input type="number" class="f-wheel-amount" value="${wheelAmount}" min="1" max="127"/></div>
          </div>
          <p class="hint">按下按键时按此方向滚动一次；格数越大滚动越多。</p>
        </div>
        <div class="f-gamepad">
          <div class="inline">
            <div class="field"><label>手柄按键</label><select class="f-gp-control">${gamepadOptions}</select></div>
            <div class="field f-gp-mag"><label>幅度 (0-255)</label><input type="number" class="f-gp-value" value="${gpValue || 255}" min="0" max="255"/></div>
          </div>
          <p class="hint">映射到虚拟 Xbox 360 手柄（XInput）：左/右摇杆为模拟量，LT/RT 为左/右扳机（模拟量 0-255），方向键即十字键，另含 A/B/X/Y、LB/RB、View/Back、Menu/Start、Xbox(Guide)、L3/R3（摇杆按下）、截屏键。摇杆与扳机可设幅度 0-255（默认 255），普通按钮无幅度；摇杆方向建议用「手柄-按住」（按住期间持续偏转），按钮用「手柄-单击」。</p>
        </div>
      </div>`;
  }

  function refreshFieldVisibility() {
    document.querySelectorAll(".action-block").forEach((block) => {
      const typeSel = block.querySelector(".f-type");
      if (!typeSel) return;
      const type = parseInt(typeSel.value, 10);
      const voiceSel = block.querySelector(".f-voice-shortcut");
      const voiceCustom = !!(voiceSel && voiceSel.value === "custom");
      // Keyboard pickers show for keyboard actions, and for a custom voice
      // shortcut (a preset voice shortcut needs no manual key editing).
      block.querySelector(".f-keyboard").style.display =
        (type === 1 || type === 2 || (type === 7 && voiceCustom)) ? "block" : "none";
      const voice = block.querySelector(".f-voice");
      if (voice) voice.style.display = (type === 7) ? "block" : "none";
      block.querySelector(".f-consumer").style.display = (type === 4) ? "block" : "none";
      block.querySelector(".f-mouse").style.display = (type === 11 || type === 12) ? "block" : "none";
      block.querySelector(".f-move").style.display = (type === 14) ? "block" : "none";
      block.querySelector(".f-wheel").style.display = (type === 15) ? "block" : "none";
      const gp = block.querySelector(".f-gamepad");
      if (gp) gp.style.display = (type === 17 || type === 18) ? "block" : "none";
      // The magnitude field only applies to analog controls (sticks, and
      // shoulder/trigger); plain buttons have no magnitude.
      const gpCtrl = block.querySelector(".f-gp-control");
      const magField = block.querySelector(".f-gp-mag");
      if (magField && gpCtrl) {
        magField.style.display = GAMEPAD_ANALOG.has(parseInt(gpCtrl.value, 10)) ? "block" : "none";
      }
    });
  }

  // Enforces the mutual exclusion between the "长按" gesture and the "按住"
  // (hold) action type: a hold already keeps the key pressed while the physical
  // key is held, so it cannot coexist with a long-press trigger.
  function updateGestureExclusion() {
    const blocks = document.querySelectorAll("#modal-body .action-block");
    if (blocks.length < 3) return;
    const clickSel = blocks[0].querySelector(".f-type");
    const longHas = blocks[1].querySelector(".f-has");
    const longType = parseInt(blocks[1].querySelector(".f-type")?.value || "0", 10);
    const longTab = document.querySelector('#gesture-tabs .gesture-tab[data-gesture="1"]');

    const clickHold = HOLD_TYPES.has(parseInt(clickSel?.value || "0", 10));
    if (clickHold) {
      if (longHas) longHas.checked = false;
      if (longTab) longTab.classList.add("disabled");
      if (editingGesture === 1) setGesture(0);
    } else if (longTab) {
      longTab.classList.remove("disabled");
    }

    if (clickSel) {
      const longOn = !!(longHas && longHas.checked && longType !== 0);
      Array.from(clickSel.querySelectorAll("option")).forEach((o) => {
        o.disabled = longOn && HOLD_TYPES.has(parseInt(o.value, 10));
      });
      if (clickSel.selectedOptions[0] && clickSel.selectedOptions[0].disabled) {
        clickSel.value = "1";
      }
    }
  }

  function readActionFields(block) {
    const type = parseInt(block.querySelector(".f-type").value, 10);
    const hasBox = block.querySelector(".f-has");
    const has = hasBox ? hasBox.checked : true;
    let mod = 0;
    block.querySelectorAll(".f-mod").forEach((c) => { if (c.checked) mod |= parseInt(c.value, 10); });
    const keyVal = parseInt(block.querySelector(".f-key")?.value || "0", 10);
    const consVal = parseInt(block.querySelector(".f-cons")?.value || "0", 10);
    const moveDir = block.querySelector(".f-move-dir")?.value || "up";
    let moveSpeed = parseInt(block.querySelector(".f-move-speed")?.value || "8", 10);
    if (!(moveSpeed >= 1)) moveSpeed = 8;
    if (moveSpeed > 127) moveSpeed = 127;
    let mdx = 0, mdy = 0;
    if (moveDir === "up") mdy = -moveSpeed;
    else if (moveDir === "down") mdy = moveSpeed;
    else if (moveDir === "left") mdx = -moveSpeed;
    else if (moveDir === "right") mdx = moveSpeed;
    const wheelDir = block.querySelector(".f-wheel-dir")?.value || "up";
    let wheelAmount = parseInt(block.querySelector(".f-wheel-amount")?.value || "3", 10);
    if (!(wheelAmount >= 1)) wheelAmount = 1;
    if (wheelAmount > 127) wheelAmount = 127;
    return {
      has,
      type,
      mod,
      key: keyVal,
      cons: consVal,
      mouseBtn: parseInt(block.querySelector(".f-mousebtn")?.value || "0", 10),
      gpControl: parseInt(block.querySelector(".f-gp-control")?.value || "0", 10),
      gpValue: parseInt(block.querySelector(".f-gp-value")?.value || "0", 10),
      dx: mdx,
      dy: mdy,
      wheel: wheelDir === "down" ? -wheelAmount : wheelAmount,
      ms: parseInt(block.querySelector(".f-ms")?.value || "0", 10),
    };
  }

  function openEditor(pk) {
    editingKey = pk;
    editingGesture = 0;
    const layer = getLayer(activeLayer);
    const b = getBinding(layer, pk.vk) || { source_vk: pk.vk };
    $("modal-title").textContent = `${pk.name} · ${configLabel(getLayer(activeLayer))}`;
    $("modal-body").innerHTML =
      `<div class="gesture-tabs" id="gesture-tabs">
         <button type="button" class="gesture-tab" data-gesture="0">单击</button>
         <button type="button" class="gesture-tab" data-gesture="1">长按</button>
         <button type="button" class="gesture-tab" data-gesture="2">双击</button>
         <button type="button" class="gesture-tab" data-gesture="3">连发</button>
       </div>
       <div class="preset-bar" id="preset-bar"></div>` +
      renderActionFields("click", b) +
      renderActionFields("long", b) +
      renderActionFields("double", b) +
      `<div class="action-block">
         <h4>连发 <label class="check" style="float:right"><input type="checkbox" class="f-has" data-prefix="repeat" ${b.has_repeat ? "checked" : ""}/> 启用</label></h4>
         <div class="inline">
           <div class="field"><label>起始延迟 (ms)</label><input type="number" id="rep-delay" value="${b.repeat_delay_ms ?? 350}"/></div>
           <div class="field"><label>连发间隔 (ms)</label><input type="number" id="rep-interval" value="${b.repeat_interval_ms ?? 70}"/></div>
         </div>
       </div>`;
    const configured = [!!b.has_click, !!b.has_long, !!b.has_double, !!b.has_repeat];
    document.querySelectorAll("#gesture-tabs .gesture-tab").forEach((t, i) => {
      if (configured[i]) t.classList.add("has-action");
      t.onclick = () => {
        if (t.classList.contains("disabled")) return;
        setGesture(parseInt(t.dataset.gesture, 10));
      };
    });
    renderPresets();
    // Wire the inline visual keyboard and the extended-keys dropdown so both
    // stay in sync with the hidden <select class="f-key">.
    document.querySelectorAll("#modal-body .kb-inline").forEach((kbEl) => {
      const block = kbEl.closest(".action-block");
      if (!block) return;
      kbEl.querySelectorAll("[data-val]").forEach((btn) => {
        btn.addEventListener("click", () => selectKeyInBlock(block, Number(btn.dataset.val)));
      });
    });
    document.querySelectorAll("#modal-body select.f-key-ext").forEach((sel) => {
      sel.addEventListener("change", () => {
        if (!sel.value) return;
        selectKeyInBlock(sel.closest(".action-block"), Number(sel.value));
      });
    });
    // Voice shortcut presets write the modifier + key fields; "custom" reveals
    // the manual keyboard picker instead.
    document.querySelectorAll("#modal-body select.f-voice-shortcut").forEach((sel) => {
      sel.addEventListener("change", () => {
        const block = sel.closest(".action-block");
        if (sel.value !== "custom") {
          const preset = VOICE_SHORTCUTS[parseInt(sel.value, 10)];
          if (preset) {
            block.querySelectorAll(".f-mod").forEach((c) => {
              c.checked = (preset.mod & parseInt(c.value, 10)) !== 0;
            });
            selectKeyInBlock(block, preset.key);
          }
        }
        refreshFieldVisibility();
      });
    });
    // Reflect the current key in every control (highlight, dropdown, label).
    document.querySelectorAll("#modal-body .f-mod").forEach((c) => {
      c.addEventListener("change", () => updateKeyLabel(c.closest(".action-block")));
    });
    document.querySelectorAll("#modal-body .action-block").forEach((block) => {
      const hidden = block.querySelector("select.f-key");
      if (hidden) selectKeyInBlock(block, parseInt(hidden.value || "0", 10));
    });
    $("modal").classList.remove("hidden");
    refreshFieldVisibility();
    setGesture(0);
    updateGestureExclusion();
  }

  function applyEditor() {
    const layer = getLayer(activeLayer);
    const b = ensureBinding(layer, editingKey.vk);
    const blocks = document.querySelectorAll("#modal-body .action-block");
    ["click", "long", "double"].forEach((prefix, i) => {
      const cfg = readActionFields(blocks[i]);
      b["has_" + prefix] = cfg.type !== 0;
      b[prefix + "_type"] = cfg.type;
      // Clear every type-specific field so stale values (e.g. a leftover key
      // code) never leak into the new action.
      delete b[prefix + "_mod"];
      delete b[prefix + "_key"];
      delete b[prefix + "_cons"];
      delete b[prefix + "_layer"];
      delete b[prefix + "_dx"];
      delete b[prefix + "_dy"];
      delete b[prefix + "_wheel"];
      if (cfg.type === 1 || cfg.type === 2 || cfg.type === 7) {
        b[prefix + "_mod"] = cfg.mod;
        b[prefix + "_key"] = cfg.key;
      } else if (cfg.type === 4) {
        b[prefix + "_cons"] = cfg.cons;
      } else if (cfg.type === 11 || cfg.type === 12 || cfg.type === 13) {
        b[prefix + "_key"] = cfg.mouseBtn;
      } else if (cfg.type === 14) {
        b[prefix + "_dx"] = cfg.dx;
        b[prefix + "_dy"] = cfg.dy;
      } else if (cfg.type === 15) {
        b[prefix + "_wheel"] = cfg.wheel;
      } else if (cfg.type === 17 || cfg.type === 18) {
        b[prefix + "_key"] = cfg.gpControl;
        b[prefix + "_cons"] = cfg.gpValue;
      }
      if (prefix !== "click") b[prefix + "_ms"] = cfg.ms;
      if (!b["has_" + prefix]) {
        delete b[prefix + "_type"];
      }
    });
    const repBlock = blocks[3];
    const repEnabled = repBlock.querySelector(".f-has").checked;
    if (repEnabled) {
      b.has_repeat = true;
      b.repeat_type = b.click_type || 4;
      b.repeat_mod = b.click_mod || 0;
      b.repeat_key = b.click_key || 0;
      b.repeat_cons = b.click_cons || 0;
      b.repeat_dx = b.click_dx || 0;
      b.repeat_dy = b.click_dy || 0;
      b.repeat_wheel = b.click_wheel || 0;
      b.repeat_delay_ms = parseInt($("rep-delay").value, 10);
      b.repeat_interval_ms = parseInt($("rep-interval").value, 10);
    } else {
      delete b.has_repeat;
      delete b.repeat_type;
      delete b.repeat_cons;
      delete b.repeat_dx;
      delete b.repeat_dy;
      delete b.repeat_wheel;
    }
    layer.bindings = layer.bindings.filter((x) => x.has_click || x.has_long || x.has_double);
    $("modal").classList.add("hidden");
    dirty = true;
    updateDirtyIndicator();
    renderKeymapGrid();
    toast("已修改，点击「保存到设备」生效");
  }

  /* ------------------------- auto refresh ------------------------- */

  function startAutoRefresh() {
    stopAutoRefresh();
    statusTimer = setInterval(refreshStatus, 1000);
    logTimer = setInterval(() => {
      if ($("log-auto").checked) refreshLogs();
    }, 3000);
    telemetryTimer = setInterval(() => {
      if ($("telemetry-live").checked) refreshTelemetry();
    }, 150);
  }
  function stopAutoRefresh() {
    clearInterval(statusTimer);
    clearInterval(logTimer);
    clearInterval(telemetryTimer);
    statusTimer = logTimer = telemetryTimer = null;
    const nameEl = $("live-key-name");
    if (nameEl) { nameEl.textContent = "—"; nameEl.classList.remove("active"); }
    document.querySelectorAll(".remote .pressed").forEach((el) => el.classList.remove("pressed"));
  }

  /* ------------------------- wiring ------------------------- */

  function initTabs() {
    document.querySelectorAll(".tab").forEach((tab) => {
      tab.onclick = () => {
        document.querySelectorAll(".tab").forEach((t) => t.classList.remove("active"));
        document.querySelectorAll(".panel").forEach((p) => p.classList.remove("active"));
        tab.classList.add("active");
        $("panel-" + tab.dataset.tab).classList.add("active");
        if (tab.dataset.tab === "logs") refreshLogs();
        if (tab.dataset.tab === "ble") refreshBleInfo();
      };
    });
  }

  function init() {
    const webuiEl = $("webui-version");
    if (webuiEl) webuiEl.textContent = "WebUI v" + WEBUI_VERSION;
    if (!navigator.usb) $("unsupported").classList.remove("hidden");
    initTabs();

    dev.on("disconnect", () => { stopAutoRefresh(); setConnected(false); toast("设备已拔出", true); });

    // Refresh immediately when the tab/window regains focus instead of waiting
    // for the next poll tick.
    const refreshOnReturn = () => {
      if (!dev.isConnected() || document.hidden) return;
      refreshStatus();
      refreshBleInfo();
      if ($("log-auto") && $("log-auto").checked) refreshLogs();
    };
    document.addEventListener("visibilitychange", refreshOnReturn);
    window.addEventListener("focus", refreshOnReturn);

    $("btn-connect").onclick = connect;
    $("btn-disconnect").onclick = disconnect;
    $("btn-keymap-refresh").onclick = () => {
      if (dirty && !confirm("有未保存的修改，确定从设备重新读取？")) return;
      loadKeymap();
    };
    $("btn-keymap-save").onclick = saveKeymap;
    $("btn-keymap-reset").onclick = resetKeymap;
    const rpSel = $("remote-profile");
    if (rpSel) rpSel.onchange = async () => {
      if (!dev.isConnected()) { toast("请先连接设备", true); return; }
      try {
        await dev.setRemoteProfile(rpSel.value);
        activeRemoteProfile = rpSel.value;
        toast("已切换遥控器型号");
        await loadRemoteInfo();
        await loadKeymap();
      } catch (e) { toast(e.message, true); }
    };
    $("btn-keymap-activate").onclick = async () => {
      try {
        await dev.setLayer(activeLayer);
        toast("已切换为配置 " + activeLayer);
        await refreshStatus();
      } catch (e) { toast(e.message, true); }
    };
    $("btn-ble-scan").onclick = scanBle;
    $("btn-ble-info").onclick = refreshBleInfo;
    $("btn-ble-unpair").onclick = async () => {
      if (!confirm("确定解除遥控器绑定？")) return;
      try { await dev.bleUnpair(); toast("已解除绑定"); refreshBleInfo(); }
      catch (e) { toast(e.message, true); }
    };
    $("btn-ble-reconnect").onclick = async () => {
      try { await dev.bleReconnect(); toast("正在重新连接..."); } catch (e) { toast(e.message, true); }
    };
    $("btn-logs-refresh").onclick = refreshLogs;
    $("btn-logs-clear").onclick = async () => {
      try { await dev.clearLogs(); refreshLogs(); } catch (e) { toast(e.message, true); }
    };
    $("btn-restart").onclick = async () => {
      if (!confirm("确定重启设备？")) return;
      try { await dev.restart(); toast("设备正在重启..."); } catch (e) { toast(e.message, true); }
    };
    $("btn-nvs-reset").onclick = async () => {
      if (!confirm("确定恢复出厂设置？将清除所有配置与绑定。")) return;
      try { await dev.factoryReset(); toast("已恢复出厂，设备重启中..."); } catch (e) { toast(e.message, true); }
    };
    $("btn-json-load").onclick = () => loadKeymap();
    $("btn-json-apply").onclick = async () => {
      try {
        keymap = JSON.parse($("raw-json").value);
        await dev.saveKeymap(keymap);
        toast("JSON 已写入设备");
        await loadKeymap();
      } catch (e) {
        toast("JSON 无效: " + e.message, true);
      }
    };

    $("modal-close").onclick = $("modal-cancel").onclick = () => $("modal").classList.add("hidden");
    $("modal-apply").onclick = applyEditor;
    const modalApplyTop = $("modal-apply-keys");
    if (modalApplyTop) modalApplyTop.onclick = applyEditor;
    $("modal-clear").onclick = () => {
      const layer = getLayer(activeLayer);
      if (layer) layer.bindings = (layer.bindings || []).filter((x) => x.source_vk !== editingKey.vk);
      $("modal").classList.add("hidden");
      dirty = true;
      updateDirtyIndicator();
      renderKeymapGrid();
      toast("已清除该按键，点击「保存到设备」生效");
    };
    document.addEventListener("change", (e) => {
      if (e.target.classList.contains("f-type")) {
        refreshFieldVisibility();
        updateGestureExclusion();
      }
      if (e.target.classList.contains("f-has")) updateGestureExclusion();
      if (e.target.classList.contains("f-gp-control")) {
        const block = e.target.closest(".action-block");
        const mag = block && block.querySelector(".f-gp-value");
        // Default the analog magnitude to full (255) when switching to a
        // control that has one and no value was set yet.
        if (mag && GAMEPAD_ANALOG.has(parseInt(e.target.value, 10)) && !(parseInt(mag.value, 10) > 0)) {
          mag.value = "255";
        }
        refreshFieldVisibility();
      }
    });
    document.addEventListener("click", () => {
      document.querySelectorAll(".switch-picker-menu").forEach((m) => m.classList.add("hidden"));
    });

    // Configuration-switch mode modal.
    let switchBackup = null;
    const closeSwitchModal = (revert) => {
      if (revert && switchBackup && keymap) keymap.switch_map = switchBackup;
      switchBackup = null;
      $("switch-modal").classList.add("hidden");
      renderSwitchMap();
    };
    const switchOpen = $("btn-switch-open");
    if (switchOpen) switchOpen.onclick = () => {
      if (!keymap) { toast("请先连接设备并读取按键配置", true); return; }
      switchBackup = JSON.parse(JSON.stringify(keymap.switch_map || []));
      renderSwitchMap();
      $("switch-modal").classList.remove("hidden");
    };
    $("switch-modal-close").onclick = $("switch-modal-cancel").onclick = () => closeSwitchModal(true);
    $("switch-modal-apply").onclick = () => {
      switchBackup = null;
      $("switch-modal").classList.add("hidden");
      dirty = true;
      updateDirtyIndicator();
      renderSwitchMap();
      toast("已修改，点击「保存到设备」生效");
    };

    setConnected(false);
  }

  document.addEventListener("DOMContentLoaded", init);
})();
