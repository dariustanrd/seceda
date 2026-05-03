//! Cloud provider runtime organization for Seceda.
//!
//! Provider-specific transport, auth, request transforms, and backend details
//! belong here or in child crates under this package family. `seceda-core`
//! should only see portable runtime identifiers, backend kinds, model hints,
//! and capability metadata.

use seceda_core::{
    BackendKind, ChatRequest, FinishReason, RuntimeAdapter, RuntimeCapabilities, RuntimeConfig,
    RuntimeError, RuntimeOutput, RuntimeStreamEvent,
};
use serde::{Deserialize, Serialize};
use std::env;
use std::error::Error;
use std::fmt;
use std::fs;
use std::io::{self, Read, Write};
use std::net::{SocketAddr, TcpStream, ToSocketAddrs};
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

pub const CODEX_SUBSCRIPTION_RUNTIME_ID: &str = "cloud/codex-subscription";
pub const CODEX_SUBSCRIPTION_MODEL_HINT: &str = "codex-subscription/default";
pub const MODAL_RUNTIME_ID: &str = "remote/modal-default";
pub const MODAL_MODEL_HINT: &str = "remote/default";

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CloudProviderKind {
    CodexSubscription,
    Modal,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CloudProviderDescriptor {
    pub kind: CloudProviderKind,
    pub runtime_id: &'static str,
    pub display_name: &'static str,
    pub module_path: &'static str,
    pub model_hint: &'static str,
    pub implemented_in: CloudProviderImplementation,
}

impl CloudProviderDescriptor {
    pub fn runtime_config(&self, enabled: bool) -> RuntimeConfig {
        RuntimeConfig::new(self.runtime_id, BackendKind::Cloud, enabled)
            .with_model_hint(self.model_hint)
    }

    pub fn capabilities(&self) -> RuntimeCapabilities {
        let mut capabilities = RuntimeCapabilities::new(self.runtime_id, BackendKind::Cloud);
        capabilities.models = vec![self.model_hint.to_string()];
        capabilities.supports_streaming = matches!(
            self.kind,
            CloudProviderKind::CodexSubscription | CloudProviderKind::Modal
        );
        capabilities
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CloudProviderImplementation {
    RustWorkspace,
    PreservedPythonPackage,
}

pub fn known_cloud_providers() -> Vec<CloudProviderDescriptor> {
    vec![codex_subscription_provider(), modal_provider()]
}

pub fn codex_subscription_provider() -> CloudProviderDescriptor {
    CloudProviderDescriptor {
        kind: CloudProviderKind::CodexSubscription,
        runtime_id: CODEX_SUBSCRIPTION_RUNTIME_ID,
        display_name: "Codex Subscription",
        module_path: "crates/seceda-cloud-providers/seceda-codex-subscription",
        model_hint: CODEX_SUBSCRIPTION_MODEL_HINT,
        implemented_in: CloudProviderImplementation::RustWorkspace,
    }
}

pub fn modal_provider() -> CloudProviderDescriptor {
    CloudProviderDescriptor {
        kind: CloudProviderKind::Modal,
        runtime_id: MODAL_RUNTIME_ID,
        display_name: "Modal",
        module_path: "seceda_cloud",
        model_hint: MODAL_MODEL_HINT,
        implemented_in: CloudProviderImplementation::PreservedPythonPackage,
    }
}

pub const SECEDA_HOME_ENV: &str = "SECEDA_HOME";
pub const SECEDA_HOME_DIR: &str = ".seceda";
pub const CODEX_SUBSCRIPTION_CREDENTIAL_FILE: &str = "codex-subscription-auth.json";

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct CodexSubscriptionCredential {
    pub access_token: String,
    pub refresh_token: Option<String>,
    pub expires_at_unix_seconds: u64,
    pub chatgpt_account_id: String,
    pub token_type: String,
}

impl CodexSubscriptionCredential {
    pub fn new(
        access_token: impl Into<String>,
        refresh_token: Option<impl Into<String>>,
        expires_at_unix_seconds: u64,
        chatgpt_account_id: impl Into<String>,
    ) -> Self {
        Self {
            access_token: access_token.into(),
            refresh_token: refresh_token.map(Into::into),
            expires_at_unix_seconds,
            chatgpt_account_id: chatgpt_account_id.into(),
            token_type: "Bearer".to_string(),
        }
    }

    pub fn validate(&self, now_unix_seconds: u64) -> Result<(), CredentialStatus> {
        if self.access_token.trim().is_empty() {
            return Err(CredentialStatus::Malformed(
                "missing access token".to_string(),
            ));
        }
        if self.chatgpt_account_id.trim().is_empty() {
            return Err(CredentialStatus::Malformed(
                "missing ChatGPT account id".to_string(),
            ));
        }
        if self.expires_at_unix_seconds <= now_unix_seconds {
            return Err(CredentialStatus::Expired);
        }
        Ok(())
    }

    pub fn needs_refresh(&self, now_unix_seconds: u64, buffer: Duration) -> bool {
        now_unix_seconds.saturating_add(buffer.as_secs()) >= self.expires_at_unix_seconds
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum CredentialStatus {
    Available,
    Missing,
    Malformed(String),
    Expired,
    RefreshNeeded,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CredentialState {
    pub status: CredentialStatus,
    pub path: PathBuf,
}

#[derive(Debug)]
pub enum CredentialStoreError {
    HomeUnavailable,
    Io { path: PathBuf, source: io::Error },
    Json { path: PathBuf, message: String },
    Invalid(CredentialStatus),
}

impl fmt::Display for CredentialStoreError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            CredentialStoreError::HomeUnavailable => {
                formatter.write_str("could not resolve Seceda home directory")
            }
            CredentialStoreError::Io { path, source } => {
                write!(formatter, "{}: {source}", path.display())
            }
            CredentialStoreError::Json { path, message } => {
                write!(formatter, "{}: {message}", path.display())
            }
            CredentialStoreError::Invalid(status) => {
                write!(formatter, "invalid Codex subscription credential: {status}")
            }
        }
    }
}

impl Error for CredentialStoreError {
    fn source(&self) -> Option<&(dyn Error + 'static)> {
        match self {
            CredentialStoreError::Io { source, .. } => Some(source),
            _ => None,
        }
    }
}

impl fmt::Display for CredentialStatus {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            CredentialStatus::Available => formatter.write_str("available"),
            CredentialStatus::Missing => formatter.write_str("missing"),
            CredentialStatus::Malformed(message) => write!(formatter, "malformed ({message})"),
            CredentialStatus::Expired => formatter.write_str("expired"),
            CredentialStatus::RefreshNeeded => formatter.write_str("refresh-needed"),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SecedaCredentialStore {
    home_dir: PathBuf,
}

impl SecedaCredentialStore {
    pub fn from_env() -> Result<Self, CredentialStoreError> {
        if let Some(home) = env::var_os(SECEDA_HOME_ENV) {
            return Ok(Self::new(home));
        }

        let home = env::var_os("HOME").ok_or(CredentialStoreError::HomeUnavailable)?;
        Ok(Self::new(Path::new(&home).join(SECEDA_HOME_DIR)))
    }

    pub fn new(home_dir: impl Into<PathBuf>) -> Self {
        Self {
            home_dir: home_dir.into(),
        }
    }

    pub fn home_dir(&self) -> &Path {
        &self.home_dir
    }

    pub fn codex_subscription_credential_path(&self) -> PathBuf {
        self.home_dir.join(CODEX_SUBSCRIPTION_CREDENTIAL_FILE)
    }

    pub fn write_codex_subscription_credential(
        &self,
        credential: &CodexSubscriptionCredential,
    ) -> Result<(), CredentialStoreError> {
        let path = self.codex_subscription_credential_path();
        credential
            .validate(current_unix_seconds())
            .map_err(CredentialStoreError::Invalid)?;
        fs::create_dir_all(&self.home_dir).map_err(|source| CredentialStoreError::Io {
            path: self.home_dir.clone(),
            source,
        })?;
        let body = serde_json::to_string_pretty(credential).map_err(|error| {
            CredentialStoreError::Json {
                path: path.clone(),
                message: error.to_string(),
            }
        })?;
        fs::write(&path, body).map_err(|source| CredentialStoreError::Io { path, source })
    }

    pub fn read_codex_subscription_credential(
        &self,
    ) -> Result<CodexSubscriptionCredential, CredentialStoreError> {
        let path = self.codex_subscription_credential_path();
        let body = fs::read_to_string(&path).map_err(|source| {
            if source.kind() == io::ErrorKind::NotFound {
                CredentialStoreError::Invalid(CredentialStatus::Missing)
            } else {
                CredentialStoreError::Io {
                    path: path.clone(),
                    source,
                }
            }
        })?;
        let credential =
            serde_json::from_str::<CodexSubscriptionCredential>(&body).map_err(|error| {
                CredentialStoreError::Json {
                    path: path.clone(),
                    message: error.to_string(),
                }
            })?;
        credential
            .validate(current_unix_seconds())
            .map_err(CredentialStoreError::Invalid)?;
        Ok(credential)
    }

    pub fn codex_subscription_state(&self, refresh_buffer: Duration) -> CredentialState {
        let path = self.codex_subscription_credential_path();
        let now = current_unix_seconds();
        let status = match fs::read_to_string(&path) {
            Ok(body) => match serde_json::from_str::<CodexSubscriptionCredential>(&body) {
                Ok(credential) => match credential.validate(now) {
                    Ok(()) if credential.needs_refresh(now, refresh_buffer) => {
                        CredentialStatus::RefreshNeeded
                    }
                    Ok(()) => CredentialStatus::Available,
                    Err(status) => status,
                },
                Err(error) => CredentialStatus::Malformed(error.to_string()),
            },
            Err(error) if error.kind() == io::ErrorKind::NotFound => CredentialStatus::Missing,
            Err(error) => CredentialStatus::Malformed(error.to_string()),
        };

        CredentialState { status, path }
    }
}

pub fn current_unix_seconds() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs()
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CodexSubscriptionRuntimeAdapter {
    endpoint: String,
    credential_store: SecedaCredentialStore,
    timeout: Duration,
}

impl CodexSubscriptionRuntimeAdapter {
    pub fn new(endpoint: impl Into<String>, credential_store: SecedaCredentialStore) -> Self {
        Self {
            endpoint: endpoint.into(),
            credential_store,
            timeout: Duration::from_secs(30),
        }
    }

    pub fn default_endpoint() -> &'static str {
        "https://chatgpt.com/backend-api/codex/responses"
    }

    pub fn with_timeout(mut self, timeout: Duration) -> Self {
        self.timeout = timeout;
        self
    }

    fn post_responses(&self, request: &ChatRequest, stream: bool) -> Result<String, RuntimeError> {
        let credential = self
            .credential_store
            .read_codex_subscription_credential()
            .map_err(|error| {
                RuntimeError::new(format!("Codex subscription auth failed: {error}"))
            })?;
        let parsed = ParsedHttpEndpoint::parse(&self.endpoint).ok_or_else(|| {
            RuntimeError::new("Codex subscription endpoint must be an http://host:port URL")
        })?;
        let addr = parsed.socket_addr().ok_or_else(|| {
            RuntimeError::new("Codex subscription endpoint host could not be resolved")
        })?;
        let mut tcp = TcpStream::connect_timeout(&addr, self.timeout).map_err(|error| {
            RuntimeError::new(format!(
                "failed to connect to Codex subscription backend: {error}"
            ))
        })?;
        let _ = tcp.set_read_timeout(Some(self.timeout));
        let body = codex_responses_body(request, stream).to_string();
        let http_request = format!(
            "POST {} HTTP/1.1\r\nHost: {}\r\nAuthorization: Bearer {}\r\nChatGPT-Account-Id: {}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
            parsed.path,
            parsed.host_header,
            credential.access_token,
            credential.chatgpt_account_id,
            body.len(),
            body
        );
        tcp.write_all(http_request.as_bytes()).map_err(|error| {
            RuntimeError::new(format!(
                "failed to send Codex subscription request: {error}"
            ))
        })?;

        let mut response = String::new();
        tcp.read_to_string(&mut response).map_err(|error| {
            RuntimeError::new(format!(
                "failed to read Codex subscription response: {error}"
            ))
        })?;

        let status = parse_status_code(&response).unwrap_or(0);
        if !(200..=299).contains(&status) {
            return Err(RuntimeError::new(format!(
                "Codex subscription request failed with HTTP {status}"
            )));
        }

        response_body(&response)
    }
}

impl RuntimeAdapter for CodexSubscriptionRuntimeAdapter {
    fn capabilities(&self) -> RuntimeCapabilities {
        codex_subscription_provider().capabilities()
    }

    fn execute(&self, request: &ChatRequest) -> Result<RuntimeOutput, RuntimeError> {
        let body = self.post_responses(request, false)?;
        parse_codex_response(&body)
    }

    fn execute_stream(
        &self,
        request: &ChatRequest,
    ) -> Result<Vec<RuntimeStreamEvent>, RuntimeError> {
        let body = self.post_responses(request, true)?;
        parse_codex_stream(&body, &request.model)
    }
}

fn codex_responses_body(request: &ChatRequest, stream: bool) -> serde_json::Value {
    let instructions = request.messages.iter().find_map(|message| {
        (message.role == seceda_core::ChatRole::System).then_some(message.content.as_str())
    });
    let input = request
        .messages
        .iter()
        .rev()
        .find_map(|message| {
            (message.role == seceda_core::ChatRole::User).then_some(message.content.as_str())
        })
        .unwrap_or("");
    let mut body = serde_json::json!({
        "model": request.model,
        "input": input,
        "stream": stream,
        "store": false,
        "include": ["reasoning.encrypted_content"]
    });
    if let Some(instructions) = instructions {
        body["instructions"] = serde_json::Value::String(instructions.to_string());
    }
    body
}

fn parse_codex_response(body: &str) -> Result<RuntimeOutput, RuntimeError> {
    let value: serde_json::Value = serde_json::from_str(body).map_err(|error| {
        RuntimeError::new(format!("invalid Codex subscription JSON response: {error}"))
    })?;
    let text = response_output_text(&value).ok_or_else(|| {
        RuntimeError::new("Codex subscription response missing output text content")
    })?;
    let model = value["model"]
        .as_str()
        .unwrap_or(CODEX_SUBSCRIPTION_MODEL_HINT);
    Ok(RuntimeOutput {
        text,
        model: model.to_string(),
        finish_reason: FinishReason::Stop,
    })
}

fn response_output_text(value: &serde_json::Value) -> Option<String> {
    value["output"].as_array()?.iter().find_map(|item| {
        item["content"].as_array()?.iter().find_map(|content| {
            content["text"]
                .as_str()
                .map(ToString::to_string)
                .or_else(|| content["delta"].as_str().map(ToString::to_string))
        })
    })
}

fn parse_codex_stream(
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
        if data.trim() == "[DONE]" || data.trim().is_empty() {
            continue;
        }
        let value: serde_json::Value = serde_json::from_str(data).map_err(|error| {
            RuntimeError::new(format!(
                "invalid Codex subscription streaming event: {error}"
            ))
        })?;
        if let Some(event_model) = value["response"]["model"].as_str() {
            model = event_model.to_string();
        }
        if events.is_empty() {
            events.push(RuntimeStreamEvent::Started {
                model: model.clone(),
            });
        }
        let event_type = value["type"].as_str().unwrap_or_default();
        match event_type {
            "response.output_text.delta" | "response.refusal.delta" => {
                if let Some(delta) = value["delta"].as_str() {
                    text.push_str(delta);
                    events.push(RuntimeStreamEvent::TextDelta {
                        text: delta.to_string(),
                    });
                }
            }
            "response.completed" => {
                if let Some(response_model) = value["response"]["model"].as_str() {
                    model = response_model.to_string();
                }
                events.push(RuntimeStreamEvent::Completed {
                    output: RuntimeOutput {
                        text: text.clone(),
                        model: model.clone(),
                        finish_reason: FinishReason::Stop,
                    },
                });
            }
            "response.failed" => {
                let message = value["response"]["error"]["message"]
                    .as_str()
                    .unwrap_or("Codex subscription stream failed")
                    .to_string();
                events.push(RuntimeStreamEvent::Failed { message });
            }
            _ => {}
        }
    }

    if events.is_empty() {
        events.push(RuntimeStreamEvent::Failed {
            message: "Codex subscription stream did not contain any events".to_string(),
        });
    } else if !matches!(
        events.last(),
        Some(RuntimeStreamEvent::Completed { .. } | RuntimeStreamEvent::Failed { .. })
    ) {
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

#[derive(Debug, Clone, PartialEq, Eq)]
struct ParsedHttpEndpoint {
    host: String,
    port: u16,
    path: String,
    host_header: String,
}

impl ParsedHttpEndpoint {
    fn parse(endpoint: &str) -> Option<Self> {
        let rest = endpoint.strip_prefix("http://")?;
        let authority = rest.split('/').next().unwrap_or(rest);
        let path = if rest[authority.len()..].is_empty() {
            "/backend-api/codex/responses".to_string()
        } else {
            rest[authority.len()..].to_string()
        };
        let (host, port) = authority.rsplit_once(':')?;
        let port = port.parse().ok()?;
        Some(Self {
            host: host.to_string(),
            port,
            path,
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

fn response_body(response: &str) -> Result<String, RuntimeError> {
    let (headers, body) = response
        .split_once("\r\n\r\n")
        .ok_or_else(|| RuntimeError::new("Codex subscription response was malformed"))?;

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
            .ok_or_else(|| RuntimeError::new("malformed chunked Codex subscription response"))?;
        let size_hex = size_line.split(';').next().unwrap_or(size_line).trim();
        let size = usize::from_str_radix(size_hex, 16)
            .map_err(|_| RuntimeError::new("invalid chunk size in Codex subscription response"))?;
        if size == 0 {
            break;
        }
        if rest.len() < size + 2 {
            return Err(RuntimeError::new(
                "truncated chunked Codex subscription response",
            ));
        }
        decoded.push_str(&rest[..size]);
        body = &rest[size + 2..];
    }

    Ok(decoded)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn known_providers_include_codex_subscription_and_modal() {
        let providers = known_cloud_providers();

        assert_eq!(providers.len(), 2);
        assert!(providers
            .iter()
            .any(|provider| provider.runtime_id == CODEX_SUBSCRIPTION_RUNTIME_ID));
        assert!(providers
            .iter()
            .any(|provider| provider.runtime_id == MODAL_RUNTIME_ID));
    }

    #[test]
    fn descriptors_build_cloud_runtime_configs() {
        let runtime = codex_subscription_provider().runtime_config(true);

        assert_eq!(runtime.id, CODEX_SUBSCRIPTION_RUNTIME_ID);
        assert_eq!(runtime.backend, BackendKind::Cloud);
        assert!(runtime.enabled);
        assert_eq!(
            runtime.model_hint.as_deref(),
            Some(CODEX_SUBSCRIPTION_MODEL_HINT)
        );
    }

    #[test]
    fn core_capabilities_remain_provider_agnostic() {
        let capabilities = modal_provider().capabilities();

        assert_eq!(capabilities.runtime_id, MODAL_RUNTIME_ID);
        assert_eq!(capabilities.backend, BackendKind::Cloud);
        assert_eq!(capabilities.models, vec![MODAL_MODEL_HINT.to_string()]);
    }

    #[test]
    fn codex_subscription_capabilities_are_conservative() {
        let capabilities = codex_subscription_provider().capabilities();

        assert_eq!(capabilities.runtime_id, CODEX_SUBSCRIPTION_RUNTIME_ID);
        assert_eq!(capabilities.backend, BackendKind::Cloud);
        assert!(capabilities.supports_streaming);
        assert!(!capabilities.supports_tools);
        assert!(!capabilities.supports_stateful_responses);
        assert!(!capabilities.supports_multimodal_input);
    }

    #[test]
    fn codex_adapter_sends_auth_and_account_headers() {
        let store = credential_store_with_valid_credential("headers");
        let endpoint = spawn_fake_codex_server(
            |request| {
                assert!(request.starts_with("POST /backend-api/codex/responses HTTP/1.1"));
                assert!(request.contains("Authorization: Bearer access-token"));
                assert!(request.contains("ChatGPT-Account-Id: acct_123"));
                assert!(request.contains(r#""store":false"#));
                assert!(request.contains(r#""include":["reasoning.encrypted_content"]"#));
                assert!(request.contains(r#""input":"hello""#));
                assert!(request.contains(r#""instructions":"be brief""#));
                assert!(request.contains(r#""stream":false"#));
            },
            r#"{"model":"gpt-5.1-codex","output":[{"content":[{"type":"output_text","text":"cloud text"}]}]}"#,
        );
        let adapter = CodexSubscriptionRuntimeAdapter::new(endpoint, store)
            .with_timeout(Duration::from_secs(1));
        let request = ChatRequest::new(
            "seceda/default",
            vec![
                seceda_core::ChatMessage::system("be brief"),
                seceda_core::ChatMessage::user("hello"),
            ],
        );

        let output = adapter.execute(&request).expect("codex execution");

        assert_eq!(output.text, "cloud text");
        assert_eq!(output.model, "gpt-5.1-codex");
    }

    #[test]
    fn codex_adapter_streams_text_events() {
        let store = credential_store_with_valid_credential("stream");
        let endpoint = spawn_fake_codex_server(
            |request| {
                assert!(request.contains(r#""stream":true"#));
            },
            concat!(
                "data: {\"type\":\"response.created\",\"response\":{\"model\":\"gpt-5.1-codex\"}}\n\n",
                "data: {\"type\":\"response.output_text.delta\",\"delta\":\"one\"}\n\n",
                "data: {\"type\":\"response.output_text.delta\",\"delta\":\" two\"}\n\n",
                "data: {\"type\":\"response.completed\",\"response\":{\"model\":\"gpt-5.1-codex\"}}\n\n",
                "data: [DONE]\n\n"
            ),
        );
        let adapter = CodexSubscriptionRuntimeAdapter::new(endpoint, store)
            .with_timeout(Duration::from_secs(1));
        let request = ChatRequest::new(
            "seceda/default",
            vec![seceda_core::ChatMessage::user("hello")],
        );

        let events = adapter.execute_stream(&request).expect("codex stream");

        assert_eq!(
            events,
            vec![
                RuntimeStreamEvent::Started {
                    model: "gpt-5.1-codex".to_string()
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
                        model: "gpt-5.1-codex".to_string(),
                        finish_reason: FinishReason::Stop
                    }
                }
            ]
        );
    }

    #[test]
    fn codex_adapter_fails_when_credentials_are_missing() {
        let store = SecedaCredentialStore::new(test_store_dir("adapter-missing"));
        let adapter = CodexSubscriptionRuntimeAdapter::new("http://127.0.0.1:1", store)
            .with_timeout(Duration::from_millis(10));
        let request = ChatRequest::new(
            "seceda/default",
            vec![seceda_core::ChatMessage::user("hello")],
        );

        let error = adapter.execute(&request).expect_err("missing credential");

        assert!(error.to_string().contains("Codex subscription auth failed"));
        assert!(error.to_string().contains("missing"));
    }

    #[test]
    fn credential_store_reports_missing_codex_subscription_credential() {
        let store = SecedaCredentialStore::new(test_store_dir("missing"));

        let state = store.codex_subscription_state(Duration::from_secs(300));

        assert_eq!(state.status, CredentialStatus::Missing);
        assert!(state.path.ends_with(CODEX_SUBSCRIPTION_CREDENTIAL_FILE));
    }

    #[test]
    fn credential_store_writes_and_reads_codex_subscription_credential() {
        let store = SecedaCredentialStore::new(test_store_dir("roundtrip"));
        let credential = CodexSubscriptionCredential::new(
            "access-token",
            Some("refresh-token"),
            current_unix_seconds() + 3600,
            "acct_123",
        );

        store
            .write_codex_subscription_credential(&credential)
            .expect("credential write");
        let actual = store
            .read_codex_subscription_credential()
            .expect("credential read");

        assert_eq!(actual, credential);
        assert_eq!(
            store
                .codex_subscription_state(Duration::from_secs(300))
                .status,
            CredentialStatus::Available
        );
    }

    #[test]
    fn credential_store_reports_malformed_json() {
        let store = SecedaCredentialStore::new(test_store_dir("malformed"));
        std::fs::create_dir_all(store.home_dir()).expect("test dir");
        std::fs::write(store.codex_subscription_credential_path(), "{not-json")
            .expect("test credential");

        let state = store.codex_subscription_state(Duration::from_secs(300));

        assert!(matches!(state.status, CredentialStatus::Malformed(_)));
    }

    #[test]
    fn credential_store_reports_expired_and_refresh_needed_states() {
        let expired = CodexSubscriptionCredential::new(
            "access-token",
            Some("refresh-token"),
            current_unix_seconds().saturating_sub(1),
            "acct_123",
        );
        assert_eq!(
            expired.validate(current_unix_seconds()),
            Err(CredentialStatus::Expired)
        );

        let refresh_needed = CodexSubscriptionCredential::new(
            "access-token",
            Some("refresh-token"),
            current_unix_seconds() + 120,
            "acct_123",
        );
        assert!(refresh_needed.needs_refresh(current_unix_seconds(), Duration::from_secs(300)));
    }

    fn test_store_dir(name: &str) -> PathBuf {
        let mut path = std::env::temp_dir();
        path.push(format!(
            "seceda-cloud-providers-test-{name}-{}",
            std::process::id()
        ));
        let _ = std::fs::remove_dir_all(&path);
        path
    }

    fn credential_store_with_valid_credential(name: &str) -> SecedaCredentialStore {
        let store = SecedaCredentialStore::new(test_store_dir(name));
        let credential = CodexSubscriptionCredential::new(
            "access-token",
            Some("refresh-token"),
            current_unix_seconds() + 3600,
            "acct_123",
        );
        store
            .write_codex_subscription_credential(&credential)
            .expect("credential write");
        store
    }

    fn spawn_fake_codex_server(
        assert_request: impl FnOnce(&str) + Send + 'static,
        response_body: &'static str,
    ) -> String {
        let listener = std::net::TcpListener::bind("127.0.0.1:0").expect("listener");
        let addr = listener.local_addr().expect("local addr");
        std::thread::spawn(move || {
            let (mut stream, _) = listener.accept().expect("accept");
            let mut buffer = [0; 8192];
            let bytes = stream.read(&mut buffer).expect("read");
            let request = String::from_utf8_lossy(&buffer[..bytes]).to_string();
            assert_request(&request);
            let response = format!(
                "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
                response_body.len(),
                response_body
            );
            stream.write_all(response.as_bytes()).expect("write");
        });
        format!("http://{addr}/backend-api/codex/responses")
    }
}
