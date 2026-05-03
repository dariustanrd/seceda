//! Server boundary for Seceda's OpenAI-compatible localhost API.

use seceda_core::{
    execute_with_adapters, BackendKind, ChatMessage, ChatRequest, MockRuntimeAdapter,
    ModelDescriptor, RequestFeatures, RuntimeAdapter, SecedaConfig,
};
use serde_json::{json, Value};
use std::error::Error;
use std::fmt;
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};

/// Minimal server configuration used by the CLI and future HTTP listener.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ServerConfig {
    pub bind_host: String,
    pub port: u16,
}

impl Default for ServerConfig {
    fn default() -> Self {
        Self {
            bind_host: "127.0.0.1".to_string(),
            port: 8080,
        }
    }
}

impl ServerConfig {
    pub fn listen_addr(&self) -> String {
        format!("{}:{}", self.bind_host, self.port)
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

#[derive(Debug, Clone)]
pub struct ServerState {
    pub core_config: SecedaConfig,
}

impl Default for ServerState {
    fn default() -> Self {
        Self {
            core_config: SecedaConfig::default(),
        }
    }
}

/// Start the minimal headless server. This call blocks until the listener fails.
pub fn run_headless(config: ServerConfig) -> Result<(), ServerError> {
    let listener = TcpListener::bind(config.listen_addr())?;
    serve_listener(listener, ServerState::default())
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

    Ok(HttpResponse::json(
        200,
        json!({
            "id": "resp_mock_0000000000000000",
            "object": "response",
            "model": result.output.model,
            "output": [
                {
                    "type": "message",
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
        }),
    ))
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
}

impl HttpResponse {
    fn json(status: u16, body: Value) -> Self {
        Self {
            status,
            body: body.to_string(),
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
            "HTTP/1.1 {} {}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
            self.status,
            status_text,
            self.body.len(),
            self.body
        )
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
}
