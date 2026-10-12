use libs_backend::{config::SiteConfig, ipc, runtime::Runtime};
use std::path::PathBuf;

fn main() {
    if let Err((code, message)) = run() {
        eprintln!("{message}");
        std::process::exit(code);
    }
}
fn run() -> std::result::Result<(), (i32, String)> {
    let mut args = std::env::args().skip(1);
    let mut config = None;
    let mut data = PathBuf::from("data");
    let mut pipe = r"\\.\pipe\libs-businessd".to_owned();
    let mut devices = true;
    let mut template = false;
    let mut algorithm = None;
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--config" => {
                config = Some(PathBuf::from(
                    args.next().ok_or((2, "命令 --config 缺少参数值".into()))?,
                ))
            }
            "--data-dir" => {
                data = PathBuf::from(
                    args.next()
                        .ok_or((2, "命令 --data-dir 缺少参数值".into()))?,
                )
            }
            "--pipe" => pipe = args.next().ok_or((2, "命令 --pipe 缺少参数值".into()))?,
            "--algorithm-dir" => {
                algorithm = Some(PathBuf::from(
                    args.next()
                        .ok_or((2, "命令 --algorithm-dir 缺少参数值".into()))?,
                ))
            }
            "--no-devices" => devices = false,
            "--print-default-config" => template = true,
            "-h" | "--help" => {
                println!(
                    "businessd [--config <file>] [--data-dir <dir>] [--pipe <name>] [--algorithm-dir <dir>] [--no-devices]\nbusinessd --print-default-config"
                );
                return Ok(());
            }
            _ => return Err((2, format!("不认识参数：{arg}"))),
        }
    }
    if template {
        println!(
            "{}",
            serde_json::to_string_pretty(&SiteConfig::template()).unwrap()
        );
        return Ok(());
    }
    let site = match config {
        Some(path) => SiteConfig::load(&path).map_err(|e| (1, format!("参数错误：{e}")))?,
        None => SiteConfig::zero(),
    };
    let algorithm_dir = algorithm
        .or_else(|| std::env::var_os("LIBS_ALGORITHM_DIR").map(PathBuf::from))
        .unwrap_or_else(|| {
            let bin = std::env::current_exe()
                .unwrap_or_default()
                .parent()
                .unwrap_or(std::path::Path::new("."))
                .to_path_buf();
            if bin.join("algorithms").is_dir() {
                bin.join("algorithms")
            } else if bin.parent().is_some_and(|p| p.join("algorithms").is_dir()) {
                bin.parent().unwrap().join("algorithms")
            } else {
                bin.ancestors()
                    .map(|p| p.join("src/algorithms"))
                    .find(|p| p.is_dir())
                    .unwrap_or_else(|| PathBuf::from("algorithms"))
            }
        });
    let tokio = tokio::runtime::Builder::new_multi_thread()
        .worker_threads(2)
        .enable_all()
        .build()
        .map_err(|e| (1, e.to_string()))?;
    let runtime =
        Runtime::start(site, data, devices, algorithm_dir).map_err(|e| (1, e.to_string()))?;
    let result = tokio.block_on(ipc::serve(runtime.clone(), &pipe));
    runtime.stop();
    result.map_err(|e| (1, format!("命名管道失败：{e}")))
}
