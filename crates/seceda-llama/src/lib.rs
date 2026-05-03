//! llama.cpp runtime integration scaffold.

use seceda_core::{
    BackendKind, ChatRequest, ChatRole, FinishReason, RuntimeAdapter, RuntimeCapabilities,
    RuntimeError, RuntimeOutput, RuntimeStreamEvent,
};
use serde_json::{json, Value};
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
pub struct LlamaRuntimeAdapter {
    endpoint: String,
    timeout: Duration,
    capabilities: RuntimeCapabilities,
}

impl LlamaRuntimeAdapter {
    pub fn new(endpoint: impl Into<String>) -> Self {
        let mut capabilities = RuntimeCapabilities::new("local/llama.cpp", BackendKind::Local);
        capabilities.models = vec!["local/default".to_string()];
        capabilities.supports_streaming = true;
        capabilities.max_context_tokens = Some(4096);

        Self {
            endpoint: endpoint.into(),
            timeout: Duration::from_secs(30),
            capabilities,
        }
    }

    pub fn with_timeout(mut self, timeout: Duration) -> Self {
        self.timeout = timeout;
        self
    }

    fn post_chat_completions(&self, body: Value) -> Result<String, RuntimeError> {
        let parsed = ParsedHttpEndpoint::parse(&self.endpoint).ok_or_else(|| {
            RuntimeError::new("llama.cpp endpoint must be an http://host:port URL")
        })?;
        let addr = parsed
            .socket_addr()
            .ok_or_else(|| RuntimeError::new("llama.cpp endpoint host could not be resolved"))?;
        let mut stream = TcpStream::connect_timeout(&addr, self.timeout).map_err(|error| {
            RuntimeError::new(format!("failed to connect to llama.cpp: {error}"))
        })?;
        let _ = stream.set_read_timeout(Some(self.timeout));
        let body = body.to_string();
        let request = format!(
            "POST /v1/chat/completions HTTP/1.1\r\nHost: {}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
            parsed.host_header,
            body.len(),
            body
        );
        stream.write_all(request.as_bytes()).map_err(|error| {
            RuntimeError::new(format!("failed to send llama.cpp request: {error}"))
        })?;

        let mut response = String::new();
        stream.read_to_string(&mut response).map_err(|error| {
            RuntimeError::new(format!("failed to read llama.cpp response: {error}"))
        })?;

        let status = parse_status_code(&response).unwrap_or(0);
        if !(200..=299).contains(&status) {
            return Err(RuntimeError::new(format!(
                "llama.cpp chat completions request failed with HTTP {status}"
            )));
        }

        response_body(&response)
    }
}

impl RuntimeAdapter for LlamaRuntimeAdapter {
    fn capabilities(&self) -> RuntimeCapabilities {
        self.capabilities.clone()
    }

    fn execute(&self, request: &ChatRequest) -> Result<RuntimeOutput, RuntimeError> {
        let body = chat_completions_body(request, false);
        let response = self.post_chat_completions(body)?;
        parse_chat_completion_response(&response)
    }

    fn execute_stream(
        &self,
        request: &ChatRequest,
    ) -> Result<Vec<RuntimeStreamEvent>, RuntimeError> {
        let body = chat_completions_body(request, true);
        let response = self.post_chat_completions(body)?;
        parse_chat_completion_stream(&response, &request.model)
    }
}

fn chat_completions_body(request: &ChatRequest, stream: bool) -> Value {
    let messages: Vec<Value> = request
        .messages
        .iter()
        .map(|message| {
            json!({
                "role": match message.role {
                    ChatRole::System => "system",
                    ChatRole::User => "user",
                    ChatRole::Assistant => "assistant",
                    ChatRole::Tool => "tool",
                },
                "content": message.content
            })
        })
        .collect();

    json!({
        "model": request.model,
        "messages": messages,
        "stream": stream
    })
}

fn parse_chat_completion_response(body: &str) -> Result<RuntimeOutput, RuntimeError> {
    let value: Value = serde_json::from_str(body)
        .map_err(|error| RuntimeError::new(format!("invalid llama.cpp JSON response: {error}")))?;
    let text = value["choices"][0]["message"]["content"]
        .as_str()
        .ok_or_else(|| {
            RuntimeError::new("llama.cpp response missing choices[0].message.content")
        })?;
    let model = value["model"].as_str().unwrap_or("local/default");
    Ok(RuntimeOutput {
        text: text.to_string(),
        model: model.to_string(),
        finish_reason: parse_finish_reason(value["choices"][0]["finish_reason"].as_str()),
    })
}

fn parse_chat_completion_stream(
    body: &str,
    fallback_model: &str,
) -> Result<Vec<RuntimeStreamEvent>, RuntimeError> {
    let mut events = Vec::new();
    let mut text = String::new();
    let mut model = fallback_model.to_string();

    for line in body.lines() {
        let Some(data) = line.strip_prefix("data: ") else {
            continue;
        };
        if data.trim() == "[DONE]" {
            break;
        }
        let value: Value = serde_json::from_str(data).map_err(|error| {
            RuntimeError::new(format!("invalid llama.cpp streaming JSON chunk: {error}"))
        })?;
        if let Some(chunk_model) = value["model"].as_str() {
            model = chunk_model.to_string();
        }
        if events.is_empty() {
            events.push(RuntimeStreamEvent::Started {
                model: model.clone(),
            });
        }
        if let Some(delta) = value["choices"][0]["delta"]["content"].as_str() {
            text.push_str(delta);
            events.push(RuntimeStreamEvent::TextDelta {
                text: delta.to_string(),
            });
        }
        if let Some(finish_reason) = value["choices"][0]["finish_reason"].as_str() {
            events.push(RuntimeStreamEvent::Completed {
                output: RuntimeOutput {
                    text: text.clone(),
                    model: model.clone(),
                    finish_reason: parse_finish_reason(Some(finish_reason)),
                },
            });
        }
    }

    if events.is_empty() {
        events.push(RuntimeStreamEvent::Failed {
            message: "llama.cpp stream did not contain any chunks".to_string(),
        });
    } else if !matches!(events.last(), Some(RuntimeStreamEvent::Completed { .. })) {
        events.push(RuntimeStreamEvent::Completed {
            output: RuntimeOutput {
                text,
                model,
                finish_reason: FinishReason::Stop,
            },
        });
    }

    Ok(events)
}

fn parse_finish_reason(reason: Option<&str>) -> FinishReason {
    match reason {
        Some("length") => FinishReason::Length,
        _ => FinishReason::Stop,
    }
}

fn response_body(response: &str) -> Result<String, RuntimeError> {
    let (headers, body) = response
        .split_once("\r\n\r\n")
        .ok_or_else(|| RuntimeError::new("llama.cpp response was malformed"))?;

    if headers.lines().any(|line| {
        line.to_ascii_lowercase()
            .starts_with("transfer-encoding: chunked")
    }) {
        return decode_chunked_body(body);
    }

    Ok(body.to_string())
}

fn decode_chunked_body(mut body: &str) -> Result<String, RuntimeError> {
    let mut decoded = String::new();

    loop {
        let (size_line, rest) = body
            .split_once("\r\n")
            .ok_or_else(|| RuntimeError::new("malformed chunked llama.cpp response"))?;
        let size_hex = size_line.split(';').next().unwrap_or(size_line).trim();
        let size = usize::from_str_radix(size_hex, 16)
            .map_err(|_| RuntimeError::new("invalid chunk size in llama.cpp response"))?;
        if size == 0 {
            break;
        }
        if rest.len() < size + 2 {
            return Err(RuntimeError::new("truncated chunked llama.cpp response"));
        }
        decoded.push_str(&rest[..size]);
        body = &rest[size + 2..];
    }

    Ok(decoded)
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
    use seceda_core::ChatMessage;
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

    #[test]
    fn adapter_posts_non_streaming_chat_completions_request() {
        let endpoint = spawn_fake_chat_completion_server(
            |request| {
                assert!(request.starts_with("POST /v1/chat/completions HTTP/1.1"));
                assert!(request.contains(r#""stream":false"#));
                assert!(request.contains(r#""role":"system""#));
                assert!(request.contains(r#""content":"be brief""#));
                assert!(request.contains(r#""role":"user""#));
                assert!(request.contains(r#""content":"hello""#));
            },
            r#"{"model":"llama-test","choices":[{"message":{"content":"adapter text"},"finish_reason":"stop"}]}"#,
        );
        let adapter = LlamaRuntimeAdapter::new(endpoint).with_timeout(Duration::from_secs(1));
        let request = ChatRequest::new(
            "seceda/default",
            vec![ChatMessage::system("be brief"), ChatMessage::user("hello")],
        );

        let output = adapter.execute(&request).expect("llama execution");

        assert_eq!(output.text, "adapter text");
        assert_eq!(output.model, "llama-test");
        assert_eq!(output.finish_reason, FinishReason::Stop);
    }

    #[test]
    fn adapter_parses_streaming_chat_completion_chunks() {
        let endpoint = spawn_fake_chat_completion_server(
            |request| {
                assert!(request.contains(r#""stream":true"#));
            },
            concat!(
                "data: {\"model\":\"llama-test\",\"choices\":[{\"delta\":{\"content\":\"one\"},\"finish_reason\":null}]}\n",
                "data: {\"model\":\"llama-test\",\"choices\":[{\"delta\":{\"content\":\" two\"},\"finish_reason\":\"stop\"}]}\n",
                "data: [DONE]\n\n"
            ),
        );
        let adapter = LlamaRuntimeAdapter::new(endpoint).with_timeout(Duration::from_secs(1));
        let request = ChatRequest::new("seceda/default", vec![ChatMessage::user("hello")]);

        let events = adapter.execute_stream(&request).expect("llama stream");

        assert_eq!(
            events,
            vec![
                RuntimeStreamEvent::Started {
                    model: "llama-test".to_string()
                },
                RuntimeStreamEvent::TextDelta {
                    text: "one".to_string()
                },
                RuntimeStreamEvent::TextDelta {
                    text: " two".to_string()
                },
                RuntimeStreamEvent::Completed {
                    output: RuntimeOutput {
                        text: "one two".to_string(),
                        model: "llama-test".to_string(),
                        finish_reason: FinishReason::Stop
                    }
                }
            ]
        );
    }

    #[test]
    fn adapter_decodes_chunked_streaming_chat_completion_response() {
        let chunk = "data: {\"model\":\"llama-test\",\"choices\":[{\"delta\":{\"content\":\"chunk\"},\"finish_reason\":\"stop\"}]}\n\n";
        let endpoint = spawn_fake_chat_completion_server_with_headers(
            |_| {},
            "Transfer-Encoding: chunked\r\n",
            format!("{:x}\r\n{}\r\n0\r\n\r\n", chunk.len(), chunk),
        );
        let adapter = LlamaRuntimeAdapter::new(endpoint).with_timeout(Duration::from_secs(1));
        let request = ChatRequest::new("seceda/default", vec![ChatMessage::user("hello")]);

        let events = adapter.execute_stream(&request).expect("llama stream");

        assert!(events.contains(&RuntimeStreamEvent::TextDelta {
            text: "chunk".to_string()
        }));
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

    fn spawn_fake_chat_completion_server(
        assert_request: impl FnOnce(&str) + Send + 'static,
        response_body: &'static str,
    ) -> String {
        spawn_fake_chat_completion_server_with_headers(
            assert_request,
            &format!("Content-Length: {}\r\n", response_body.len()),
            response_body.to_string(),
        )
    }

    fn spawn_fake_chat_completion_server_with_headers(
        assert_request: impl FnOnce(&str) + Send + 'static,
        headers: &str,
        response_body: String,
    ) -> String {
        let listener = TcpListener::bind("127.0.0.1:0").expect("listener");
        let addr = listener.local_addr().expect("local addr");
        let headers = headers.to_string();
        thread::spawn(move || {
            let (mut stream, _) = listener.accept().expect("accept");
            let mut buffer = [0; 8192];
            let bytes = stream.read(&mut buffer).expect("read");
            let request = String::from_utf8_lossy(&buffer[..bytes]).to_string();
            assert_request(&request);
            let response = format!(
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n{}Connection: close\r\n\r\n{}",
                headers, response_body
            );
            stream.write_all(response.as_bytes()).expect("write");
        });
        format!("http://{addr}/health")
    }
}
