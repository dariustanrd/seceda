use seceda_server::{default_models, ServerConfig};

fn main() {
    let mut args = std::env::args().skip(1);
    match args.next().as_deref() {
        Some("serve") => print_serve_plan(),
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
    println!("  seceda serve    Show the default server bind address");
    println!("  seceda models   List scaffold model aliases");
}

fn print_serve_plan() {
    let config = ServerConfig::default();
    println!(
        "Seceda server scaffold would listen on {}",
        config.listen_addr()
    );
}

fn print_models() {
    for model in default_models() {
        println!("{}\t{:?}", model.id, model.backend);
    }
}
