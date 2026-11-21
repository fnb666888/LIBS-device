"use strict";

const byId = (id) => document.getElementById(id);
const scenario = byId("demo-scenario");
const branch = byId("prediction-branch");
const canvas = byId("spectrum-canvas");
const context = canvas.getContext("2d");
const displayClock = new Intl.DateTimeFormat("zh-CN", { dateStyle: "short", timeStyle: "medium", hour12: false });
const displayTime = new Intl.DateTimeFormat("zh-CN", { timeStyle: "medium", hour12: false });
const displayNumber = new Intl.NumberFormat("zh-CN", { maximumFractionDigits: 3 });

const measurementGroups = [
  { id: "laser", name: "激光器参数", fields: [
    ["discharge_current_a", "放电电流", 15, 100, 1, "A", 80],
    ["pulse_width_us", "放电脉宽", 50, 300, 1, "μs", 200],
    ["frequency_hz", "放电频率", 1, 10, 1, "Hz", 1],
    ["discharge_delay_us", "放电延时", 0, 500, 1, "μs", 0],
    ["q_delay_us", "Q 延时", 2, 500, 1, "μs", 210],
    ["q_warmup_s", "Q 预热", 0, 3600, 1, "s", 0],
    ["settle_timeout_ms", "稳定等待超时", 1, 600000, 1, "ms", 20000],
    ["emission_hold_ms", "出光保持", 0, 600000, 1, "ms", 3000],
    ["min_discharges", "最小放电次数", 1, 100000, 1, "次", 1],
  ] },
  { id: "spectrometer", name: "光谱仪参数", fields: [
    ["integration_ms", "积分时间", 0.002, 60000, 0.001, "ms", "1.050"],
    ["integration_delay_ns", "积分延时", 0, 1000000, 1, "ns", 1280],
    ["capture_timeout_ms", "采集超时", 1, 600000, 1, "ms", 10000],
  ] },
];
const airGroup = { id: "air", name: "空调参数", fields: [
  ["cooling_on_c", "制冷开启", 20, 55, 0.1, "°C", 32],
  ["cooling_off_c", "制冷停止", 15, 35, 0.1, "°C", 27],
  ["heating_on_c", "加热开启", -30, 15, 0.1, "°C", 5],
  ["heating_off_c", "加热停止", 0, 23, 0.1, "°C", 10],
  ["dehumidify_on_percent", "除湿开启", 0, 100, 0.1, "%RH", 65],
  ["dehumidify_off_percent", "除湿停止", 0, 100, 0.1, "%RH", 55],
] };
const rangeFields = [
  ["empty_belt_mm", "空带基准", 1, 65534, 1, "mm", 240],
  ["material_delta_mm", "来料阈值", 1, 65534, 1, "mm", 20],
  ["valid_min_mm", "可测距离下限", 1, 65534, 1, "mm", 160],
  ["valid_max_mm", "可测距离上限", 1, 65534, 1, "mm", 230],
];
const lampColors = { red: "红灯", yellow: "黄灯", green: "绿灯", off: "全灭" };
const detectionScenes = {
  ready: { color: "green", name: "可检测" },
  standby: { color: "yellow", name: "待就绪", reason: "模拟设备正在准备，暂不能开始检测。" },
  interlock: { color: "red", name: "门禁告警", reason: "模拟门禁未闭合，检测无法启动。" },
  offline: { color: "off", name: "设备离线", reason: "模拟设备离线。请切换到设备就绪场景。" },
};
const deviceNames = {
  laser: "激光器", spectrometer: "光谱仪", indicator: "三色状态灯", rangefinder: "测距仪", air: "空调",
};
const predictionFields = ["TFe", "SiO₂", "Al₂O₃", "TiO₂", "CaO", "MgO", "MnO", "P", "S", "FeO"];
const predictionSamples = {
  oxide: [62.84, 6.29, 2.06, 0.28, 0.91, 0.58, 0.14, 0.043, 0.028, 11.47],
  sulfide: [52.41, 8.36, 3.18, 0.35, 1.16, 0.73, 0.21, 0.048, 1.52, 9.62],
};
const channels = [
  { color: "--accent", start: 185, end: 336, peaks: [[213, 17000], [239, 28200], [248, 38700], [259, 42200], [271, 23600], [296, 19000], [309, 32200]] },
  { color: "--trace-green", start: 311, end: 456, peaks: [[326, 11000], [357, 26800], [372, 30700], [383, 16200], [405, 14300], [422, 40000], [438, 21000]] },
  { color: "--trace-amber", start: 433, end: 565, peaks: [[451, 14400], [476, 19700], [492, 33700], [516, 29100], [528, 18000], [540, 25500]] },
];
const state = { running: false, frames: 0, failures: 0, samples: 0, spectrum: null, visibleSpectrum: null, scale: 64000 };

function note(id, message, tone = "") {
  const element = byId(id);
  element.textContent = message;
  element.dataset.tone = tone;
}

function readSaved(key, defaults) {
  try {
    return { ...defaults, ...JSON.parse(localStorage.getItem("libs-ui-demo:" + key)) };
  } catch {
    return { ...defaults };
  }
}

function saveLocally(key, values, feedback) {
  try {
    localStorage.setItem("libs-ui-demo:" + key, JSON.stringify(values));
    return true;
  } catch {
    note(feedback, "浏览器无法保存演示参数。", "error");
    return false;
  }
}

function setFormValues(form, values) {
  for (const [name, value] of Object.entries(values)) {
    const input = form.elements.namedItem(name);
    if (!input) continue;
    input.value = input.dataset.step ? Number(value).toFixed(stepPrecision(input)) : value;
    input.removeAttribute("aria-invalid");
  }
}

function tile([id, label, minimum, maximum, step, unit], description = "") {
  const hint = description || displayNumber.format(minimum) + " – " + displayNumber.format(maximum) + " " + unit;
  const mode = step < 1 || minimum < 0 ? "decimal" : "numeric";
  const stepButton = (direction, action, symbol) => '<button class="step" type="button" data-step-change="' +
    direction + '" aria-label="' + action + label + '" aria-controls="' + id + '">' + symbol + '</button>';
  return '<div class="parameter-tile" id="tile-' + id + '"><label for="' + id + '">' + label + '</label>' +
    '<div class="unit-input"><input type="text" inputmode="' + mode + '" id="' + id + '" name="' + id + '" data-min="' +
    minimum + '" data-max="' + maximum + '" data-step="' + step + '" aria-describedby="range-' + id +
    '" placeholder="—" autocomplete="off" required><span class="unit" aria-hidden="true">' + unit + '</span>' +
    stepButton(-1, "减小", "−") + stepButton(1, "增大", "+") + '</div>' +
    '<p class="hint" id="range-' + id + '">' + hint + '</p></div>';
}

function stepPrecision(input) {
  return input.dataset.step.split(".")[1]?.length ?? 0;
}

function parameterNumber(input) {
  const text = input.value.trim();
  return /^-?(?:\d+(?:\.\d*)?|\.\d+)$/.test(text) ? Number(text) : NaN;
}

function parameterValues(form, feedback) {
  const values = {};
  for (const input of form.querySelectorAll("input[data-step]")) {
    const value = parameterNumber(input);
    const { min, max, step } = input.dataset;
    const steps = (value - Number(min)) / Number(step);
    let error = "";
    if (!Number.isFinite(value)) error = "请输入数字。";
    else if (value < Number(min) || value > Number(max)) error = "请输入 " + min + " 到 " + max + " 之间的值。";
    else if (Math.abs(steps - Math.round(steps)) > 0.000001) error = "调整步长为 " + step + "。";
    if (error) {
      input.setAttribute("aria-invalid", "true");
      input.focus();
      note(feedback, input.labels[0].textContent + "：" + error, "error");
      return null;
    }
    values[input.name] = value;
  }
  return values;
}

function setNumericControlsEnabled(form, enabled) {
  for (const control of form.querySelectorAll("[data-step], [data-step-change]")) control.disabled = !enabled;
}

const parameterPage = byId("page-parameters");
parameterPage.addEventListener("click", (event) => {
  const button = event.target.closest("button[data-step-change]");
  if (!button) return;
  const input = byId(button.getAttribute("aria-controls"));
  const { min, max, step } = input.dataset;
  const minimum = Number(min);
  const increment = Number(step);
  const position = (parameterNumber(input) - minimum) / increment;
  const increase = button.dataset.stepChange === "1";
  const next = increase ? Math.floor(position + 0.000001) + 1 : Math.ceil(position - 0.000001) - 1;
  const value = Number.isFinite(position) ? minimum + next * increment : minimum;
  input.value = Math.min(Number(max), Math.max(minimum, value)).toFixed(stepPrecision(input));
  input.removeAttribute("aria-invalid");
});
parameterPage.addEventListener("input", (event) => event.target.removeAttribute("aria-invalid"));

function parameterCard(group) {
  const fields = group.fields.map((field) => tile(field)).join("");
  const isAir = group.id === "air";
  const feedback = isAir ? "读取设定后可编辑。" : "";
  const readLabel = isAir ? "读取设定" : "重新读取";
  const saveLabel = isAir ? "保存并回读" : "保存参数";
  return '<h2 id="' + group.id + '-parameter-title">' + group.name + '</h2><div class="tile-grid">' + fields + '</div>' +
    '<div class="parameter-footer"><p class="hint" id="' + group.id + '-note" role="status">' + feedback +
    '</p><div class="actions"><button type="button" class="ghost" id="read-' + group.id + '" aria-label="读取' +
    group.name + '">' + readLabel + '</button><button type="submit" class="primary" id="save-' + group.id +
    '" aria-label="保存' + group.name + '"' + (isAir ? " disabled" : "") + '>' + saveLabel + '</button></div></div>';
}

const legacyMeasurement = readSaved("measurement", {});
for (const group of measurementGroups) {
  const form = byId(group.id + "-parameters");
  const feedback = group.id + "-note";
  const defaults = Object.fromEntries(group.fields.map(([name, , , , , , value]) => [name, legacyMeasurement[name] ?? value]));
  const reload = () => setFormValues(form, readSaved(group.id, defaults));
  form.innerHTML = parameterCard(group);
  reload();
  form.addEventListener("submit", (event) => {
    event.preventDefault();
    if (state.running) return;
    const values = parameterValues(form, feedback);
    if (values && saveLocally(group.id, values, feedback)) note(feedback, "已保存，下次检测使用。", "success");
  });
  byId("read-" + group.id).addEventListener("click", () => {
    reload();
    note(feedback, "已重新读取。");
  });
}

const airDefaults = Object.fromEntries(airGroup.fields.map((field) => [field[0], field[6]]));
const airForm = byId("air-parameters");
airForm.innerHTML = parameterCard(airGroup);
setNumericControlsEnabled(airForm, false);
byId("read-air").addEventListener("click", () => {
  if (scenario.value === "offline") {
    note("air-note", "模拟空调离线。可切换到设备就绪场景后重试。", "error");
    return;
  }
  setFormValues(airForm, readSaved("air", airDefaults));
  setNumericControlsEnabled(airForm, true);
  byId("save-air").disabled = false;
  note("air-note", "已读取模拟设定。");
});
airForm.addEventListener("submit", (event) => {
  event.preventDefault();
  if (byId("save-air").disabled) return;
  const values = parameterValues(airForm, "air-note");
  if (!values) return;
  if (values.cooling_on_c <= values.cooling_off_c || values.heating_on_c >= values.heating_off_c ||
      values.dehumidify_on_percent <= values.dehumidify_off_percent) {
    note("air-note", "请检查制冷、加热及除湿的启停设定。", "error");
    return;
  }
  if (saveLocally("air", values, "air-note")) {
    note("air-note", "模拟设定已保存，回读一致。", "success");
  }
});

const rangeForm = byId("rangefinder-parameters");
const rangeDefaults = Object.fromEntries(rangeFields.map((field) => [field[0], field[6]]));
const rangeHints = ["空载皮带到探头的距离", "距离减少达到此值判为有料", "有效窗口的最小距离", "有效窗口的最大距离"];
let savedRangeRules = readSaved("rangefinder-rules", rangeDefaults);
let activeRangeRules = null;
byId("rangefinder-rule-fields").innerHTML = rangeFields.map((field, index) => tile(field, rangeHints[index])).join("");
setFormValues(rangeForm, savedRangeRules);
rangeForm.addEventListener("submit", (event) => {
  event.preventDefault();
  const feedback = "rangefinder-rule-note";
  const values = parameterValues(rangeForm, feedback);
  if (!values) return;
  if (values.valid_min_mm >= values.valid_max_mm) {
    note(feedback, "可测距离下限须小于上限。", "error");
    rangeForm.elements.valid_max_mm.focus();
    return;
  }
  if (values.material_delta_mm >= values.empty_belt_mm) {
    note(feedback, "来料阈值须小于空带基准。", "error");
    rangeForm.elements.material_delta_mm.focus();
    return;
  }
  if (!saveLocally("rangefinder-rules", values, feedback)) return;
  savedRangeRules = values;
  note(feedback, state.running ? "已保存。当前检测继续使用启动时的判定参数。" : "测距判定已保存。", state.running ? "pending" : "success");
  updateRangePreview();
});

function sampleDistance() {
  return Number((198.4 + Math.sin(state.samples * 0.2) * 1.3).toFixed(1));
}

function updateRangePreview() {
  const material = byId("range-material");
  const windowResult = byId("range-window");
  if (scenario.value === "offline") {
    byId("range-distance").textContent = "离线";
    material.hidden = true;
    windowResult.hidden = true;
    return;
  }
  material.hidden = false;
  windowResult.hidden = false;
  const distance = sampleDistance();
  const rules = activeRangeRules || savedRangeRules;
  const hasMaterial = rules.empty_belt_mm - distance >= rules.material_delta_mm;
  const insideWindow = distance >= rules.valid_min_mm && distance <= rules.valid_max_mm;
  byId("range-distance").textContent = distance.toFixed(1) + " mm";
  material.firstElementChild.className = "dot " + (hasMaterial ? "ok" : "warning");
  material.lastElementChild.textContent = hasMaterial ? "有料" : "无料";
  windowResult.firstElementChild.className = "dot " + (insideWindow ? "ok" : "warning");
  windowResult.lastElementChild.textContent = insideWindow ? "距离可测" : "距离超窗";
}

const indicatorForm = byId("indicator-parameters");
setFormValues(indicatorForm, readSaved("lamp-trial", { color: "red", pattern: "steady" }));
let lampTest = null;
byId("indicator-color").addEventListener("change", updateLampControls);
indicatorForm.addEventListener("submit", (event) => {
  event.preventDefault();
  if (byId("save-lamp-preset").disabled) return;
  const values = { color: byId("indicator-color").value, pattern: byId("indicator-pattern").value };
  if (saveLocally("lamp-trial", values, "indicator-test-note")) note("indicator-test-note", "试灯参数已保存。", "success");
});
byId("start-lamp-test").addEventListener("click", () => {
  if (state.running || scenario.value !== "ready") return;
  const color = byId("indicator-color").value;
  lampTest = { color, flashing: color !== "off" && byId("indicator-pattern").value === "fast" };
  note("indicator-test-note", "已应用模拟灯态。", "success");
  updateDevices();
});
byId("end-lamp-test").addEventListener("click", () => {
  lampTest = null;
  note("indicator-test-note", "已恢复自动灯态。", "success");
  updateDevices();
});

function lampOutput() {
  const color = lampTest?.color ?? detectionScenes[scenario.value].color;
  const flashing = lampTest?.flashing ?? false;
  return { color, flashing, label: lampColors[color] + (flashing ? "快闪（2 Hz）" : "") };
}

function updateLampControls() {
  const allowed = !state.running && scenario.value === "ready";
  const colorInput = byId("indicator-color");
  colorInput.disabled = !allowed;
  byId("indicator-pattern").disabled = !allowed || colorInput.value === "off";
  byId("save-lamp-preset").disabled = !allowed;
  byId("start-lamp-test").disabled = !allowed;
  byId("end-lamp-test").hidden = !lampTest;
  const output = lampOutput();
  const preview = byId("indicator-preview");
  preview.firstElementChild.className = "dot lamp-" + output.color + (output.flashing ? " blink-fast" : "");
  preview.lastElementChild.textContent = (lampTest ? "试灯 · " : "自动 · ") + output.label;
}

const deviceEntries = Object.entries(deviceNames);
byId("device-cards").innerHTML = deviceEntries.map(([key, name]) =>
  '<section class="card" id="device-' + key + '" aria-labelledby="device-name-' + key + '"><div class="card-title">' +
  '<h2 id="device-name-' + key + '">' + name + '</h2><span class="status"><span class="dot" aria-hidden="true"></span><span class="device-state"></span></span>' +
  '</div><dl class="device-readings"></dl></section>'
).join("");
byId("quick-status").innerHTML = deviceEntries.map(([key, name]) =>
  '<li class="status" id="quick-' + key + '"><span>' + name + '</span><span class="dot" role="img" tabindex="0"></span></li>'
).join("");
byId("prediction-values").innerHTML = predictionFields.map((field) => '<div><dt>' + field + '</dt><dd>—</dd></div>').join("");

function updateDevices() {
  const offline = scenario.value === "offline";
  const interlocked = scenario.value === "interlock";
  const lamp = lampOutput();
  const scene = detectionScenes[scenario.value];
  const systemLight = byId("system-status");
  const systemStatus = state.running ? "检测中" : scene.name;
  systemLight.className = "dot lamp-" + scene.color;
  systemLight.dataset.tooltip = systemStatus;
  systemLight.setAttribute("aria-label", systemStatus);
  const measurements = {
    laser: [["连接", "COM3"], ["工作状态", state.running ? "采集中" : "待机"],
      ["光闸", state.running ? "开" : "关"], ["联锁", interlocked ? "联锁 1 断开" : "联锁 1 / 2 闭合"], ["故障码", interlocked ? "门禁未闭合" : "0"]],
    spectrometer: [["连接", "USB · 3 个通道"], ["通道 1", "185–336 nm"], ["通道 2", "311–456 nm"],
      ["通道 3", "433–565 nm"], ["采集计数", state.frames + " 帧"]],
    indicator: [["连接", "COM8"], ["型号", "CL50S+K50-485"], [lampTest ? "试灯灯态" : "目标灯态", lamp.label],
      ["下发状态", "模拟读回成功"]],
    rangefinder: [["连接", "COM9"], ["距离", sampleDistance().toFixed(1) + " mm"],
      ["置信度", "98%"], ["采样计数", state.samples + ""]],
    air: [["连接", "COM10"], ["柜内温湿度", (26.2 + Math.sin(state.samples * 0.04) * 0.3).toFixed(1) + " °C / 48 %RH"],
      ["电源", "开"], ["制冷 / 加热", "停止 / 停止"], ["告警", "无"]],
  };
  for (const [key, name] of deviceEntries) {
    const card = byId("device-" + key);
    const alarm = interlocked && key === "laser";
    const status = offline ? "设备离线" : alarm ? "联锁告警" : "已打开";
    const dotClass = "dot " + (offline ? "" : alarm ? "bad" : "ok");
    card.querySelector(".dot").className = dotClass;
    card.querySelector(".device-state").textContent = status;
    const readings = card.querySelector(".device-readings");
    for (const [index, [label, value]] of measurements[key].entries()) {
      let row = readings.children[index];
      if (!row) {
        row = document.createElement("div");
        row.id = "device-" + key + "-reading-" + (index + 1);
        row.append(document.createElement("dt"), document.createElement("dd"));
        readings.append(row);
      }
      row.hidden = offline && index > 0;
      row.firstElementChild.textContent = label;
      row.lastElementChild.textContent = offline ? "等待连接" : value;
    }
    const quick = byId("quick-" + key);
    const quickDot = quick.lastElementChild;
    quickDot.className = dotClass;
    quickDot.setAttribute("aria-label", status);
    quickDot.dataset.tooltip = status;
  }
  note("refresh-note", "最近读取 " + displayTime.format(new Date()));
  updateRangePreview();
  updateLampControls();
}

function navigate() {
  if (location.hash === "#parameters/connections") history.replaceState(null, "", "#parameters");
  const section = location.hash.slice(1);
  const active = section === "parameters" || section === "devices" ? section : "spectrum";
  for (const link of document.querySelectorAll("[data-page-link]")) {
    const selected = link.dataset.pageLink === active;
    byId("page-" + link.dataset.pageLink).hidden = !selected;
    link.setAttribute("aria-current", selected ? "page" : "false");
  }
  if (active !== "spectrum") updateDevices();
  if (active === "spectrum") drawSpectrum();
  window.scrollTo(0, 0);
}

function sampleFrame() {
  return channels.map(({ start, end, peaks }) => {
    const points = [];
    for (let wavelength = start; wavelength <= end; wavelength += 0.25) {
      const ripple = Math.sin(wavelength * 11.2 + state.frames * 0.4) * 270 + Math.sin(wavelength * 1.3) * 420;
      let intensity = 1800 + 18 * (end - wavelength) + ripple;
      for (const [center, height] of peaks) {
        const offset = (wavelength - center) / (0.48 + center % 5 * 0.1);
        intensity += height * (0.91 + Math.sin(state.frames * 0.13 + center) * 0.07) * Math.exp(-0.5 * offset ** 2);
      }
      points.push([wavelength, Math.round(intensity)]);
    }
    return points;
  });
}

function drawSpectrum() {
  const width = canvas.clientWidth;
  const height = canvas.clientHeight;
  if (!width || !height) return;
  const ratio = window.devicePixelRatio || 1;
  if (canvas.width !== Math.round(width * ratio) || canvas.height !== Math.round(height * ratio)) {
    canvas.width = Math.round(width * ratio);
    canvas.height = Math.round(height * ratio);
  }
  context.setTransform(ratio, 0, 0, ratio, 0, 0);
  context.clearRect(0, 0, width, height);
  const left = 56;
  const right = width - 12;
  const top = 30;
  const bottom = height - 39;
  const style = getComputedStyle(canvas);
  const color = (variable) => style.getPropertyValue(variable).trim();
  const visible = state.visibleSpectrum;
  if (visible && byId("auto-axis").checked && !byId("lock-axis").checked) {
    const peak = Math.max(...visible.flatMap((points) => points.map(([, intensity]) => intensity)));
    state.scale = Math.max(12000, Math.ceil(peak / 4000) * 4000);
  }
  if (!byId("auto-axis").checked && !byId("lock-axis").checked) state.scale = 64000;
  const scale = state.scale;
  const toX = (value) => left + (right - left) * (value - 185) / 380;
  const toY = (value) => bottom - (bottom - top) * value / scale;
  context.font = '12px "Microsoft YaHei UI", sans-serif';
  context.strokeStyle = color("--border");
  context.fillStyle = color("--muted");
  context.textBaseline = "middle";
  context.textAlign = "left";
  context.fillText("强度 / counts", left, 11);
  context.textAlign = "right";
  context.fillText("波长 / nm", right, height - 6);
  context.setLineDash([3, 2]);
  for (let tick = 0; tick <= 4; tick++) {
    const y = toY(tick * scale / 4);
    context.beginPath();
    context.moveTo(left, y);
    context.lineTo(right, y);
    context.stroke();
    context.fillText(String(tick * scale / 4), left - 7, y);
  }
  context.textAlign = "center";
  const tickCount = width < 470 ? 3 : 6;
  for (let tick = 0; tick <= tickCount; tick++) {
    const wavelength = 185 + 380 * tick / tickCount;
    const x = toX(wavelength);
    context.beginPath();
    context.moveTo(x, top);
    context.lineTo(x, bottom);
    context.stroke();
    context.fillText(String(Math.round(wavelength)), x, bottom + 13);
  }
  context.setLineDash([]);
  context.strokeRect(left, top, right - left, bottom - top);
  if (!visible) return;
  context.save();
  context.beginPath();
  context.rect(left, top, right - left, bottom - top);
  context.clip();
  for (const [index, points] of visible.entries()) {
    context.strokeStyle = color(channels[index].color);
    context.lineWidth = 1.25;
    context.beginPath();
    for (const [wavelength, intensity] of points) context.lineTo(toX(wavelength), toY(intensity));
    context.stroke();
  }
  context.restore();
}

function showPrediction(source = "") {
  const sample = predictionSamples[branch.value];
  const ready = sample && (source || state.spectrum);
  for (const [index, value] of byId("prediction-values").querySelectorAll("dd").entries()) {
    value.textContent = ready ? sample[index].toFixed(index === 7 || index === 8 ? 3 : 2) : "—";
  }
  note("prediction-note", !sample ? "选择矿石类型后启用实时预测。"
    : source ? "导入演示 · " + source
    : ready ? "模拟预测 · 第 " + state.frames + " 帧" : "等待有效光谱。");
}

function updateCapture() {
  const paused = byId("pause-display").checked;
  byId("plot-empty").hidden = Boolean(state.visibleSpectrum);
  byId("spectrum-legend").hidden = !state.visibleSpectrum;
  byId("export-spectrum").disabled = !state.spectrum;
  note("capture-note", paused ? "显示已暂停 · 已采集 " + state.frames + " 帧"
    : !state.spectrum ? "等待采集 · 尚无光谱数据"
    : (state.running ? "采集中" : "已停止") + " · 第 " + state.frames + " 帧 · 3 个通道");
  note("capture-counters", "采集 " + state.frames + " / 失败 " + state.failures + " · 落盘 " + state.frames + " / 失败 0 · 积压 0");
  drawSpectrum();
}

function measure() {
  if (!state.running) return;
  state.frames++;
  state.spectrum = sampleFrame();
  if (!byId("pause-display").checked) state.visibleSpectrum = state.spectrum;
  updateCapture();
  showPrediction();
}

function setRunning(running) {
  if (running && lampTest) {
    lampTest = null;
    note("indicator-test-note", "检测已启动，状态灯恢复自动。");
  }
  if (!running && activeRangeRules && activeRangeRules !== savedRangeRules) {
    note("rangefinder-rule-note", "已保存的参数将在下次检测时使用。", "success");
  }
  activeRangeRules = running ? savedRangeRules : null;
  state.running = running;
  updateGateState();
  byId("stop-measurement").disabled = !running;
  for (const group of measurementGroups) {
    setNumericControlsEnabled(byId(group.id + "-parameters"), !running);
    byId("save-" + group.id).disabled = running;
    byId("read-" + group.id).disabled = running;
    note(group.id + "-note", running ? "检测中，停止后可修改。" : "", running ? "pending" : "");
  }
  updateDevices();
}

function blockingReason() {
  return detectionScenes[scenario.value].reason || "";
}

function updateGateState() {
  const reason = blockingReason();
  byId("start-measurement").disabled = state.running || Boolean(reason);
  note("gate-state", reason ? "不可检测 · " + reason : state.running ? "检测中" : "可检测",
    reason ? "error" : "success");
}

byId("start-measurement").addEventListener("click", () => {
  const reason = blockingReason();
  if (reason) {
    note("measurement-message", reason, "error");
    return;
  }
  state.frames = 0;
  state.failures = 0;
  setRunning(true);
  note("measurement-message", "正在模拟检测。生成的光谱和预测结果用于界面调试。", "success");
  measure();
});
byId("stop-measurement").addEventListener("click", () => {
  setRunning(false);
  note("measurement-message", "模拟检测已停止。可查看或导出最后一帧。");
  updateCapture();
});
scenario.addEventListener("change", () => {
  const reason = blockingReason();
  if (reason && lampTest) {
    lampTest = null;
    note("indicator-test-note", "已结束试灯，状态灯恢复自动。");
  }
  if (state.running && reason) {
    state.failures++;
    setRunning(false);
    updateCapture();
  }
  updateGateState();
  note("measurement-message", reason || "模拟采集，可启停并导出光谱。", reason ? "error" : "");
  if (scenario.value === "offline") {
    setNumericControlsEnabled(airForm, false);
    byId("save-air").disabled = true;
    note("air-note", "模拟空调离线。", "error");
  } else if (byId("save-air").disabled) {
    note("air-note", "读取设定后可编辑。");
  }
  updateDevices();
});
for (const id of ["auto-axis", "lock-axis"]) byId(id).addEventListener("change", drawSpectrum);
byId("pause-display").addEventListener("change", () => {
  if (!byId("pause-display").checked) state.visibleSpectrum = state.spectrum;
  updateCapture();
});
byId("export-spectrum").addEventListener("click", () => {
  const csv = ["channel,wavelength_nm,intensity_counts"];
  state.spectrum.forEach((points, channel) => {
    for (const [wavelength, intensity] of points) csv.push([channel + 1, wavelength, intensity].join(","));
  });
  const url = URL.createObjectURL(new Blob([csv.join("\n")], { type: "text/csv;charset=utf-8" }));
  const link = document.createElement("a");
  link.href = url;
  link.download = "libs-demo-frame-" + state.frames + ".csv";
  link.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
});
branch.addEventListener("change", () => showPrediction());
byId("retry-prediction").addEventListener("click", () => showPrediction());
byId("import-spectrum").addEventListener("click", () => {
  if (!branch.value) {
    note("prediction-note", "请先选择矿石类型。", "error");
    branch.focus();
    return;
  }
  byId("spectrum-file").click();
});
byId("spectrum-file").addEventListener("change", (event) => {
  const file = event.target.files[0];
  if (file) showPrediction(file.name);
  event.target.value = "";
});
byId("refresh-devices").addEventListener("click", () => {
  state.samples++;
  updateDevices();
});

window.addEventListener("hashchange", navigate);
new ResizeObserver(drawSpectrum).observe(byId("spectrum-plot"));
const updateClock = () => byId("clock").textContent = displayClock.format(new Date());
setInterval(updateClock, 1000);
setInterval(measure, 650);
setInterval(() => {
  if (byId("page-devices").hidden && byId("page-parameters").hidden) return;
  state.samples++;
  updateDevices();
}, 2500);
updateClock();
updateGateState();
updateDevices();
navigate();
