use libs_backend::model::Model;
use std::io::{BufRead, Write};
use std::sync::atomic::AtomicBool;

fn main() {
    let path = std::env::args_os().nth(1).expect("model directory");
    let model = Model::load_directory(std::path::Path::new(&path)).unwrap();
    let mut stdout = std::io::stdout().lock();
    for line in std::io::stdin().lock().lines() {
        let input = serde_json::from_str(&line.unwrap()).unwrap();
        let result = model.analyze(&input, &AtomicBool::new(false)).unwrap();
        writeln!(stdout, "{result}").unwrap();
        stdout.flush().unwrap();
    }
}
