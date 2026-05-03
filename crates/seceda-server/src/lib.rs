//! Server boundary for Seceda's OpenAI-compatible localhost API.

use seceda_core::{
    execute_with_adapters, BackendKind, ChatMessage, ChatRequest, MockRuntimeAdapter,
    ModelDescriptor, RequestFeatures, RuntimeAdapter, SecedaConfig,
};
use seceda_llama::{check_sidecar_health_with_timeout, SidecarHealth};
use serde_json::{json, Value};
use std::cell::RefCell;
use std::error::Error;
use std::fmt;
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::process::{Child, Command};
use std::time::Duration;

/// Minimal server configuration used by the CLI and future HTTP listener.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ServerConfig {
    pub bind_host: String,
    pub port: u16,
    pub llama_sidecar: LlamaSidecarConfig,
}

impl Default for ServerConfig {
    fn default() -> Self {
        Self {
            bind_host: "127.0.0.1".to_string(),
            port: 8080,
            llama_sidecar: LlamaSidecarConfig::default(),
        }
    }
}

impl ServerConfig {
    pub fn listen_addr(&self) -> String {
        format!("{}:{}", self.bind_host, self.port)
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LlamaSidecarConfig {
    pub launch_policy: LlamaSidecarLaunchPolicy,
    pub command: String,
    pub args: Vec<String>,
    pub model_path: String,
    pub host: String,
    pub port: u16,
    pub health_path: String,
}

impl Default for LlamaSidecarConfig {
    fn default() -> Self {
        Self {
            launch_policy: LlamaSidecarLaunchPolicy::Never,
            command: "llama-server".to_string(),
            args: Vec::new(),
            model_path: String::new(),
            host: "127.0.0.1".to_string(),
            port: 8081,
            health_path: "/health".to_string(),
        }
    }
}

impl LlamaSidecarConfig {
    pub fn endpoint(&self) -> String {
        format!("http://{}:{}{}", self.host, self.port, self.health_path)
    }

    fn command_args(&self) -> Vec<String> {
        if !self.args.is_empty() {
            return self.args.clone();
        }

        let mut args = vec![
            "--host".to_string(),
            self.host.clone(),
            "--port".to_string(),
            self.port.to_string(),
        ];
        if !self.model_path.is_empty() {
            args.push("--model".to_string());
            args.push(self.model_path.clone());
        }
        args
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LlamaSidecarLaunchPolicy {
    Never,
    IfMissing,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LlamaSidecarStatus {
    pub state: LlamaSidecarState,
    pub endpoint: String,
    pub pid: Option<u32>,
    pub message: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LlamaSidecarState {
    NotConfigured,
    AlreadyRunning,
    Launched,
    FailedLaunch,
    Stopped,
}

impl LlamaSidecarState {
    fn as_str(self) -> &'static str {
        match self {
            LlamaSidecarState::NotConfigured => "not-configured",
            LlamaSidecarState::AlreadyRunning => "already-running",
            LlamaSidecarState::Launched => "launched",
            LlamaSidecarState::FailedLaunch => "failed-launch",
            LlamaSidecarState::Stopped => "stopped",
        }
    }
}

#[derive(Debug)]
struct LlamaSidecarSupervisor {
    config: LlamaSidecarConfig,
    child: RefCell<Option<Child>>,
    status: RefCell<LlamaSidecarStatus>,
}

impl LlamaSidecarSupervisor {
    fn new(config: LlamaSidecarConfig) -> Self {
        let endpoint = config.endpoint();
        Self {
            config,
            child: RefCell::new(None),
            status: RefCell::new(LlamaSidecarStatus {
                state: LlamaSidecarState::NotConfigured,
                endpoint,
                pid: None,
                message: "llama.cpp sidecar has not been checked".to_string(),
            }),
        }
    }

    fn ensure_started(&self) -> LlamaSidecarStatus {
        if self.config.model_path.trim().is_empty() {
            return self.update(
                LlamaSidecarState::NotConfigured,
                None,
                "model path is not configured",
            );
        }

        if let Some(status) = self.refresh_child_status() {
            return status;
        }

        let endpoint = self.config.endpoint();
        let health = check_sidecar_health_with_timeout(&endpoint, Duration::from_millis(100));
        if health.health == SidecarHealth::Healthy {
            return self.update(
                LlamaSidecarState::AlreadyRunning,
                None,
                "llama.cpp sidecar is already running",
            );
        }

        if self.config.launch_policy == LlamaSidecarLaunchPolicy::Never {
            return self.update(
                LlamaSidecarState::Stopped,
                None,
                "llama.cpp sidecar is not running and launch policy is never",
            );
        }

        match Command::new(&self.config.command)
            .args(self.config.command_args())
            .spawn()
        {
            Ok(child) => {
                let pid = child.id();
                *self.child.borrow_mut() = Some(child);
                self.update(
                    LlamaSidecarState::Launched,
                    Some(pid),
                    "llama.cpp sidecar process launched",
                )
            }
            Err(error) => self.update(
                LlamaSidecarState::FailedLaunch,
                None,
                format!("failed to launch llama.cpp sidecar: {error}"),
            ),
        }
    }

    fn status(&self) -> LlamaSidecarStatus {
        self.refresh_child_status()
            .unwrap_or_else(|| self.status.borrow().clone())
    }

    fn refresh_child_status(&self) -> Option<LlamaSidecarStatus> {
        let mut child = self.child.borrow_mut();
        let running_pid = child.as_ref().map(Child::id);
        if let Some(process) = child.as_mut() {
            match process.try_wait() {
                Ok(Some(status)) => {
                    *child = None;
                    return Some(self.update(
                        LlamaSidecarState::Stopped,
                        running_pid,
                        format!("llama.cpp sidecar exited with {status}"),
                    ));
                }
                Ok(None) => {
                    return Some(self.update(
                        LlamaSidecarState::Launched,
                        running_pid,
                        "llama.cpp sidecar process is running",
                    ));
                }
                Err(error) => {
                    return Some(self.update(
                        LlamaSidecarState::FailedLaunch,
                        running_pid,
                        format!("failed to inspect llama.cpp sidecar: {error}"),
                    ));
                }
            }
        }
        None
    }

    fn update(
        &self,
        state: LlamaSidecarState,
        pid: Option<u32>,
        message: impl Into<String>,
    ) -> LlamaSidecarStatus {
        let status = LlamaSidecarStatus {
            state,
            endpoint: self.config.endpoint(),
            pid,
            message: message.into(),
        };
        *self.status.borrow_mut() = status.clone();
        status
    }
}

impl Drop for LlamaSidecarSupervisor {
    fn drop(&mut self) {
        if let Some(mut child) = self.child.borrow_mut().take() {
            let _ = child.kill();
            let _ = child.wait();
        }
    }
}

/// Static model aliases exposed by the scaffold until runtime discovery lands.
pub fn default_models() -> Vec<ModelDescriptor> {
    vec![
        ModelDescriptor::new("seceda/default", BackendKind::Local),
        ModelDescriptor::new("local/default", BackendKind::Local),
        ModelDescriptor::new("remote/default", BackendKind::Cloud),
    ]
}

#[derive(Debug)]
pub enum ServerError {
    Io(std::io::Error),
    Json(serde_json::Error),
    BadRequest(String),
}

impl fmt::Display for ServerError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            ServerError::Io(error) => write!(formatter, "{error}"),
            ServerError::Json(error) => write!(formatter, "{error}"),
            ServerError::BadRequest(message) => formatter.write_str(message),
        }
    }
}

impl Error for ServerError {}

impl From<std::io::Error> for ServerError {
    fn from(error: std::io::Error) -> Self {
        ServerError::Io(error)
    }
}

impl From<serde_json::Error> for ServerError {
    fn from(error: serde_json::Error) -> Self {
        ServerError::Json(error)
    }
}

#[derive(Debug)]
pub struct ServerState {
    pub core_config: SecedaConfig,
    sidecar: LlamaSidecarSupervisor,
    traces: RefCell<Vec<ObservableEvent>>,
}

impl Default for ServerState {
    fn default() -> Self {
        Self {
            core_config: SecedaConfig::default(),
            sidecar: LlamaSidecarSupervisor::new(LlamaSidecarConfig::default()),
            traces: RefCell::new(Vec::new()),
        }
    }
}

impl ServerState {
    pub fn new(config: ServerConfig) -> Self {
        Self {
            core_config: SecedaConfig::default(),
            sidecar: LlamaSidecarSupervisor::new(config.llama_sidecar),
            traces: RefCell::new(Vec::new()),
        }
    }

    pub fn observed_events(&self) -> Vec<ObservableEvent> {
        self.traces.borrow().clone()
    }

    pub fn ensure_llama_sidecar(&self) -> LlamaSidecarStatus {
        self.sidecar.ensure_started()
    }

    pub fn llama_sidecar_status(&self) -> LlamaSidecarStatus {
        self.sidecar.status()
    }

    fn record(&self, event: ObservableEvent) {
        self.traces.borrow_mut().push(event);
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ObservableEvent {
    pub request_id: String,
    pub kind: ObservableEventKind,
    pub message: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ObservableEventKind {
    RequestNormalized,
    RoutingDecided,
    RuntimeSelected,
    StreamDelta,
    Completed,
    Error,
}

/// Start the minimal headless server. This call blocks until the listener fails.
pub fn run_headless(config: ServerConfig) -> Result<(), ServerError> {
    let listener = TcpListener::bind(config.listen_addr())?;
    let state = ServerState::new(config);
    state.ensure_llama_sidecar();
    serve_listener(listener, state)
}

pub fn serve_listener(listener: TcpListener, state: ServerState) -> Result<(), ServerError> {
    for stream in listener.incoming() {
        serve_stream(stream?, &state)?;
    }
    Ok(())
}

pub fn serve_one(listener: TcpListener, state: &ServerState) -> Result<(), ServerError> {
    let (stream, _) = listener.accept()?;
    serve_stream(stream, state)
}

fn serve_stream(mut stream: TcpStream, state: &ServerState) -> Result<(), ServerError> {
    let mut buffer = [0; 16 * 1024];
    let bytes_read = stream.read(&mut buffer)?;
    let request = String::from_utf8_lossy(&buffer[..bytes_read]);
    let response = handle_http_request(&request, state);

    stream.write_all(response.as_bytes())?;
    stream.flush()?;
    Ok(())
}

pub fn handle_http_request(raw: &str, state: &ServerState) -> String {
    match route_http_request(raw, state) {
        Ok(response) => response.to_http(),
        Err(error) => HttpResponse::json(
            error.status,
            json!({
                "error": {
                    "message": error.message,
                    "type": error.error_type,
                    "code": error.code
                }
            }),
        )
        .to_http(),
    }
}

fn route_http_request(raw: &str, state: &ServerState) -> Result<HttpResponse, OpenAiError> {
    let request = HttpRequest::parse(raw)?;

    if request.path == "/health" || request.path == "/admin/llama-sidecar" {
        if request.method != "GET" {
            return Err(OpenAiError::new(
                405,
                "invalid_request_error",
                "method_not_allowed",
                "GET is required for health/admin routes",
            ));
        }
        return Ok(HttpResponse::json(200, health_json(state)));
    }

    if request.path != "/v1/responses" {
        return Err(OpenAiError::new(
            404,
            "invalid_request_error",
            "not_found",
            "unsupported path",
        ));
    }

    if request.method != "POST" {
        return Err(OpenAiError::new(
            405,
            "invalid_request_error",
            "method_not_allowed",
            "POST is required for /v1/responses",
        ));
    }

    let body: Value = serde_json::from_str(&request.body).map_err(|_| {
        OpenAiError::new(
            400,
            "invalid_request_error",
            "invalid_json",
            "request body must be valid JSON",
        )
    })?;
    let stream = body.get("stream").and_then(Value::as_bool).unwrap_or(false);
    let core_request = responses_request_to_core(&body)?;
    let local = MockRuntimeAdapter::new("local/llama.cpp", BackendKind::Local);
    let cloud = MockRuntimeAdapter::new("remote/modal-default", BackendKind::Cloud);
    let adapters: [&dyn RuntimeAdapter; 2] = [&local, &cloud];
    let result =
        execute_with_adapters(&state.core_config, &core_request, &adapters).map_err(|error| {
            OpenAiError::new(
                500,
                "server_error",
                "core_execution_failed",
                error.to_string(),
            )
        })?;
    record_trace_events(state, RESPONSE_ID, &result);

    if stream {
        return Ok(HttpResponse::sse(
            200,
            response_stream(RESPONSE_ID, &result),
        ));
    }

    Ok(HttpResponse::json(200, response_json(RESPONSE_ID, &result)))
}

const RESPONSE_ID: &str = "resp_mock_0000000000000000";

fn health_json(state: &ServerState) -> Value {
    let sidecar = state.llama_sidecar_status();
    json!({
        "status": "ok",
        "llama_sidecar": {
            "state": sidecar.state.as_str(),
            "endpoint": sidecar.endpoint,
            "pid": sidecar.pid,
            "message": sidecar.message
        }
    })
}

fn response_json(id: &str, result: &seceda_core::ExecutionResult) -> Value {
    json!({
        "id": id,
        "object": "response",
        "model": result.output.model,
        "output": [
            {
                "type": "message",
                "id": "msg_mock_0000000000000000",
                "role": "assistant",
                "content": [
                    {
                        "type": "output_text",
                        "text": result.output.text
                    }
                ]
            }
        ],
        "usage": {
            "input_tokens": result.routing.estimated_prompt_tokens,
            "output_tokens": 0,
            "total_tokens": result.routing.estimated_prompt_tokens
        }
    })
}

fn response_stream(id: &str, result: &seceda_core::ExecutionResult) -> Vec<SseEvent> {
    vec![
        SseEvent::new(
            "response.created",
            json!({
                "type": "response.created",
                "response": {
                    "id": id,
                    "object": "response",
                    "model": result.output.model,
                    "status": "in_progress"
                }
            }),
        ),
        SseEvent::new(
            "response.output_item.added",
            json!({
                "type": "response.output_item.added",
                "output_index": 0,
                "item": {
                    "id": "msg_mock_0000000000000000",
                    "type": "message",
                    "role": "assistant",
                    "status": "in_progress",
                    "content": []
                }
            }),
        ),
        SseEvent::new(
            "response.content_part.added",
            json!({
                "type": "response.content_part.added",
                "item_id": "msg_mock_0000000000000000",
                "output_index": 0,
                "content_index": 0,
                "part": {
                    "type": "output_text",
                    "text": ""
                }
            }),
        ),
        SseEvent::new(
            "response.output_text.delta",
            json!({
                "type": "response.output_text.delta",
                "item_id": "msg_mock_0000000000000000",
                "output_index": 0,
                "content_index": 0,
                "delta": result.output.text
            }),
        ),
        SseEvent::new(
            "response.output_text.done",
            json!({
                "type": "response.output_text.done",
                "item_id": "msg_mock_0000000000000000",
                "output_index": 0,
                "content_index": 0,
                "text": result.output.text
            }),
        ),
        SseEvent::new(
            "response.content_part.done",
            json!({
                "type": "response.content_part.done",
                "item_id": "msg_mock_0000000000000000",
                "output_index": 0,
                "content_index": 0,
                "part": {
                    "type": "output_text",
                    "text": result.output.text
                }
            }),
        ),
        SseEvent::new(
            "response.output_item.done",
            json!({
                "type": "response.output_item.done",
                "output_index": 0,
                "item": {
                    "id": "msg_mock_0000000000000000",
                    "type": "message",
                    "role": "assistant",
                    "status": "completed",
                    "content": [
                        {
                            "type": "output_text",
                            "text": result.output.text
                        }
                    ]
                }
            }),
        ),
        SseEvent::new(
            "response.completed",
            json!({
                "type": "response.completed",
                "response": response_json(id, result)
            }),
        ),
    ]
}

fn record_trace_events(
    state: &ServerState,
    request_id: &str,
    result: &seceda_core::ExecutionResult,
) {
    state.record(ObservableEvent {
        request_id: request_id.to_string(),
        kind: ObservableEventKind::RequestNormalized,
        message: "responses request normalized".to_string(),
    });
    state.record(ObservableEvent {
        request_id: request_id.to_string(),
        kind: ObservableEventKind::RoutingDecided,
        message: format!(
            "target={:?} reason={:?} matched_rules={}",
            result.routing.target,
            result.routing.reason,
            result.routing.matched_rules.join(",")
        ),
    });
    state.record(ObservableEvent {
        request_id: request_id.to_string(),
        kind: ObservableEventKind::RuntimeSelected,
        message: format!("runtime={}", result.routing.runtime_id),
    });
    state.record(ObservableEvent {
        request_id: request_id.to_string(),
        kind: ObservableEventKind::StreamDelta,
        message: format!("bytes={}", result.output.text.len()),
    });
    state.record(ObservableEvent {
        request_id: request_id.to_string(),
        kind: ObservableEventKind::Completed,
        message: format!("finish_reason={:?}", result.output.finish_reason),
    });
}

fn responses_request_to_core(body: &Value) -> Result<ChatRequest, OpenAiError> {
    let input = body.get("input").ok_or_else(|| {
        OpenAiError::new(
            400,
            "invalid_request_error",
            "missing_input",
            "minimal tracer requires string field 'input'",
        )
    })?;
    let input = input.as_str().ok_or_else(|| {
        OpenAiError::new(
            400,
            "invalid_request_error",
            "unsupported_input",
            "minimal tracer only supports string input",
        )
    })?;

    let model = body
        .get("model")
        .and_then(Value::as_str)
        .unwrap_or("seceda/default");
    let features = RequestFeatures {
        tools: body.get("tools").is_some(),
        tool_choice: body.get("tool_choice").is_some(),
        structured_output: body.get("response_format").is_some(),
    };

    Ok(ChatRequest::new(model, vec![ChatMessage::user(input)]).with_features(features))
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct HttpRequest {
    method: String,
    path: String,
    body: String,
}

impl HttpRequest {
    fn parse(raw: &str) -> Result<Self, OpenAiError> {
        let (head, body) = raw.split_once("\r\n\r\n").ok_or_else(|| {
            OpenAiError::new(
                400,
                "invalid_request_error",
                "malformed_http",
                "request must contain headers and body",
            )
        })?;
        let request_line = head.lines().next().ok_or_else(|| {
            OpenAiError::new(
                400,
                "invalid_request_error",
                "malformed_http",
                "request line is missing",
            )
        })?;
        let mut parts = request_line.split_whitespace();
        let method = parts.next().unwrap_or_default();
        let path = parts.next().unwrap_or_default();
        if method.is_empty() || path.is_empty() {
            return Err(OpenAiError::new(
                400,
                "invalid_request_error",
                "malformed_http",
                "request line must include method and path",
            ));
        }

        Ok(Self {
            method: method.to_string(),
            path: path.to_string(),
            body: body.to_string(),
        })
    }
}

struct HttpResponse {
    status: u16,
    body: String,
    content_type: &'static str,
}

impl HttpResponse {
    fn json(status: u16, body: Value) -> Self {
        Self {
            status,
            body: body.to_string(),
            content_type: "application/json",
        }
    }

    fn sse(status: u16, events: Vec<SseEvent>) -> Self {
        Self {
            status,
            body: events
                .into_iter()
                .map(|event| event.to_sse())
                .collect::<String>(),
            content_type: "text/event-stream",
        }
    }

    fn to_http(&self) -> String {
        let status_text = match self.status {
            200 => "OK",
            400 => "Bad Request",
            404 => "Not Found",
            405 => "Method Not Allowed",
            500 => "Internal Server Error",
            _ => "Unknown",
        };

        format!(
            "HTTP/1.1 {} {}\r\nContent-Type: {}\r\nCache-Control: no-cache\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
            self.status,
            status_text,
            self.content_type,
            self.body.len(),
            self.body
        )
    }
}

struct SseEvent {
    event: &'static str,
    data: Value,
}

impl SseEvent {
    fn new(event: &'static str, data: Value) -> Self {
        Self { event, data }
    }

    fn to_sse(&self) -> String {
        format!("event: {}\ndata: {}\n\n", self.event, self.data)
    }
}

#[derive(Debug)]
struct OpenAiError {
    status: u16,
    error_type: &'static str,
    code: &'static str,
    message: String,
}

impl OpenAiError {
    fn new(
        status: u16,
        error_type: &'static str,
        code: &'static str,
        message: impl Into<String>,
    ) -> Self {
        Self {
            status,
            error_type,
            code,
            message: message.into(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::Value;
    use std::io::{Read, Write};
    use std::net::{TcpListener, TcpStream};
    use std::thread;

    #[test]
    fn default_config_binds_loopback() {
        assert_eq!(ServerConfig::default().listen_addr(), "127.0.0.1:8080");
    }

    #[test]
    fn server_config_expresses_llama_sidecar_launch_inputs() {
        let config = LlamaSidecarConfig {
            launch_policy: LlamaSidecarLaunchPolicy::IfMissing,
            command: "fake-llama-server".to_string(),
            args: vec!["--threads".to_string(), "4".to_string()],
            model_path: "models/local.gguf".to_string(),
            host: "127.0.0.1".to_string(),
            port: 18081,
            health_path: "/health".to_string(),
        };

        assert_eq!(config.endpoint(), "http://127.0.0.1:18081/health");
        assert_eq!(config.command_args(), vec!["--threads", "4"]);
    }

    #[test]
    fn llama_sidecar_reports_already_running_before_launch() {
        let endpoint = spawn_fake_health_server(200);
        let mut config = llama_config_for_endpoint(&endpoint);
        config.launch_policy = LlamaSidecarLaunchPolicy::IfMissing;
        config.command = "missing-command-that-should-not-run".to_string();
        let state = ServerState::new(ServerConfig {
            llama_sidecar: config,
            ..ServerConfig::default()
        });

        let status = state.ensure_llama_sidecar();

        assert_eq!(status.state, LlamaSidecarState::AlreadyRunning);
        assert_eq!(status.pid, None);
    }

    #[test]
    fn llama_sidecar_launches_when_missing_and_allowed() {
        let config = LlamaSidecarConfig {
            launch_policy: LlamaSidecarLaunchPolicy::IfMissing,
            command: "sh".to_string(),
            args: vec!["-c".to_string(), "sleep 5".to_string()],
            model_path: "models/local.gguf".to_string(),
            host: "127.0.0.1".to_string(),
            port: unused_port(),
            health_path: "/health".to_string(),
        };
        let state = ServerState::new(ServerConfig {
            llama_sidecar: config,
            ..ServerConfig::default()
        });

        let status = state.ensure_llama_sidecar();

        assert_eq!(status.state, LlamaSidecarState::Launched);
        assert!(status.pid.is_some());
    }

    #[test]
    fn llama_sidecar_reports_stopped_after_launched_process_exits() {
        let config = LlamaSidecarConfig {
            launch_policy: LlamaSidecarLaunchPolicy::IfMissing,
            command: "sh".to_string(),
            args: vec!["-c".to_string(), "exit 0".to_string()],
            model_path: "models/local.gguf".to_string(),
            host: "127.0.0.1".to_string(),
            port: unused_port(),
            health_path: "/health".to_string(),
        };
        let state = ServerState::new(ServerConfig {
            llama_sidecar: config,
            ..ServerConfig::default()
        });

        let launched = state.ensure_llama_sidecar();
        let stopped = wait_for_stopped_sidecar(&state);

        assert_eq!(launched.state, LlamaSidecarState::Launched);
        assert_eq!(stopped.state, LlamaSidecarState::Stopped);
        assert!(stopped.message.contains("exited"));
    }

    #[test]
    fn llama_sidecar_reports_failed_launch() {
        let config = LlamaSidecarConfig {
            launch_policy: LlamaSidecarLaunchPolicy::IfMissing,
            command: "missing-seceda-llama-command".to_string(),
            args: Vec::new(),
            model_path: "models/local.gguf".to_string(),
            host: "127.0.0.1".to_string(),
            port: unused_port(),
            health_path: "/health".to_string(),
        };
        let state = ServerState::new(ServerConfig {
            llama_sidecar: config,
            ..ServerConfig::default()
        });

        let status = state.ensure_llama_sidecar();

        assert_eq!(status.state, LlamaSidecarState::FailedLaunch);
        assert!(status.message.contains("failed to launch"));
    }

    #[test]
    fn llama_sidecar_reports_stopped_when_launch_disabled() {
        let config = LlamaSidecarConfig {
            launch_policy: LlamaSidecarLaunchPolicy::Never,
            command: "llama-server".to_string(),
            args: Vec::new(),
            model_path: "models/local.gguf".to_string(),
            host: "127.0.0.1".to_string(),
            port: unused_port(),
            health_path: "/health".to_string(),
        };
        let state = ServerState::new(ServerConfig {
            llama_sidecar: config,
            ..ServerConfig::default()
        });

        let status = state.ensure_llama_sidecar();

        assert_eq!(status.state, LlamaSidecarState::Stopped);
    }

    #[test]
    fn health_route_reports_llama_sidecar_status() {
        let state = ServerState::new(ServerConfig {
            llama_sidecar: LlamaSidecarConfig {
                launch_policy: LlamaSidecarLaunchPolicy::Never,
                command: "llama-server".to_string(),
                args: Vec::new(),
                model_path: "models/local.gguf".to_string(),
                host: "127.0.0.1".to_string(),
                port: unused_port(),
                health_path: "/health".to_string(),
            },
            ..ServerConfig::default()
        });
        state.ensure_llama_sidecar();
        let raw = http_request("GET", "/admin/llama-sidecar", "");

        let response = handle_http_request(&raw, &state);
        let value: Value = serde_json::from_str(response_body(&response)).expect("json response");

        assert!(response.starts_with("HTTP/1.1 200 OK"));
        assert_eq!(value["llama_sidecar"]["state"], "stopped");
        assert!(value["llama_sidecar"]["endpoint"]
            .as_str()
            .expect("endpoint")
            .starts_with("http://127.0.0.1:"));
    }

    #[test]
    fn default_model_aliases_are_available() {
        let models = default_models();

        assert!(models.iter().any(|model| model.id == "seceda/default"));
        assert!(models.iter().any(|model| model.id == "remote/default"));
    }

    #[test]
    fn responses_path_routes_through_core_and_returns_public_response() {
        let raw = http_request(
            "POST",
            "/v1/responses",
            r#"{"model":"seceda/default","input":"hello"}"#,
        );

        let response = handle_http_request(&raw, &ServerState::default());
        let body = response_body(&response);
        let value: Value = serde_json::from_str(body).expect("json response");

        assert!(response.starts_with("HTTP/1.1 200 OK"));
        assert_eq!(value["object"], "response");
        assert_eq!(value["model"], "seceda/default");
        assert_eq!(
            value["output"][0]["content"][0]["text"],
            "local/llama.cpp response: hello"
        );
        assert!(value.get("routing").is_none());
        assert!(value.get("matched_rules").is_none());
    }

    #[test]
    fn responses_path_uses_remote_capability_routing_without_leaking_metadata() {
        let raw = http_request(
            "POST",
            "/v1/responses",
            r#"{"model":"seceda/default","input":"hello","tools":[]}"#,
        );

        let response = handle_http_request(&raw, &ServerState::default());
        let value: Value = serde_json::from_str(response_body(&response)).expect("json response");

        assert!(response.starts_with("HTTP/1.1 200 OK"));
        assert_eq!(
            value["output"][0]["content"][0]["text"],
            "remote/modal-default response: hello"
        );
        assert!(value.get("routing").is_none());
    }

    #[test]
    fn malformed_responses_request_returns_openai_style_error() {
        let raw = http_request("POST", "/v1/responses", r#"{"input":["unsupported"]}"#);

        let response = handle_http_request(&raw, &ServerState::default());
        let value: Value = serde_json::from_str(response_body(&response)).expect("json response");

        assert!(response.starts_with("HTTP/1.1 400 Bad Request"));
        assert_eq!(value["error"]["type"], "invalid_request_error");
        assert_eq!(value["error"]["code"], "unsupported_input");
    }

    #[test]
    fn streaming_responses_path_emits_stable_sse_sequence() {
        let raw = http_request(
            "POST",
            "/v1/responses",
            r#"{"model":"seceda/default","input":"hello","stream":true}"#,
        );

        let response = handle_http_request(&raw, &ServerState::default());
        let events = sse_event_names(response_body(&response));

        assert!(response.starts_with("HTTP/1.1 200 OK"));
        assert!(response.contains("Content-Type: text/event-stream"));
        assert_eq!(
            events,
            vec![
                "response.created",
                "response.output_item.added",
                "response.content_part.added",
                "response.output_text.delta",
                "response.output_text.done",
                "response.content_part.done",
                "response.output_item.done",
                "response.completed",
            ]
        );
        assert!(response.contains("local/llama.cpp response: hello"));
    }

    #[test]
    fn streaming_public_events_do_not_expose_routing_metadata() {
        let raw = http_request(
            "POST",
            "/v1/responses",
            r#"{"model":"seceda/default","input":"latest news","stream":true}"#,
        );
        let state = ServerState::default();

        let response = handle_http_request(&raw, &state);
        let public_body = response_body(&response);
        let observed = state.observed_events();

        assert!(response.starts_with("HTTP/1.1 200 OK"));
        assert!(!public_body.contains("matched_rules"));
        assert!(!public_body.contains("FreshnessKeyword"));
        assert!(observed
            .iter()
            .any(|event| event.kind == ObservableEventKind::RoutingDecided
                && event.message.contains("FreshnessKeyword")
                && event.message.contains("latest")));
        assert!(observed
            .iter()
            .any(|event| event.kind == ObservableEventKind::StreamDelta));
    }

    #[test]
    fn malformed_streaming_request_returns_json_error() {
        let raw = http_request(
            "POST",
            "/v1/responses",
            r#"{"input":["unsupported"],"stream":true}"#,
        );

        let response = handle_http_request(&raw, &ServerState::default());
        let value: Value = serde_json::from_str(response_body(&response)).expect("json response");

        assert!(response.starts_with("HTTP/1.1 400 Bad Request"));
        assert!(response.contains("Content-Type: application/json"));
        assert_eq!(value["error"]["code"], "unsupported_input");
    }

    #[test]
    fn serve_one_handles_http_level_request() {
        let listener = TcpListener::bind("127.0.0.1:0").expect("listener");
        let addr = listener.local_addr().expect("local addr");
        let state = ServerState::default();
        let server = thread::spawn(move || serve_one(listener, &state));

        let mut stream = TcpStream::connect(addr).expect("client connect");
        stream
            .write_all(
                http_request("POST", "/v1/responses", r#"{"input":"hello over tcp"}"#).as_bytes(),
            )
            .expect("write request");
        let mut response = String::new();
        stream.read_to_string(&mut response).expect("read response");

        server.join().expect("server thread").expect("serve one");
        assert!(response.starts_with("HTTP/1.1 200 OK"));
        assert!(response.contains("hello over tcp"));
    }

    fn http_request(method: &str, path: &str, body: &str) -> String {
        format!(
            "{method} {path} HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: {}\r\n\r\n{body}",
            body.len()
        )
    }

    fn response_body(response: &str) -> &str {
        response.split_once("\r\n\r\n").expect("response body").1
    }

    fn sse_event_names(body: &str) -> Vec<&str> {
        body.lines()
            .filter_map(|line| line.strip_prefix("event: "))
            .collect()
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

    fn llama_config_for_endpoint(endpoint: &str) -> LlamaSidecarConfig {
        let rest = endpoint
            .strip_prefix("http://")
            .expect("test endpoint uses http");
        let (authority, path) = rest.split_once('/').expect("test endpoint path");
        let (host, port) = authority.rsplit_once(':').expect("host port");
        LlamaSidecarConfig {
            launch_policy: LlamaSidecarLaunchPolicy::Never,
            command: "llama-server".to_string(),
            args: Vec::new(),
            model_path: "models/local.gguf".to_string(),
            host: host.to_string(),
            port: port.parse().expect("port"),
            health_path: format!("/{path}"),
        }
    }

    fn unused_port() -> u16 {
        let listener = TcpListener::bind("127.0.0.1:0").expect("listener");
        listener.local_addr().expect("local addr").port()
    }

    fn wait_for_stopped_sidecar(state: &ServerState) -> LlamaSidecarStatus {
        for _ in 0..20 {
            let status = state.llama_sidecar_status();
            if status.state == LlamaSidecarState::Stopped {
                return status;
            }
            thread::sleep(std::time::Duration::from_millis(10));
        }
        state.llama_sidecar_status()
    }
}
