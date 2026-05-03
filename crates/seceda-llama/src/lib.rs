//! llama.cpp runtime integration scaffold.

use seceda_core::{BackendKind, RuntimeCapabilities};
use std::env;
use std::io::{Read, Write};
use std::net::{SocketAddr, TcpStream, ToSocketAddrs};
use std::path::{Path, PathBuf};
use std::time::Duration;

/// Configuration for a future llama.cpp-backed local runtime.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LlamaRuntimeConfig {
    pub model_path: String,
    pub context_size: usize,
    pub binary_name: String,
    pub sidecar_endpoint: String,
}

impl LlamaRuntimeConfig {
    pub fn new(model_path: impl Into<String>) -> Self {
        Self {
            model_path: model_path.into(),
            context_size: 4096,
            binary_name: "llama-server".to_string(),
            sidecar_endpoint: "http://127.0.0.1:8081".to_string(),
        }
    }

    pub fn backend_kind(&self) -> BackendKind {
        BackendKind::Local
    }

    pub fn capabilities(&self, health: SidecarHealth) -> RuntimeCapabilities {
        let mut capabilities = RuntimeCapabilities::new("local/llama.cpp", BackendKind::Local);
        capabilities.models = vec!["local/default".to_string()];
        capabilities.supports_streaming = matches!(health, SidecarHealth::Healthy);
        capabilities.max_context_tokens = Some(self.context_size);
        capabilities
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LlamaDiscovery {
    pub binary_name: String,
    pub binary_path: Option<PathBuf>,
    pub status: DiscoveryStatus,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DiscoveryStatus {
    Found,
    Missing,
}

pub fn discover_llama_server() -> LlamaDiscovery {
    let path = env::var_os("PATH").unwrap_or_default();
    discover_llama_server_in_path("llama-server", &path.to_string_lossy())
}

pub fn discover_llama_server_in_path(binary_name: &str, path_var: &str) -> LlamaDiscovery {
    let binary_path = path_var
        .split(if cfg!(windows) { ';' } else { ':' })
        .filter(|entry| !entry.is_empty())
        .map(|entry| Path::new(entry).join(binary_name))
        .find(|candidate| candidate.is_file());

    LlamaDiscovery {
        binary_name: binary_name.to_string(),
        status: if binary_path.is_some() {
            DiscoveryStatus::Found
        } else {
            DiscoveryStatus::Missing
        },
        binary_path,
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SidecarHealthReport {
    pub endpoint: String,
    pub health: SidecarHealth,
    pub status_code: Option<u16>,
    pub message: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SidecarHealth {
    Healthy,
    Unhealthy,
    Unreachable,
    InvalidEndpoint,
}

pub fn check_sidecar_health(endpoint: &str) -> SidecarHealthReport {
    check_sidecar_health_with_timeout(endpoint, Duration::from_secs(2))
}

pub fn check_sidecar_health_with_timeout(endpoint: &str, timeout: Duration) -> SidecarHealthReport {
    let parsed = match ParsedHttpEndpoint::parse(endpoint) {
        Some(parsed) => parsed,
        None => {
            return SidecarHealthReport {
                endpoint: endpoint.to_string(),
                health: SidecarHealth::InvalidEndpoint,
                status_code: None,
                message: "endpoint must be an http://host:port URL".to_string(),
            };
        }
    };

    let addr = match parsed.socket_addr() {
        Some(addr) => addr,
        None => {
            return SidecarHealthReport {
                endpoint: endpoint.to_string(),
                health: SidecarHealth::InvalidEndpoint,
                status_code: None,
                message: "endpoint host could not be resolved".to_string(),
            };
        }
    };

    match TcpStream::connect_timeout(&addr, timeout) {
        Ok(mut stream) => {
            let _ = stream.set_read_timeout(Some(timeout));
            let request = format!(
                "GET {} HTTP/1.1\r\nHost: {}\r\nConnection: close\r\n\r\n",
                parsed.health_path, parsed.host_header
            );
            if let Err(error) = stream.write_all(request.as_bytes()) {
                return SidecarHealthReport {
                    endpoint: endpoint.to_string(),
                    health: SidecarHealth::Unreachable,
                    status_code: None,
                    message: error.to_string(),
                };
            }

            let mut response = String::new();
            if let Err(error) = stream.read_to_string(&mut response) {
                return SidecarHealthReport {
                    endpoint: endpoint.to_string(),
                    health: SidecarHealth::Unreachable,
                    status_code: None,
                    message: error.to_string(),
                };
            }

            let status_code = parse_status_code(&response);
            let healthy = matches!(status_code, Some(200..=299));
            SidecarHealthReport {
                endpoint: endpoint.to_string(),
                health: if healthy {
                    SidecarHealth::Healthy
                } else {
                    SidecarHealth::Unhealthy
                },
                status_code,
                message: if healthy {
                    "llama.cpp sidecar is healthy".to_string()
                } else {
                    "llama.cpp sidecar returned an unhealthy status".to_string()
                },
            }
        }
        Err(error) => SidecarHealthReport {
            endpoint: endpoint.to_string(),
            health: SidecarHealth::Unreachable,
            status_code: None,
            message: error.to_string(),
        },
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct ParsedHttpEndpoint {
    host: String,
    port: u16,
    health_path: String,
    host_header: String,
}

impl ParsedHttpEndpoint {
    fn parse(endpoint: &str) -> Option<Self> {
        let rest = endpoint.strip_prefix("http://")?;
        let authority = rest.split('/').next().unwrap_or(rest);
        let health_path = if rest[authority.len()..].is_empty() {
            "/health".to_string()
        } else {
            rest[authority.len()..].to_string()
        };
        let (host, port) = authority.rsplit_once(':')?;
        let port = port.parse().ok()?;
        Some(Self {
            host: host.to_string(),
            port,
            health_path,
            host_header: authority.to_string(),
        })
    }

    fn socket_addr(&self) -> Option<SocketAddr> {
        (self.host.as_str(), self.port)
            .to_socket_addrs()
            .ok()?
            .next()
    }
}

fn parse_status_code(response: &str) -> Option<u16> {
    response
        .lines()
        .next()?
        .split_whitespace()
        .nth(1)?
        .parse()
        .ok()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::io::{Read, Write};
    use std::net::TcpListener;
    use std::thread;

    #[test]
    fn llama_runtime_is_local_backend() {
        let config = LlamaRuntimeConfig::new("models/local.gguf");

        assert_eq!(config.backend_kind(), BackendKind::Local);
        assert_eq!(config.context_size, 4096);
    }

    #[test]
    fn discovers_missing_llama_server() {
        let discovery = discover_llama_server_in_path("llama-server", "");

        assert_eq!(discovery.status, DiscoveryStatus::Missing);
        assert_eq!(discovery.binary_path, None);
    }

    #[test]
    fn discovers_llama_server_in_path() {
        let temp_dir = env::temp_dir().join(format!("seceda-llama-test-{}", std::process::id()));
        fs::create_dir_all(&temp_dir).expect("create temp dir");
        let binary = temp_dir.join("llama-server");
        fs::write(&binary, "").expect("write fake binary");

        let discovery = discover_llama_server_in_path("llama-server", &temp_dir.to_string_lossy());

        assert_eq!(discovery.status, DiscoveryStatus::Found);
        assert_eq!(discovery.binary_path, Some(binary));
        let _ = fs::remove_dir_all(temp_dir);
    }

    #[test]
    fn reports_healthy_sidecar_endpoint() {
        let endpoint = spawn_fake_health_server(200);

        let report = check_sidecar_health_with_timeout(&endpoint, Duration::from_secs(1));

        assert_eq!(report.health, SidecarHealth::Healthy);
        assert_eq!(report.status_code, Some(200));
    }

    #[test]
    fn reports_unhealthy_sidecar_endpoint() {
        let endpoint = spawn_fake_health_server(503);

        let report = check_sidecar_health_with_timeout(&endpoint, Duration::from_secs(1));

        assert_eq!(report.health, SidecarHealth::Unhealthy);
        assert_eq!(report.status_code, Some(503));
    }

    #[test]
    fn reports_invalid_sidecar_endpoint() {
        let report = check_sidecar_health("https://127.0.0.1:8081");

        assert_eq!(report.health, SidecarHealth::InvalidEndpoint);
        assert_eq!(report.status_code, None);
    }

    #[test]
    fn exposes_runtime_capabilities_for_core() {
        let config = LlamaRuntimeConfig::new("models/local.gguf");

        let capabilities = config.capabilities(SidecarHealth::Healthy);

        assert_eq!(capabilities.runtime_id, "local/llama.cpp");
        assert_eq!(capabilities.backend, BackendKind::Local);
        assert_eq!(capabilities.models, vec!["local/default"]);
        assert!(capabilities.supports_streaming);
        assert_eq!(capabilities.max_context_tokens, Some(4096));
    }

    fn spawn_fake_health_server(status: u16) -> String {
        let listener = TcpListener::bind("127.0.0.1:0").expect("listener");
        let addr = listener.local_addr().expect("local addr");
        thread::spawn(move || {
            let (mut stream, _) = listener.accept().expect("accept");
            let mut buffer = [0; 512];
            let _ = stream.read(&mut buffer);
            let status_text = if status == 200 { "OK" } else { "Unavailable" };
            let response = format!(
                "HTTP/1.1 {status} {status_text}\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok"
            );
            stream.write_all(response.as_bytes()).expect("write");
        });
        format!("http://{addr}/health")
    }
}
