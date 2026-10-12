use crate::error::{Error, Result};
use serde::Deserialize;
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use std::{
    collections::{BTreeMap, BTreeSet, VecDeque},
    path::{Path, PathBuf},
    sync::atomic::{AtomicBool, Ordering},
    time::Instant,
};

const TARGETS: [&str; 10] = [
    "TFe", "SiO2", "Al2O3", "TiO2", "CaO", "MgO", "MnO", "P", "S", "FeO",
];
const DEFINITION: &str = "FeO equivalent of ferrous iron in oxide/silicate phases, excluding pyrite iron; included in TFe";

#[derive(Deserialize)]
struct NativePackage {
    format: String,
    manifest: Value,
    heads: BTreeMap<String, HeadMeta>,
    asset_sha256: BTreeMap<String, String>,
}
#[derive(Deserialize)]
struct HeadMeta {
    file: String,
    targets: Vec<String>,
    log_targets: bool,
    offsets: Vec<f64>,
    domain: Option<Domain>,
}
#[derive(Deserialize)]
struct Domain {
    mean: Vec<f64>,
    scale: Vec<f64>,
    references: Vec<Vec<f64>>,
    threshold: f64,
}
struct Tree {
    left: Vec<i32>,
    right: Vec<i32>,
    feature: Vec<i32>,
    threshold: Vec<f64>,
    values: Vec<f64>,
    outputs: usize,
}
struct Head {
    meta: HeadMeta,
    trees: Vec<Tree>,
}
struct FeaturePrediction {
    values: [f64; 10],
    before: f64,
    domain: Option<(f64, f64)>,
}
#[derive(Deserialize)]
struct Window {
    wavelength: f64,
    element: String,
}
pub struct Model {
    directory: PathBuf,
    manifest: Value,
    windows: Vec<Window>,
    heads: BTreeMap<String, Head>,
    wave: Vec<f64>,
    crop: bool,
}
impl Model {
    pub fn load(algorithm_dir: &Path) -> Result<Self> {
        let directory = if let Some(path) = std::env::var_os("LIBS_MODEL_DIR") {
            PathBuf::from(path)
        } else {
            let local = algorithm_dir.join("runtime.local.json");
            let path = if local.exists() {
                local
            } else {
                algorithm_dir.join("runtime.json")
            };
            let config: Value = serde_json::from_slice(&std::fs::read(&path).map_err(|_| {
                Error::new(
                    "unsupported",
                    "model_file_missing",
                    format!("无法读取模型运行配置/响应: {}", path.display()),
                )
            })?)
            .map_err(|e| {
                Error::new("protocol", "model_bad_json", format!("模型 JSON 无效: {e}"))
            })?;
            if let Some(timeout) = config.get("timeout_ms") {
                let v = timeout.as_f64().unwrap_or(0.);
                if !(1000.0..=120000.).contains(&v) || v.fract() != 0. {
                    return Err(Error::invalid(
                        "model_timeout_invalid",
                        "模型 timeout_ms 须为 1000..120000 整数",
                    ));
                }
            }
            let folder = PathBuf::from(
                config["model_dir"]
                    .as_str()
                    .unwrap_or("iron_ore/model/current"),
            );
            if folder.is_absolute() {
                folder
            } else {
                algorithm_dir.join(folder)
            }
        };
        Self::load_directory(&directory)
    }
    pub fn load_directory(directory: &Path) -> Result<Self> {
        let bad = |message: String| {
            Error::new(
                "unsupported",
                "model_load_failed",
                format!("模型加载失败: {message}"),
            )
        };
        let bytes=std::fs::read(directory.join("native_model.json")).map_err(|_|Error::new("unsupported","model_runtime_missing","请安装完整模型包，并运行 backend/tools/export_model.py 生成 Rust 模型；后端运行不需要 Python"))?;
        let package: NativePackage =
            serde_json::from_slice(&bytes).map_err(|e| bad(e.to_string()))?;
        if package.format != "libs.extratrees.v1" {
            return Err(bad("unsupported native model format".into()));
        }
        for (name, hash) in &package.asset_sha256 {
            if Path::new(name).components().count() != 1 {
                return Err(bad("invalid model asset path".into()));
            }
            let bytes = std::fs::read(directory.join(name)).map_err(|e| bad(e.to_string()))?;
            if format!("{:x}", Sha256::digest(bytes)) != *hash {
                return Err(bad(format!("Model asset hash mismatch: {name}")));
            }
        }
        let windows: Vec<Window> = csv::Reader::from_path(directory.join("feature_windows.csv"))
            .map_err(|e| bad(e.to_string()))?
            .deserialize()
            .collect::<std::result::Result<_, _>>()
            .map_err(|e| bad(e.to_string()))?;
        let count = windows.len() * 2;
        if package.manifest["n_features"].as_u64() != Some(count as u64) {
            return Err(bad(
                "Model feature manifest does not match its windows".into()
            ));
        }
        let band = &package.manifest["wavelength_range_nm"];
        let low = band[0].as_f64().unwrap_or(200.);
        let high = band[1].as_f64().unwrap_or(600.);
        let step = package.manifest["wavelength_step_nm"]
            .as_f64()
            .unwrap_or(0.05);
        if !low.is_finite()
            || !high.is_finite()
            || !step.is_finite()
            || low >= high
            || step <= 0.
            || (high - low) / step > 1000000.
        {
            return Err(bad("Invalid wavelength grid".into()));
        }
        let wave: Vec<_> = (0..=((high - low) / step).round() as usize)
            .map(|i| ((low + i as f64 * step) * 100000.).round() / 100000.)
            .collect();
        let crop = package.manifest["format"] == "current_mixed_v2"
            && low == 200.
            && high == 565.
            && count == 240;
        if !["current_mixed_v1", "current_mixed_v2"]
            .contains(&package.manifest["format"].as_str().unwrap_or(""))
        {
            return Err(bad("Unsupported model format".into()));
        }
        if windows.iter().any(|w| {
            !w.wavelength.is_finite()
                || w.wavelength - 0.65 < low
                || w.wavelength + 0.65 > if crop { 564.7 } else { high }
        }) {
            return Err(bad(
                "Model windows extend outside its wavelength range".into()
            ));
        }
        let mut heads = BTreeMap::new();
        for (name, meta) in package.heads {
            if Path::new(&meta.file).components().count() != 1 {
                return Err(bad("invalid tree asset path".into()));
            }
            let trees = load_trees(&directory.join(&meta.file), count, meta.targets.len())
                .map_err(|e| bad(e.to_string()))?;
            if meta.offsets.len() != meta.targets.len()
                || meta.targets.iter().any(|t| !TARGETS.contains(&t.as_str()))
            {
                return Err(bad("invalid model targets or offsets".into()));
            }
            if let Some(d) = &meta.domain {
                if d.mean.len() != count
                    || d.scale.len() != count
                    || d.scale.iter().any(|v| !v.is_finite() || *v <= 0.)
                    || d.references.is_empty()
                    || d.references
                        .iter()
                        .any(|r| r.len() != count || r.iter().any(|v| !v.is_finite()))
                {
                    return Err(bad("invalid observed-domain metadata".into()));
                }
            }
            heads.insert(name, Head { meta, trees });
        }
        for name in [
            "oxide_main",
            "oxide_added",
            "sulfide_main",
            "sulfide_added",
            "FeO_mixed",
        ] {
            if !heads.contains_key(name) {
                return Err(bad(format!("missing model head: {name}")));
            }
        }
        Ok(Self {
            directory: directory.to_path_buf(),
            manifest: package.manifest,
            windows,
            heads,
            wave,
            crop,
        })
    }
    pub fn status(&self) -> Value {
        json!({"availability":"available","implemented":true,"model_id":format!("iron_ore_{}",self.manifest["format"].as_str().unwrap_or("")),"model_format":self.manifest["format"],
            "model_dir":self.directory,"targets":TARGETS,"unit":self.manifest["unit"],"branches":["oxide","sulfide"],"required_wavelength_nm":[self.wave[0],self.wave[self.wave.len()-1]],
            "minimum_accepted_wavelength_nm":[self.wave[0],if self.crop{564.7}else{self.wave[self.wave.len()-1]}],"feature_count":self.windows.len()*2,"asset_sha256":self.manifest["asset_sha256"],
            "boundary_policy":if self.crop{"full_grid_or_fixed_564_7_nm_crop"}else{"full_grid_only"},"validated_for_quantification":false,"inference_network_required":false,"phosphorus_definition":"elemental_P; no automatic P2O5 conversion"})
    }
    pub fn analyze(&self, input: &Value, cancel: &AtomicBool) -> Result<Value> {
        let start = Instant::now();
        crate::prediction::validate(input)?;
        let bad = |s: &str| Error::invalid("model_prediction_failed", s);
        let branch = input["parameters"]["branch"].as_str().unwrap_or("");
        if !["oxide", "sulfide"].contains(&branch) {
            return Err(bad(
                "input.parameters.branch must explicitly be oxide or sulfide",
            ));
        }
        if input["parameters"]
            .as_object()
            .is_some_and(|o| o.keys().any(|k| k != "branch"))
        {
            return Err(bad(
                "Only parameters.branch is supported; preprocessing is fixed by the model",
            ));
        }
        let model_id = format!(
            "iron_ore_{}",
            self.manifest["format"].as_str().unwrap_or("")
        );
        if input["model"]
            .as_object()
            .is_some_and(|o| o.keys().any(|k| k != "id"))
            || input["model"].get("id").is_some_and(|v| v != &model_id)
        {
            return Err(bad(&format!("Unsupported model selection; use {model_id}")));
        }
        let spectrum = if input.get("spectrum").is_some() {
            &input["spectrum"]
        } else {
            &input["raw_capture"]
        };
        let groups = spectrum["scan_groups"].as_array().unwrap();
        if groups.len() > 128 {
            return Err(bad("Expected 1..128 scan groups"));
        }
        let mut rows = vec![];
        let mut vectors = vec![];
        for (index, group) in groups.iter().enumerate() {
            if cancel.load(Ordering::Acquire) {
                return Err(Error::new("internal", "model_cancelled", "模型任务已取消"));
            }
            let mut row = json!({"group_index":index,"shot_id":group["shot_id"],"branch":branch,"availability":"unavailable","composition":composition(None),"hardware_synchronized":group["hardware_synchronized"],"issues":[]});
            if group["complete"] == false {
                row["status"] = "incomplete_scan_group".into();
                rows.push(row);
                continue;
            }
            if group["hardware_synchronized"] != true {
                row["issues"]
                    .as_array_mut()
                    .unwrap()
                    .push("hardware_synchronization_not_confirmed".into());
            }
            match self.prepare_group(&group["channels"], &mut row) {
                Err(message) => {
                    row["status"] = "ineligible_spectrum".into();
                    row["detail"] = message.into();
                }
                Ok(None) => (),
                Ok(Some(x)) => {
                    let FeaturePrediction {
                        values,
                        before,
                        domain,
                    } = self.predict(&x, branch, cancel)?;
                    if values.iter().any(|v| !v.is_finite()) {
                        row["status"] = "nonfinite_model_output".into();
                        rows.push(row);
                        continue;
                    }
                    row["status"] = "research_prediction".into();
                    row["availability"] = "available".into();
                    row["composition"] = composition(Some(&values));
                    row["unphysical_output"] = values.iter().any(|v| *v < 0. || *v > 100.).into();
                    row["iron_pool_violation"] = (values[9] < 0.
                        || values[9] * 55.845 / (55.845 + 15.999)
                            > values[0] - values[8] * 55.845 / (2. * 32.06) + 1e-8)
                        .into();
                    row["outside_observed_domain"] =
                        domain.map(|(d, t)| json!(d > t)).unwrap_or(Value::Null);
                    row["ood_distance"] = domain.map(|(d, _)| json!(d)).unwrap_or(Value::Null);
                    row["validated_for_quantification"] = false.into();
                    row["FeO_iron_already_in_TFe"] = true.into();
                    if branch == "oxide" {
                        row["FeO_before_mixed_training"] = before.into();
                    }
                    for key in [
                        "unphysical_output",
                        "iron_pool_violation",
                        "outside_observed_domain",
                    ] {
                        if row[key] == true {
                            row["issues"].as_array_mut().unwrap().push(key.into());
                        }
                    }
                    vectors.push(values);
                }
            }
            rows.push(row);
        }
        let mut issues = BTreeSet::new();
        for row in &rows {
            for issue in row["issues"].as_array().unwrap() {
                issues.insert(issue.as_str().unwrap());
            }
        }
        let mut issues: Vec<_> = issues.into_iter().map(String::from).collect();
        if vectors.len() < rows.len() {
            issues.push("some_or_all_groups_ineligible".into());
        }
        let mut mean = [0.; 10];
        if !vectors.is_empty() {
            for vector in &vectors {
                for (sum, v) in mean.iter_mut().zip(vector) {
                    *sum += v;
                }
            }
            for v in &mut mean {
                *v /= vectors.len() as f64;
            }
        }
        let effective: Vec<_> = rows
            .iter()
            .filter_map(|r| r.get("metadata").map(|m| m["effective_grid_nm"].clone()))
            .collect();
        let available = !vectors.is_empty();
        Ok(
            json!({"schema_version":"libs.algorithm.result.v1","request_id":input["request_id"],"availability":if available{"available"}else{"unavailable"},"executed":available,
            "status":{"state":if available{"completed"}else{"not_executed"},"reason":if available{"research_prediction"}else{"no_eligible_spectrum"}},"composition":composition(if available{Some(&mean)}else{None}),
            "results":rows,"aggregation":{"method":"mean_of_valid_group_predictions","valid_groups":vectors.len(),"total_groups":groups.len()},"classification":{"label":null,"confidence":null,"classes":[]},"similarity":{"score":null,"reference_id":null,"matches":[]},
            "quality":{"status":if !issues.is_empty()||!available{"review_required"}else{"research_prediction"},"issues":issues},"context":input.get("context").cloned().unwrap_or(json!({})),"model":self.status(),"branch":branch,"validated_for_quantification":false,
            "FeO_definition":DEFINITION,"FeO_iron_already_in_TFe":true,"parameters_applied":true,"preprocessing":{"grid_nm":[self.wave[0],self.wave[self.wave.len()-1],self.manifest["wavelength_step_nm"].as_f64().unwrap_or(0.05)],"effective_grids_nm":effective,"feature_count":self.windows.len()*2,"extrapolation":false,
            "boundary_policy":self.status()["boundary_policy"],"channel_join":"sort_and_average_exact_duplicate_wavelengths","max_wavelength_gap_nm":1.0,"channel_gain_correction":false},"inference_ms":(start.elapsed().as_secs_f64()*1e6).round()/1000.}),
        )
    }
    fn prepare_group(
        &self,
        channels: &Value,
        row: &mut Value,
    ) -> std::result::Result<Option<Vec<f64>>, String> {
        let channels = channels.as_array().ok_or("No captured channels")?;
        let mut points = vec![];
        for channel in channels {
            let waves = channel["wavelength_nm"]
                .as_array()
                .ok_or("Expected matching wavelength_nm and counts arrays of at least 3 points")?;
            let counts = channel["counts"]
                .as_array()
                .ok_or("Expected matching wavelength_nm and counts arrays of at least 3 points")?;
            if waves.len() < 3 || waves.len() != counts.len() {
                return Err(
                    "Expected matching wavelength_nm and counts arrays of at least 3 points".into(),
                );
            }
            let mut last = f64::NEG_INFINITY;
            for (w, c) in waves.iter().zip(counts) {
                let w = w.as_f64().unwrap();
                if w <= last {
                    return Err("nonmonotonic_wavelength".into());
                }
                last = w;
                points.push((w, c.as_f64().unwrap()));
            }
        }
        if points.is_empty() {
            return Err("No captured channels".into());
        }
        points.sort_by(|a, b| a.0.total_cmp(&b.0));
        let mut data = vec![];
        let mut i = 0;
        while i < points.len() {
            let wave = points[i].0;
            let mut sum = 0.;
            let mut n = 0;
            while i < points.len() && points[i].0 == wave {
                sum += points[i].1;
                n += 1;
                i += 1;
            }
            data.push((wave, sum / n as f64));
        }
        let high = *self.wave.last().unwrap();
        let minimum = if self.crop { 564.7 } else { high };
        if data[0].0 > self.wave[0] || data.last().unwrap().0 < minimum {
            return Err("insufficient_model_wavelength_coverage".into());
        }
        if data
            .windows(2)
            .any(|p| p[0].0 < high && p[1].0 > self.wave[0] && p[1].0 - p[0].0 > 1.)
        {
            return Err("spectral_gap_exceeds_1_nm".into());
        }
        let adapted = data.last().unwrap().0 < high;
        let wave = &self.wave[..if adapted {
            self.wave.partition_point(|v| *v <= 564.7)
        } else {
            self.wave.len()
        }];
        let mut signal = Vec::with_capacity(wave.len());
        let mut j = 0;
        for &w in wave {
            while j + 1 < data.len() && data[j + 1].0 < w {
                j += 1;
            }
            let (x0, y0) = data[j];
            let (x1, y1) = data[(j + 1).min(data.len() - 1)];
            signal.push(if x1 == x0 {
                y0
            } else {
                y0 + (w - x0) * (y1 - y0) / (x1 - x0)
            });
        }
        let mut difference: Vec<_> = signal.windows(2).map(|p| (p[1] - p[0]).abs()).collect();
        difference.sort_by(f64::total_cmp);
        let noise = quantile_sorted(&difference, 0.5) / 0.953872 + 1e-12;
        let mut sorted = signal.clone();
        sorted.sort_by(f64::total_cmp);
        let contrast = quantile_sorted(&sorted, 0.999) - quantile_sorted(&sorted, 0.25);
        let snr = contrast / noise;
        row["metadata"] = json!({"n_points":data.len(),"wavelength_min_nm":data[0].0,"wavelength_max_nm":data.last().unwrap().0,"max_wavelength_gap_nm":data.windows(2).map(|p|p[1].0-p[0].0).fold(0.,f64::max),
            "required_wavelength_min_nm":wave[0],"required_wavelength_max_nm":wave[wave.len()-1],"snr_proxy":snr,"model_wavelength_range_nm":[self.wave[0],high],"effective_grid_nm":[wave[0],wave[wave.len()-1],wave[1]-wave[0]],"boundary_adapted":adapted});
        if adapted {
            row["issues"]
                .as_array_mut()
                .unwrap()
                .push("model_boundary_adapted".into());
        }
        if contrast <= 1e-10 || snr < 8. {
            row["status"] = "blank_or_low_snr".into();
            return Ok(None);
        }
        row["status"] = "feature_eligible".into();
        let baseline = gaussian(&minimum_filter(&signal, 81), 12.);
        for (v, b) in signal.iter_mut().zip(baseline) {
            *v = (*v - b).max(0.);
        }
        let mut areas = vec![];
        for width in [0.30, 0.65] {
            for window in &self.windows {
                let begin = wave.partition_point(|v| {
                    *v < window.wavelength && (*v - window.wavelength).abs() > width
                });
                let end = wave.partition_point(|v| {
                    *v <= window.wavelength || (*v - window.wavelength).abs() <= width
                });
                let mut area = 0.;
                for k in begin..end.saturating_sub(1) {
                    area += (signal[k] + signal[k + 1]) * 0.5 * (wave[k + 1] - wave[k]);
                }
                areas.push(area.max(0.));
            }
        }
        let denominator = areas
            .iter()
            .enumerate()
            .filter(|(i, _)| {
                ["Fe", "Si", "Al", "Ti"]
                    .contains(&self.windows[*i % self.windows.len()].element.as_str())
            })
            .map(|(_, v)| *v)
            .sum::<f64>()
            .max(1e-30);
        Ok(Some(
            areas
                .iter()
                .map(|v| (v / denominator + 1e-6).log10())
                .collect(),
        ))
    }
    fn predict(&self, x: &[f64], branch: &str, cancel: &AtomicBool) -> Result<FeaturePrediction> {
        let mut values = [f64::NAN; 10];
        let mut domain = None;
        for name in ["main", "added"] {
            let head = &self.heads[&format!("{branch}_{name}")];
            let predicted = head.predict(x, cancel)?;
            for (i, target) in head.meta.targets.iter().enumerate() {
                let j = TARGETS.iter().position(|t| *t == target).unwrap();
                values[j] = if head.meta.log_targets {
                    10f64.powf(predicted[i]) - head.meta.offsets[i]
                } else {
                    predicted[i]
                };
            }
            if name == "main" {
                if let Some(d) = &head.meta.domain {
                    let scaled: Vec<_> = x
                        .iter()
                        .zip(&d.mean)
                        .zip(&d.scale)
                        .map(|((x, m), s)| (x - m) / s)
                        .collect();
                    let distance = d
                        .references
                        .iter()
                        .map(|r| {
                            scaled
                                .iter()
                                .zip(r)
                                .map(|(a, b)| (a - b).powi(2))
                                .sum::<f64>()
                                / x.len() as f64
                        })
                        .map(f64::sqrt)
                        .fold(f64::INFINITY, f64::min);
                    domain = Some((distance, d.threshold));
                }
            }
        }
        let before = values[9];
        if branch == "oxide" {
            values[9] = self.heads["FeO_mixed"].predict(x, cancel)?[0];
        }
        Ok(FeaturePrediction {
            values,
            before,
            domain,
        })
    }
}
impl Head {
    fn predict(&self, x: &[f64], cancel: &AtomicBool) -> Result<Vec<f64>> {
        let mut sum = vec![0.; self.meta.targets.len()];
        for tree in &self.trees {
            if cancel.load(Ordering::Acquire) {
                return Err(Error::new("internal", "model_cancelled", "模型任务已取消"));
            }
            let mut node = 0;
            while tree.left[node] != -1 {
                node = if (x[tree.feature[node] as usize] as f32) as f64 <= tree.threshold[node] {
                    tree.left[node]
                } else {
                    tree.right[node]
                } as usize;
            }
            for (j, v) in sum.iter_mut().enumerate() {
                *v += tree.values[node * tree.outputs + j];
            }
        }
        for value in &mut sum {
            *value /= self.trees.len() as f64;
        }
        Ok(sum)
    }
}
fn load_trees(path: &Path, features: usize, outputs: usize) -> std::io::Result<Vec<Tree>> {
    use std::io::{Error, ErrorKind};
    let bytes = std::fs::read(path)?;
    let mut cursor = 0;
    fn take<'a>(bytes: &'a [u8], cursor: &mut usize, size: usize) -> std::io::Result<&'a [u8]> {
        let end = cursor
            .checked_add(size)
            .filter(|v| *v <= bytes.len())
            .ok_or_else(|| {
                std::io::Error::new(std::io::ErrorKind::InvalidData, "truncated forest")
            })?;
        let result = &bytes[*cursor..end];
        *cursor = end;
        Ok(result)
    }
    fn uint(bytes: &[u8], cursor: &mut usize) -> std::io::Result<u32> {
        Ok(u32::from_le_bytes(
            take(bytes, cursor, 4)?.try_into().unwrap(),
        ))
    }
    if take(&bytes, &mut cursor, 8)? != b"LIBSTRE1" {
        return Err(Error::new(ErrorKind::InvalidData, "invalid forest magic"));
    }
    let n = uint(&bytes, &mut cursor)?;
    if n == 0 || n > 10000 {
        return Err(Error::new(ErrorKind::InvalidData, "invalid forest size"));
    }
    let mut trees = vec![];
    for _ in 0..n {
        let nodes = uint(&bytes, &mut cursor)? as usize;
        let out = uint(&bytes, &mut cursor)? as usize;
        if nodes == 0 || nodes > 10000000 || out != outputs {
            return Err(Error::new(ErrorKind::InvalidData, "invalid tree shape"));
        }
        let mut ints = || -> std::io::Result<Vec<i32>> {
            Ok(take(&bytes, &mut cursor, nodes * 4)?
                .chunks_exact(4)
                .map(|p| i32::from_le_bytes(p.try_into().unwrap()))
                .collect())
        };
        let left = ints()?;
        let right = ints()?;
        let feature = ints()?;
        let threshold: Vec<_> = take(&bytes, &mut cursor, nodes * 8)?
            .chunks_exact(8)
            .map(|p| f64::from_le_bytes(p.try_into().unwrap()))
            .collect();
        let values: Vec<_> = take(&bytes, &mut cursor, nodes * out * 8)?
            .chunks_exact(8)
            .map(|p| f64::from_le_bytes(p.try_into().unwrap()))
            .collect();
        for i in 0..nodes {
            // Forward-only children make cycles impossible, including corrupt package data.
            if left[i] != -1
                && (left[i] <= i as i32
                    || right[i] <= i as i32
                    || left[i] as usize >= nodes
                    || right[i] as usize >= nodes
                    || feature[i] < 0
                    || feature[i] as usize >= features)
            {
                return Err(Error::new(ErrorKind::InvalidData, "invalid tree node"));
            }
        }
        if threshold.iter().chain(&values).any(|v| !v.is_finite()) {
            return Err(Error::new(ErrorKind::InvalidData, "nonfinite forest value"));
        }
        trees.push(Tree {
            left,
            right,
            feature,
            threshold,
            values,
            outputs: out,
        });
    }
    if cursor != bytes.len() {
        return Err(Error::new(ErrorKind::InvalidData, "trailing forest bytes"));
    }
    Ok(trees)
}
fn composition(vector: Option<&[f64; 10]>) -> Value {
    let mut values = json!({});
    let mut targets = json!({});
    for component in [
        "AL2O3", "CAO", "FEO", "MGO", "MNO", "P2O5", "S", "SIO2", "TFE", "TIO2",
    ] {
        values[format!("COMPO_{component}")] = Value::Null;
    }
    if let Some(v) = vector {
        for (target, value) in TARGETS.iter().zip(v) {
            targets[*target] = json!(value);
            if *target != "P" {
                values[format!("COMPO_{}", target.to_uppercase())] = json!(value);
            }
        }
    }
    json!({"availability":if vector.is_some(){"available"}else{"unavailable"},"unit":"mass_fraction_percent","unit_confirmed":true,"values":values,"target_values":targets,"phosphorus_definition":"elemental_P; COMPO_P2O5 is not predicted"})
}
fn quantile_sorted(x: &[f64], p: f64) -> f64 {
    let position = (x.len() - 1) as f64 * p;
    let index = position.floor() as usize;
    x[index] + (x[(index + 1).min(x.len() - 1)] - x[index]) * position.fract()
}
fn minimum_filter(x: &[f64], size: usize) -> Vec<f64> {
    let radius = size / 2;
    let mut queue: VecDeque<(usize, f64)> = VecDeque::new();
    let mut out = Vec::with_capacity(x.len());
    for i in 0..x.len() + radius * 2 {
        let value = x[i.saturating_sub(radius).min(x.len() - 1)];
        while queue.back().is_some_and(|(_, v)| *v >= value) {
            queue.pop_back();
        }
        queue.push_back((i, value));
        while queue.front().is_some_and(|(j, _)| *j + size <= i) {
            queue.pop_front();
        }
        if i + 1 >= size {
            out.push(queue.front().unwrap().1);
        }
    }
    out
}
fn gaussian(x: &[f64], sigma: f64) -> Vec<f64> {
    let radius = (4. * sigma + 0.5).floor() as isize;
    let mut kernel: Vec<_> = (-radius..=radius)
        .map(|i| (-0.5 * (i as f64 / sigma).powi(2)).exp())
        .collect();
    let total = kernel.iter().sum::<f64>();
    for v in &mut kernel {
        *v /= total;
    }
    (0..x.len())
        .map(|i| {
            kernel
                .iter()
                .enumerate()
                .map(|(k, w)| {
                    let mut j = i as isize + k as isize - radius;
                    while j < 0 || j >= x.len() as isize {
                        j = if j < 0 {
                            -j - 1
                        } else {
                            2 * x.len() as isize - j - 1
                        };
                    }
                    w * x[j as usize]
                })
                .sum()
        })
        .collect()
}
