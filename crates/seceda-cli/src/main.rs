use seceda_server::{default_models, run_headless, ServerConfig};

fn main() {
    let mut args = std::env::args().skip(1);
    match args.next().as_deref() {
        Some("server") | Some("serve") => run_server(),
        Some("models") => print_models(),
        Some("-h") | Some("--help") | None => print_help(),
        Some(command) => {
            eprintln!("unknown command: {command}");
            print_help();
            std::process::exit(2);
        }
    }
}

fn print_help() {
    println!("Seceda Rust CLI scaffold");
    println!();
    println!("Usage:");
    println!("  seceda server   Start the headless server");
    println!("  seceda models   List scaffold model aliases");
}

fn run_server() {
    let config = ServerConfig::default();
    println!("Seceda server listening on {}", config.listen_addr());
    if let Err(error) = run_headless(config) {
        eprintln!("server failed: {error}");
        std::process::exit(1);
    }
}

fn print_models() {
    for model in default_models() {
        println!("{}\t{:?}", model.id, model.backend);
    }
}
