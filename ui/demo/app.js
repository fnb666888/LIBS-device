"use strict";

const byId = (id) => document.getElementById(id);
const scenario = byId("demo-scenario");
const motorScenario = byId("demo-motor");
const branch = byId("prediction-branch");
const canvas = byId("spectrum-canvas");
const context = canvas.getContext("2d");
const displayDate = new Intl.DateTimeFormat("sv-SE", { year: "numeric", month: "2-digit", day: "2-digit" });
const displayTime = new Intl.DateTimeFormat("zh-CN", { timeStyle: "medium", hourCycle: "h23" });
const displayNumber = new Intl.NumberFormat("zh-CN", { maximumFractionDigits: 3 });

const measurementGroups = [
  { id: "laser", fields: [
    ["discharge_current_a", "放电电流", 15, 100, 1, "A", 80],
    ["pulse_width_us", "放电脉宽", 50, 300, 1, "μs", 200],
    ["frequency_hz", "放电频率", 1, 10, 1, "Hz", 1],
    ["discharge_delay_us", "放电延时", 0, 500, 1, "μs", 0],
    ["q_delay_us", "Q 延时", 2, 500, 1, "μs", 210],
    ["q_warmup_s", "Q 预热", 0, 3600, 1, "s", 0],
    ["settle_timeout_ms", "稳定等待超时", 1, 600000, 1, "ms", 20000],
  ] },
  { id: "spectrometer", fields: [
    ["integration_ms", "积分时间", 0.002, 60000, 0.001, "ms", "1.050"],
    ["integration_delay_ns", "积分延时", 0, 1000000, 1, "ns", 1280],
    ["capture_timeout_ms", "采集超时", 1, 600000, 1, "ms", 10000],
  ] },
];
const airGroup = { id: "air", fields: [
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
const motorFields = [
  ["jog_ms", "点动时长", 100, 5000, 1, "ms", 1800],
  ["jog_hz", "模拟点动频率", 0.1, 10, 0.1, "Hz", 5],
];
const lampColors = { red: "红灯", yellow: "黄灯", green: "绿灯", off: "全灭" };
const detectionScenes = {
  ready: { lamp: "green" },
  standby: { lamp: "yellow", reason: "测量设备未启用或未就绪" },
  interlock: { lamp: "red", reason: "联锁断开" },
  offline: { lamp: "off", reason: "设备尚未全部打开" },
  "aux-warning": { lamp: "green", warning: "辅助设备告警 · 空调设备告警" },
};
const deviceNames = {
  laser: "激光器", spectrometer: "光谱仪", indicator: "三色状态灯", rangefinder: "测距仪", air: "空调",
  motor: "检测头移位电机",
};
const airModes = { cooling: "制冷", heating: "加热", dehumidify: "除湿" };
const manualActions = {
  laser: [["start-laser-trial", "连续出光"], ["stop-laser-trial", "停止出光"]],
  spectrometer: [["capture-spectrometer", "采集一帧"]],
  air: [...Object.entries(airModes).map(([mode, label]) => ["test-air-" + mode, "试运行" + label]),
    ["stop-air-trial", "停止试运行"]],
};
const predictionFields = ["TFe", "SiO₂", "Al₂O₃", "TiO₂", "CaO", "MgO", "MnO", "P", "S", "FeO"];
const predictionSamples = {
  oxide: [62.84, 6.29, 2.06, 0.28, 0.91, 0.58, 0.14, 0.043, 0.028, 11.47],
  sulfide: [52.41, 8.36, 3.18, 0.35, 1.16, 0.73, 0.21, 0.048, 1.52, 9.62],
};
const channels = [
  { color: "--accent", start: 185.6, end: 312.1, peaks: [[213, 17000], [239, 28200], [248, 38700], [259, 42200], [271, 23600], [296, 19000], [309, 32200]] },
  { color: "--trace-green", start: 305.8, end: 415.9, peaks: [[309, 31000], [326, 11000], [357, 26800], [372, 30700], [383, 16200], [405, 14300]] },
  { color: "--trace-amber", start: 403.7, end: 496.7, peaks: [[405, 13100], [422, 40000], [438, 21000], [451, 14400], [476, 19700], [492, 33700]] },
  { color: "--trace-purple", start: 512.0, end: 564.7, peaks: [[516, 29100], [528, 18000], [540, 25500], [555, 14200]] },
];
const wavelengthMin = channels[0].start;
const wavelengthMax = channels.at(-1).end;
const wavelengthSpan = wavelengthMax - wavelengthMin;
const legendItems = channels.map(({ color }, index) => {
  const item = document.createElement("span");
  const trace = document.createElement("i");
  trace.className = "trace";
  trace.style.backgroundColor = "var(" + color + ")";
  item.append(trace, "通道 " + (index + 1));
  return item;
});
byId("spectrum-legend").replaceChildren(...legendItems);
const state = { running: false, frames: 0, failures: 0, samples: 0, spectrum: null, visibleSpectrum: null, scale: 64000 };
const defaultBanner = byId("measurement-message").textContent;
const motor = { phase: "stopped", direction: "", outputHz: 0, timer: 0 };
let laserTrial = null;
let airTrial = null;
let spectrometerTrials = 0;

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
    '<span class="step-buttons">' + stepButton(1, "增大", "⌃") + stepButton(-1, "减小", "⌄") + '</span></div>' +
    '<p class="hint" id="range-' + id + '">' + hint + '</p></div>';
}

function stepPrecision(input) {
  return input.dataset.step.split(".")[1]?.length ?? 0;
}

function parameterNumber(input) {
  const text = input.value.trim();
  return /^-?(?:\d+(?:\.\d*)?|\.\d+)$/.test(text) ? Number(text) : NaN;
}

function validatedParameter(input, feedback) {
  const value = parameterNumber(input);
  const { min, max, step } = input.dataset;
  const steps = (value - Number(min)) / Number(step);
  let error = "";
  if (!Number.isFinite(value)) error = "请输入数字。";
  else if (value < Number(min) || value > Number(max)) error = "请输入 " + min + " 到 " + max + " 之间的值。";
  else if (Math.abs(steps - Math.round(steps)) > 0.000001) error = "调整步长为 " + step + "。";
  if (!error) return value;
  input.setAttribute("aria-invalid", "true");
  input.focus();
  note(feedback, input.labels[0].textContent + "：" + error, "error");
  return null;
}

function parameterValues(form, feedback) {
  const values = {};
  for (const input of form.querySelectorAll("input[data-step]")) {
    const value = validatedParameter(input, feedback);
    if (value === null) return null;
    values[input.name] = value;
  }
  return values;
}

function setNumericControlsEnabled(form, enabled) {
  for (const control of form.querySelectorAll("[data-step], [data-step-change]")) control.disabled = !enabled;
}

function sampleCabinet() {
  const temperature = Number((26.2 + Math.sin(state.samples * 0.04) * 0.3).toFixed(1));
  return { temperature, humidity: scenario.value === "aux-warning" ? 72 : 48 };
}

function validAirThresholds(values) {
  return values.cooling_on_c > values.cooling_off_c && values.heating_on_c < values.heating_off_c &&
    values.dehumidify_on_percent > values.dehumidify_off_percent;
}

function airTest(values) {
  if (!validAirThresholds(values)) return "请检查制冷、加热及除湿的启停设定。";
  const { temperature, humidity } = sampleCabinet();
  const action = (start, stop) => start ? "开启" : stop ? "停止" : "保持";
  return "模拟柜温 " + temperature + " °C / 湿度 " + humidity + " %RH\n" +
    "制冷 " + action(temperature >= values.cooling_on_c, temperature <= values.cooling_off_c) +
    " · 加热 " + action(temperature <= values.heating_on_c, temperature >= values.heating_off_c) +
    " · 除湿 " + action(humidity >= values.dehumidify_on_percent, humidity <= values.dehumidify_off_percent);
}

function rangeDecision(rules, distance) {
  return {
    hasMaterial: rules.empty_belt_mm - distance >= rules.material_delta_mm,
    insideWindow: distance >= rules.valid_min_mm && distance <= rules.valid_max_mm,
  };
}

function rangeTest(rules) {
  if (rules.valid_min_mm >= rules.valid_max_mm || rules.material_delta_mm >= rules.empty_belt_mm) {
    return "请检查可测距离上下限、空带基准和来料阈值。";
  }
  const distance = sampleDistance();
  const { hasMaterial, insideWindow } = rangeDecision(rules, distance);
  return "模拟距离 " + displayNumber.format(distance) + " mm · " + (hasMaterial ? "有料" : "无料") +
    " · " + (insideWindow ? "距离可测" : "距离超窗");
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

const motorSettingsForm = byId("motor-settings");
const motorDefaults = Object.fromEntries(motorFields.map(([name, , , , , , value]) => [name, value]));
byId("motor-setting-fields").innerHTML = motorFields.map((field) => tile(field)).join("");
const reloadMotorSettings = () => setFormValues(motorSettingsForm, readSaved("motor-jog", motorDefaults));
reloadMotorSettings();
byId("read-motor-settings").addEventListener("click", () => {
  reloadMotorSettings();
  note("motor-settings-note", "已重新读取。");
});
motorSettingsForm.addEventListener("submit", (event) => {
  event.preventDefault();
  if (byId("save-motor-settings").disabled) return;
  const feedback = "motor-settings-note";
  const values = parameterValues(motorSettingsForm, feedback);
  if (!values || !saveLocally("motor-jog", values, feedback)) return;
  note(feedback, "已保存。", "success");
});

function parameterCard(group) {
  const name = deviceNames[group.id];
  const fields = group.fields.map((field) => tile(field)).join("");
  const buttons = manualActions[group.id].map(([id, label]) =>
    '<button type="button" id="' + id + '"' + (id.startsWith("stop-") ? ' class="danger"' : "") + '>' + label + '</button>');
  return '<div class="card-title"><h2 id="' + group.id + '-parameter-title">' + name + '</h2></div>' +
    '<section class="parameter-section" aria-label="参数调节"><div class="card-title"><h3>参数调节</h3>' +
    '<a class="parameter-help-link" href="#help/' + group.id + '" aria-label="' + name + '参数说明">参数说明</a></div>' +
    '<div class="tile-grid">' + fields + '</div>' +
    '<div class="parameter-footer"><p class="hint" id="' + group.id + '-note" role="status"></p>' +
    '<div class="actions"><button type="button" class="ghost" id="read-' + group.id + '" aria-label="读取' +
    name + '参数">重新读取</button><button type="submit" class="primary" id="save-' + group.id +
    '" aria-label="保存' + name + '参数">保存参数</button></div></div></section>' +
    '<section class="manual-section" aria-label="手动测试"><h3>手动测试</h3>' +
    '<div class="actions">' + buttons.join("") + '</div>' +
    (group.id === "laser" ? '<p class="hint manual-result" id="laser-trial-unavailable"></p>' : "") +
    '<p class="hint manual-result" id="' + group.id + '-trial-result" role="status"></p></section>';
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
    if (byId("save-" + group.id).disabled) return;
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
const reloadAirSettings = () => setFormValues(airForm, readSaved("air", airDefaults));
reloadAirSettings();
byId("read-air").addEventListener("click", () => {
  reloadAirSettings();
  note("air-note", "已重新读取。");
});
airForm.addEventListener("submit", (event) => {
  event.preventDefault();
  const values = parameterValues(airForm, "air-note");
  if (!values) return;
  if (!validAirThresholds(values)) {
    note("air-note", "请检查制冷、加热及除湿的启停设定。", "error");
    return;
  }
  if (saveLocally("air", values, "air-note")) {
    note("air-note", "已保存。", "success");
  }
});

const rangeForm = byId("rangefinder-parameters");
const rangeDefaults = Object.fromEntries(rangeFields.map((field) => [field[0], field[6]]));
const rangeHints = ["空载皮带到探头的距离", "距离减少达到此值判为有料", "有效窗口的最小距离", "有效窗口的最大距离"];
let savedRangeRules = readSaved("rangefinder-rules", rangeDefaults);
let activeRangeRules = null;
byId("rangefinder-rule-fields").innerHTML = rangeFields.map((field, index) => tile(field, rangeHints[index])).join("");
setFormValues(rangeForm, savedRangeRules);
byId("reload-range-rules").addEventListener("click", () => {
  savedRangeRules = readSaved("rangefinder-rules", rangeDefaults);
  setFormValues(rangeForm, savedRangeRules);
  updateRangePreview();
  note("rangefinder-rule-note", "已重新读取。");
});
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
  if (scenario.value === "offline") {
    renderDevice("rangefinder", "离线", "bad");
    return;
  }
  const distance = sampleDistance();
  const rules = activeRangeRules || savedRangeRules;
  const { hasMaterial, insideWindow } = rangeDecision(rules, distance);
  renderDevice("rangefinder", "在线", "ok", [
    ["当前距离", distance.toFixed(1) + " mm"],
    ["来料判定", hasMaterial ? "有料" : "无料"],
    ["可测窗口", insideWindow ? "距离可测" : "距离超窗", insideWindow ? "" : "warning"],
  ]);
}

byId("read-rangefinder").addEventListener("click", () => {
  const feedback = "rangefinder-trial-result";
  const rules = parameterValues(rangeForm, feedback);
  if (!rules) return;
  state.samples++;
  updateDevices();
  note(feedback, rangeTest(rules));
});

let lampTest = null;
byId("indicator-color").addEventListener("change", updateLampControls);
byId("start-lamp-test").addEventListener("click", () => {
  if (state.running) return;
  const color = byId("indicator-color").value;
  lampTest = { color, flashing: color !== "off" && byId("indicator-pattern").value === "fast" };
  note("indicator-test-note", "正在预览试灯。", "success");
  updateLampControls();
});
byId("end-lamp-test").addEventListener("click", () => {
  lampTest = null;
  note("indicator-test-note", "已退出试灯预览。", "success");
  updateLampControls();
});

function automaticLamp() {
  const scene = detectionScenes[scenario.value];
  return !scene.reason && blockingReason() ? "yellow" : scene.lamp;
}

function updateLampControls() {
  const colorInput = byId("indicator-color");
  byId("indicator-pattern").disabled = colorInput.value === "off";
  byId("start-lamp-test").disabled = state.running;
  byId("end-lamp-test").disabled = !lampTest;
  const offline = scenario.value === "offline";
  if (offline && !lampTest) {
    renderDevice("indicator", "离线", "bad");
    return;
  }
  const output = lampTest ?? { color: automaticLamp(), flashing: false };
  const metrics = [[lampTest ? "试灯灯态" : "自动灯态", lampColors[output.color],
    "lamp-" + output.color + (output.flashing ? " blink-fast" : "")]];
  if (lampTest && output.color !== "off") metrics.push(["显示方式", output.flashing ? "快闪（2 Hz）" : "常亮"]);
  renderDevice("indicator", offline ? "离线" : "在线", offline ? "bad" : "ok", metrics);
}

const deviceEntries = Object.entries(deviceNames);
for (const [key, name] of deviceEntries) {
  const title = byId(key + "-parameters").querySelector(".card-title");
  title.insertAdjacentHTML("afterend",
    '<section class="device-status-section" aria-label="状态"><div class="device-status-heading"><h3>状态</h3>' +
    '<span class="status device-status" id="device-status-' + key + '">' +
    '<span class="dot" aria-hidden="true"></span><span class="device-state"></span></span></div>' +
    '<dl class="device-metrics" id="device-' + key + '" aria-label="' + name + '状态读数" hidden></dl></section>');
}
byId("quick-status").innerHTML = deviceEntries.map(([key, name]) =>
  '<li class="status" id="quick-' + key + '"><span>' + name + '</span><span class="dot" role="img" tabindex="0"></span></li>'
).join("");
byId("prediction-values").innerHTML = predictionFields.map((field) => '<div><dt>' + field + '</dt><dd>—</dd></div>').join("");

function renderDevice(key, status, level, metrics = []) {
  const badge = byId("device-status-" + key);
  badge.dataset.level = level;
  badge.firstElementChild.className = "dot " + level;
  badge.lastElementChild.textContent = status;

  const list = byId("device-" + key);
  const rows = metrics.map(([name, value, tone = ""]) => {
    const row = document.createElement("div");
    const term = document.createElement("dt");
    const reading = document.createElement("dd");
    term.textContent = name;
    if (tone.startsWith("lamp-")) {
      const dot = document.createElement("span");
      dot.className = "dot " + tone;
      dot.setAttribute("aria-hidden", "true");
      reading.append(dot);
    } else {
      reading.dataset.tone = tone;
    }
    reading.append(value);
    row.append(term, reading);
    return row;
  });
  list.replaceChildren(...rows);
  list.hidden = !rows.length;

  const quickDot = byId("quick-" + key).lastElementChild;
  quickDot.className = "dot " + level;
  quickDot.setAttribute("aria-label", deviceNames[key] + " · " + status);
  quickDot.dataset.tooltip = status;
}

function motorConnected() {
  return scenario.value !== "offline" && !["offline", "disabled"].includes(motorScenario.value);
}

function renderMotorState(status, level, readings, controlNote) {
  renderDevice("motor", status, level, readings);
  note("motor-control-note", controlNote);
}

function renderMotor() {
  const mode = motorScenario.value;
  const connected = motorConnected();
  const controls = connected && mode !== "read-only";
  const stopped = motor.phase === "stopped";
  const canJog = controls && stopped && !state.running && !laserTrial && mode !== "fault";
  setNumericControlsEnabled(motorSettingsForm, canJog);
  byId("read-motor-settings").disabled = !canJog;
  byId("save-motor-settings").disabled = !canJog;
  byId("motor-forward").disabled = !canJog || mode === "forward-limit";
  byId("motor-reverse").disabled = !canJog || mode === "reverse-limit";
  byId("motor-stop").disabled = !controls;
  if (!connected) {
    if (mode === "disabled") renderMotorState("未启用", "", [], "站点未启用检测头移位电机。");
    else renderMotorState("未连接", "bad", [], "连接变频器后可操作。");
    return;
  }
  const limit = mode === "forward-limit" ? "正向" : mode === "reverse-limit" ? "反向" : "";
  const states = { stopped: limit ? limit + "限位触发" : "停机已回读", starting: "等待点动回读",
    jogging: "点动中", stopping: "等待停机回读" };
  const direction = motor.direction === "forward" ? "正向" : motor.direction === "reverse" ? "反向" : "—";
  const outputFrequency = displayNumber.format(motor.outputHz) + " Hz";
  const readings = [["输出频率", outputFrequency], ["行程限位", limit ? limit + "触发" : "未触发", limit ? "warning" : ""]];
  if (motor.outputHz > 0) readings.push(["运行方向", direction]);
  if (mode === "fault") readings.push(["故障码", "3", "bad"]);
  let controlNote = "按当前输入试点动，到时自动停止。";
  if (!controls) controlNote = "站点尚未启用手动移位。";
  else if (mode === "fault") controlNote = "变频器故障，点动不可用。";
  else if (state.running) controlNote = "检测中，停止检测后可点动。";
  else if (laserTrial) controlNote = "激光器正在手动测试，停止出光后可点动。";
  else if (!stopped) controlNote = "确认停机后可再次点动。";
  else if (limit) controlNote = limit + "限位已触发，可向另一方向点动。";
  renderMotorState(mode === "fault" ? "变频器故障" : states[motor.phase],
    mode === "fault" ? "bad" : stopped && !limit ? "ok" : "warning", readings, controlNote);
}

function showMotorResult(message, tone = "") {
  note("motor-result", message, tone);
  byId("motor-result").hidden = !message;
}

function resetMotor() {
  clearTimeout(motor.timer);
  motor.phase = "stopped";
  motor.direction = "";
  motor.outputHz = 0;
}

function stopMotor() {
  clearTimeout(motor.timer);
  if (motor.phase === "stopped") {
    showMotorResult("变频器停机已回读");
    return;
  }
  motor.phase = "stopping";
  showMotorResult("已发送停止指令，等待停机回读");
  motor.timer = setTimeout(() => {
    resetMotor();
    showMotorResult("变频器停机已回读");
    updateDevices();
  }, 350);
  updateDevices();
}

for (const direction of ["forward", "reverse"]) {
  byId("motor-" + direction).addEventListener("click", () => {
    if (scenario.value === "interlock") {
      showMotorResult("无法确认光闸关闭、联锁闭合且急停解除", "error");
      return;
    }
    const settings = parameterValues(motorSettingsForm, "motor-settings-note");
    if (!settings) return;
    motor.phase = "starting";
    motor.direction = direction;
    showMotorResult("已发送点动指令，请查看状态回读");
    motor.timer = setTimeout(() => {
      motor.phase = "jogging";
      motor.outputHz = settings.jog_hz;
      motor.timer = setTimeout(stopMotor, settings.jog_ms);
      updateDevices();
    }, 250);
    updateDevices();
  });
}
byId("motor-stop").addEventListener("click", stopMotor);

function finishLaserTrial(message, tone = "") {
  const { values, timer, openedAt } = laserTrial;
  clearTimeout(timer);
  laserTrial = null;
  if (openedAt !== null) {
    const elapsed = performance.now() - openedAt;
    const pulses = Math.floor(elapsed * values.frequency_hz / 1000);
    message += "\n模拟放电 " + pulses + " 次";
  }
  note("laser-trial-result", message, tone);
  updateDevices();
}

function beginLaserEmission() {
  const { values } = laserTrial;
  const format = displayNumber.format;
  laserTrial.phase = "模拟出光中";
  laserTrial.openedAt = performance.now();
  note("laser-trial-result", "模拟连续出光中 · 放电间隔 " + format(1000 / values.frequency_hz) +
    " ms · 点击停止模拟出光结束\n" +
    format(values.discharge_current_a) + " A / " + format(values.pulse_width_us) + " μs" +
    " · 放电延时 " + format(values.discharge_delay_us) + " μs · Q 延时 " + format(values.q_delay_us) + " μs");
  updateDevices();
}

byId("start-laser-trial").addEventListener("click", () => {
  const values = parameterValues(byId("laser-parameters"), "laser-trial-result");
  if (!values) return;
  const readyDelayMs = 500;
  laserTrial = { values, phase: "等待就绪", timer: 0, openedAt: null };
  laserTrial.timer = setTimeout(() => {
    if (values.settle_timeout_ms < readyDelayMs) {
      finishLaserTrial("模拟等待超时 · 设备预计 " + readyDelayMs + " ms 后就绪。", "error");
    } else if (values.q_warmup_s > 0) {
      laserTrial.phase = "Q 预热中";
      laserTrial.timer = setTimeout(beginLaserEmission, values.q_warmup_s * 1000);
      note("laser-trial-result", "正在模拟预热 " + values.q_warmup_s + " 秒。");
      updateDevices();
    } else {
      beginLaserEmission();
    }
  }, Math.min(values.settle_timeout_ms, readyDelayMs));
  note("laser-trial-result", "正在等待模拟设备就绪。");
  updateDevices();
});

byId("stop-laser-trial").addEventListener("click", () => {
  if (laserTrial) finishLaserTrial("已停止模拟出光，光闸已关闭。");
});

byId("capture-spectrometer").addEventListener("click", () => {
  const feedback = "spectrometer-trial-result";
  const values = parameterValues(byId("spectrometer-parameters"), feedback);
  if (!values) return;
  const triggerDelayMs = 20;
  if (values.capture_timeout_ms < triggerDelayMs) {
    note(feedback, "等待触发超时 · 模拟触发在 " + triggerDelayMs + " ms 到达。", "error");
    return;
  }
  spectrometerTrials++;
  const endNs = values.integration_delay_ns + values.integration_ms * 1000000;
  note(feedback, "已模拟采集第 " + spectrometerTrials + " 帧 · " + channels.length + " 路通道\n" +
    "触发后的积分窗口 " + displayNumber.format(values.integration_delay_ns) + "–" +
    displayNumber.format(endNs) + " ns", "success");
  updateDevices();
});

for (const [mode, label] of Object.entries(airModes)) {
  byId("test-air-" + mode).addEventListener("click", () => {
    const feedback = "air-trial-result";
    const values = parameterValues(airForm, feedback);
    if (!values) return;
    if (!validAirThresholds(values)) {
      note(feedback, "请检查制冷、加热及除湿的启停设定。", "error");
      return;
    }
    airTrial = mode;
    note(feedback, "正在模拟" + label + "试运行。\n自动控制在当前读数下的判定\n" + airTest(values));
    updateDevices();
  });
}

byId("stop-air-trial").addEventListener("click", () => {
  airTrial = null;
  note("air-trial-result", "已停止空调模拟试运行。");
  updateDevices();
});

function updateManualControls() {
  const offline = scenario.value === "offline";
  const laserBlock = deviceBlockingReason();
  byId("start-laser-trial").disabled = state.running || Boolean(laserTrial || laserBlock);
  byId("stop-laser-trial").disabled = !laserTrial;
  note("laser-trial-unavailable", !state.running && !laserTrial ? laserBlock : "");
  byId("capture-spectrometer").disabled = state.running || offline;
  byId("read-rangefinder").disabled = state.running || offline;
  for (const mode of Object.keys(airModes)) {
    byId("test-air-" + mode).disabled = state.running || offline || Boolean(airTrial);
  }
  byId("stop-air-trial").disabled = !airTrial;
  for (const { id } of measurementGroups) {
    const editable = !state.running && (id !== "laser" || !laserTrial);
    setNumericControlsEnabled(byId(id + "-parameters"), editable);
    byId("save-" + id).disabled = !editable;
    byId("read-" + id).disabled = !editable;
  }
}

function updateDevices() {
  const interlocked = scenario.value === "interlock";
  const airWarning = scenario.value === "aux-warning";
  const { temperature, humidity } = sampleCabinet();
  if (scenario.value === "offline") {
    for (const device of ["laser", "spectrometer", "air"]) renderDevice(device, "离线", "bad");
  } else {
    renderDevice("laser", interlocked ? "联锁断开" : laserTrial?.phase ?? "在线",
      interlocked ? "bad" : laserTrial ? "warning" : "ok", [
      ["光闸", state.running || laserTrial?.openedAt != null ? "开启" : "关闭"],
      ["安全联锁", interlocked ? "断开" : "已闭合", interlocked ? "bad" : ""],
    ]);
    const spectrometerReadings = [
      ["可用通道", channels.length + " 路"],
      ["波段范围", wavelengthMin + "–" + wavelengthMax + " nm"],
    ];
    if (spectrometerTrials) spectrometerReadings.push(["最近试采", "第 " + spectrometerTrials + " 帧"]);
    renderDevice("spectrometer", "在线", "ok", spectrometerReadings);
    const airReadings = [["柜内温度", temperature.toFixed(1) + " °C"], ["相对湿度", humidity + " %RH"]];
    if (airWarning) airReadings.push(["告警", "柜内湿度偏高", "warning"]);
    if (airTrial) airReadings.push(["手动试运行", airModes[airTrial]]);
    renderDevice("air", airWarning ? "设备告警" : airTrial ? "试运行中" : "在线",
      airWarning || airTrial ? "warning" : "ok", airReadings);
  }
  renderMotor();
  updateGateState();
  note("refresh-note", "最近读取 " + displayTime.format(new Date()));
  updateRangePreview();
  updateLampControls();
  updateManualControls();
}

const helpPage = byId("page-help");
const helpSearch = byId("help-search");
const helpTopics = Array.from(helpPage.querySelectorAll("[data-help-topic]"), (link) => {
  const id = link.dataset.helpTopic;
  const article = byId("help-" + id);
  return { id, article, link, text: article.textContent.toLocaleLowerCase() };
});

function showHelpTopic(id) {
  const current = helpTopics.find((topic) => topic.id === id) ?? helpTopics[0];
  for (const topic of helpTopics) {
    topic.article.hidden = topic !== current;
    if (topic === current) topic.link.setAttribute("aria-current", "page");
    else topic.link.removeAttribute("aria-current");
  }
  return current.article;
}

helpSearch.addEventListener("input", () => {
  const words = helpSearch.value.trim().toLocaleLowerCase().split(/\s+/).filter(Boolean);
  for (const topic of helpTopics) {
    topic.link.hidden = !words.every((word) => topic.text.includes(word));
  }
  for (const group of helpPage.querySelectorAll(".help-nav-group")) {
    group.hidden = !group.querySelector("a:not([hidden])");
  }
  const count = helpTopics.filter(({ link }) => !link.hidden).length;
  note("help-search-status", !words.length ? "" : count ? "找到 " + count + " 篇文档" : "没有匹配的文档");
});

helpSearch.addEventListener("keydown", (event) => {
  if (event.key !== "Enter" || event.isComposing || !helpSearch.value.trim()) return;
  helpTopics.find(({ link }) => !link.hidden)?.link.click();
});

function navigate() {
  if (["#parameters/connections", "#devices"].includes(location.hash)) history.replaceState(null, "", "#parameters");
  const [page, topic] = location.hash.slice(1).split("/");
  const active = byId("page-" + page) ? page : "spectrum";
  for (const link of document.querySelectorAll("[data-page-link]")) {
    const selected = link.dataset.pageLink === active;
    byId("page-" + link.dataset.pageLink).hidden = !selected;
    link.setAttribute("aria-current", selected ? "page" : "false");
  }
  updateBannerVisibility();
  if (active === "spectrum") drawSpectrum();
  if (active === "parameters") updateDevices();
  byId("demo-controls").hidden = active === "help";

  const article = active === "help" ? showHelpTopic(topic) : null;
  const section = topic && (article ?? (active === "parameters" ? byId(topic + "-parameters") : null));
  if (!section || !byId("page-" + active).contains(section)) {
    window.scrollTo(0, 0);
    return;
  }
  section.tabIndex = -1;
  section.focus({ preventScroll: true });
  const top = window.scrollY + section.getBoundingClientRect().top - byId("app-header").offsetHeight - 12;
  window.scrollTo(0, top);
}

function sampleFrame() {
  return channels.map(({ start, end, peaks }) => {
    const points = [];
    const intervals = Math.ceil((end - start) * 4);
    for (let index = 0; index <= intervals; index++) {
      const wavelength = Number(Math.min(start + index / 4, end).toFixed(2));
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
  const left = 62;
  const right = width - 10;
  const top = 32;
  const bottom = height - 42;
  const style = getComputedStyle(canvas);
  const color = (variable) => style.getPropertyValue(variable).trim();
  const visible = state.visibleSpectrum;
  if (visible && byId("auto-axis").checked && !byId("lock-axis").checked) {
    const peak = Math.max(...visible.flatMap((points) => points.map(([, intensity]) => intensity)));
    state.scale = Math.min(64000, Math.max(1000, peak * 1.05));
  }
  if (!byId("auto-axis").checked && !byId("lock-axis").checked) state.scale = 64000;
  const scale = state.scale;
  const toX = (value) => left + (right - left) * (value - wavelengthMin) / wavelengthSpan;
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
    context.fillText(String(Math.trunc(tick * scale / 4)), left - 7, y);
  }
  context.textAlign = "center";
  const tickCount = width < 470 ? 3 : 6;
  for (let tick = 0; tick <= tickCount; tick++) {
    const wavelength = wavelengthMin + wavelengthSpan * tick / tickCount;
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
    : (state.running ? "采集中" : "已停止") + " · 第 " + state.frames + " 帧 · " + channels.length + " 个通道");
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
    note("indicator-test-note", "已退出试灯预览。");
  }
  if (running && airTrial) {
    airTrial = null;
    note("air-trial-result", "检测已开始，空调试运行结束。");
  }
  if (!running && activeRangeRules && activeRangeRules !== savedRangeRules) {
    note("rangefinder-rule-note", "已保存的参数将在下次检测时使用。", "success");
  }
  activeRangeRules = running ? savedRangeRules : null;
  state.running = running;
  byId("stop-measurement").disabled = !running;
  for (const group of measurementGroups) {
    note(group.id + "-note", running ? "检测中，停止后可修改。" : "", running ? "pending" : "");
  }
  updateDevices();
}

function motorBlockingReason() {
  if (motorScenario.value === "disabled") return "";
  if (!motorConnected()) return "检测头移位电机未连接，停机未确认";
  if (motor.phase === "starting" || motor.phase === "jogging") return "检测头正在移位";
  if (motor.phase === "stopping") return "检测头等待停机回读";
  return "";
}

function deviceBlockingReason() {
  return [detectionScenes[scenario.value].reason, motorBlockingReason()].filter(Boolean).join("；");
}

function blockingReason() {
  return [deviceBlockingReason(), laserTrial && "激光器正在手动测试"].filter(Boolean).join("；");
}

function updateGateState() {
  const reason = blockingReason();
  const warning = detectionScenes[scenario.value].warning;
  const motorBlocked = motorBlockingReason();
  const motorWarning = !motorBlocked && motorScenario.value === "fault" ? "变频器故障，检测头停机已确认" : "";
  const label = reason ? "不可检测" : state.running ? "检测中" : "可检测";
  const detail = [reason, warning, motorWarning].filter(Boolean).join(" · ");
  const fault = reason && (["interlock", "offline"].includes(scenario.value) || motorScenario.value === "offline");
  const level = fault ? "bad" : detail ? "warning" : "ok";
  byId("start-measurement").disabled = state.running || Boolean(reason);
  note("gate-state", label);
  note("gate-detail", detail);
  byId("gate-detail").hidden = !detail;
  byId("gate-dot").className = "dot " + level;
  const systemLight = byId("system-status");
  systemLight.className = "dot " + level;
  const motorCheck = motorBlocked || motorWarning ? "" : motorScenario.value === "disabled"
    ? "检测头移位电机未启用" : "检测头移位电机停机已确认";
  systemLight.dataset.tooltip = [label, detail, motorCheck].filter(Boolean).join(" · ");
  systemLight.setAttribute("aria-label", systemLight.dataset.tooltip);
}

function updateBannerVisibility() {
  const banner = byId("measurement-message");
  banner.hidden = byId("page-spectrum").hidden && !banner.dataset.tone;
}

function setMeasurementBanner(message = defaultBanner, tone = "") {
  note("measurement-message", message, tone);
  updateBannerVisibility();
}

byId("start-measurement").addEventListener("click", () => {
  const reason = blockingReason();
  if (reason) {
    setMeasurementBanner(reason, "error");
    return;
  }
  state.frames = 0;
  state.failures = 0;
  setRunning(true);
  setMeasurementBanner();
  measure();
});
byId("stop-measurement").addEventListener("click", () => {
  setRunning(false);
  setMeasurementBanner();
  updateCapture();
});
scenario.addEventListener("change", () => {
  if (!motorConnected() && motor.phase !== "stopped") {
    resetMotor();
    showMotorResult("连接中断，停机未确认", "error");
  }
  applyScene();
});
motorScenario.addEventListener("change", () => {
  showMotorResult("");
  if (!motorConnected()) resetMotor();
  else if (motor.phase !== "stopped" && (motorScenario.value === "fault" ||
      motorScenario.value === motor.direction + "-limit")) stopMotor();
  applyScene();
});

function applyScene() {
  const deviceBlock = deviceBlockingReason();
  if (laserTrial && deviceBlock) finishLaserTrial("手动出光已中止：" + deviceBlock, "error");
  if (airTrial && scenario.value === "offline") {
    airTrial = null;
    note("air-trial-result", "空调离线，模拟试运行已结束。", "error");
  }
  const reason = blockingReason();
  if (state.running && reason) {
    setRunning(false);
    setMeasurementBanner("测量因故障结束：" + reason, "pending");
    updateCapture();
  } else {
    updateDevices();
  }
}
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
const appHeader = byId("app-header");
const syncHelpOffset = () => helpPage.style.setProperty("--app-header-height", appHeader.offsetHeight + "px");
new ResizeObserver(syncHelpOffset).observe(appHeader);
syncHelpOffset();
const updateClock = () => {
  const now = new Date();
  byId("clock").textContent = displayDate.format(now) + " " + displayTime.format(now);
};
setInterval(updateClock, 1000);
setInterval(measure, 650);
setInterval(() => {
  if (byId("page-parameters").hidden) return;
  state.samples++;
  updateDevices();
}, 2500);
updateClock();
updateDevices();
navigate();
