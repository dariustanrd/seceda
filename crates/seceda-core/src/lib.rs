//! Core Seceda contracts shared by the server, runtimes, and CLI.

use std::error::Error;
use std::fmt;

/// A model exposed through Seceda's OpenAI-compatible model list.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ModelDescriptor {
    pub id: String,
    pub backend: BackendKind,
}

impl ModelDescriptor {
    pub fn new(id: impl Into<String>, backend: BackendKind) -> Self {
        Self {
            id: id.into(),
            backend,
        }
    }
}

/// Runtime backend categories the router can select from.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BackendKind {
    Local,
    Cloud,
    Sidecar,
}

/// Minimal normalized chat request placeholder for the Rust rewrite.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ChatRequest {
    pub model: String,
    pub messages: Vec<ChatMessage>,
}

impl ChatRequest {
    pub fn new(model: impl Into<String>, messages: Vec<ChatMessage>) -> Self {
        Self {
            model: model.into(),
            messages,
        }
    }
}

/// Role/content pair after transport-level request normalization.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ChatMessage {
    pub role: ChatRole,
    pub content: String,
}

impl ChatMessage {
    pub fn user(content: impl Into<String>) -> Self {
        Self {
            role: ChatRole::User,
            content: content.into(),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ChatRole {
    System,
    User,
    Assistant,
    Tool,
}

/// Portable core configuration after defaults have been resolved.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SecedaConfig {
    pub router: RouterConfig,
    pub runtimes: Vec<RuntimeConfig>,
}

impl Default for SecedaConfig {
    fn default() -> Self {
        Self {
            router: RouterConfig::default(),
            runtimes: vec![
                RuntimeConfig::new("mock-local", BackendKind::Local, true),
                RuntimeConfig::new("mock-cloud", BackendKind::Cloud, true),
            ],
        }
    }
}

impl SecedaConfig {
    pub fn validate(&self) -> Result<(), ConfigError> {
        if self.router.default_local_runtime_id.trim().is_empty() {
            return Err(ConfigError::MissingDefaultRuntime("local"));
        }

        if self.router.default_cloud_runtime_id.trim().is_empty() {
            return Err(ConfigError::MissingDefaultRuntime("cloud"));
        }

        let local = self.runtime(&self.router.default_local_runtime_id);
        if !matches!(local, Some(runtime) if runtime.enabled && runtime.backend == BackendKind::Local)
        {
            return Err(ConfigError::MissingDefaultRuntime("local"));
        }

        let cloud = self.runtime(&self.router.default_cloud_runtime_id);
        if !matches!(cloud, Some(runtime) if runtime.enabled && runtime.backend == BackendKind::Cloud)
        {
            return Err(ConfigError::MissingDefaultRuntime("cloud"));
        }

        Ok(())
    }

    pub fn runtime(&self, runtime_id: &str) -> Option<&RuntimeConfig> {
        self.runtimes
            .iter()
            .find(|runtime| runtime.id == runtime_id)
    }
}

/// Router settings that are portable across server, desktop, and future SDK use.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RouterConfig {
    pub default_local_runtime_id: String,
    pub default_cloud_runtime_id: String,
    pub cloud_keywords: Vec<String>,
}

impl Default for RouterConfig {
    fn default() -> Self {
        Self {
            default_local_runtime_id: "mock-local".to_string(),
            default_cloud_runtime_id: "mock-cloud".to_string(),
            cloud_keywords: vec![
                "research".to_string(),
                "latest".to_string(),
                "current".to_string(),
                "cloud".to_string(),
            ],
        }
    }
}

/// Configured runtime entry used by core routing.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RuntimeConfig {
    pub id: String,
    pub backend: BackendKind,
    pub enabled: bool,
}

impl RuntimeConfig {
    pub fn new(id: impl Into<String>, backend: BackendKind, enabled: bool) -> Self {
        Self {
            id: id.into(),
            backend,
            enabled,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ConfigError {
    MissingDefaultRuntime(&'static str),
}

impl fmt::Display for ConfigError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            ConfigError::MissingDefaultRuntime(kind) => {
                write!(formatter, "missing enabled default {kind} runtime")
            }
        }
    }
}

impl Error for ConfigError {}

/// Runtime capabilities visible to the router and setup surfaces.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RuntimeCapabilities {
    pub runtime_id: String,
    pub backend: BackendKind,
    pub models: Vec<String>,
    pub supports_streaming: bool,
    pub max_context_tokens: Option<usize>,
}

impl RuntimeCapabilities {
    pub fn new(runtime_id: impl Into<String>, backend: BackendKind) -> Self {
        Self {
            runtime_id: runtime_id.into(),
            backend,
            models: Vec::new(),
            supports_streaming: false,
            max_context_tokens: None,
        }
    }
}

/// Portable runtime adapter contract owned by core.
pub trait RuntimeAdapter {
    fn capabilities(&self) -> RuntimeCapabilities;

    fn execute(&self, request: &ChatRequest) -> Result<RuntimeOutput, RuntimeError>;
}

/// Runtime output after adapter-specific shapes have been normalized.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RuntimeOutput {
    pub text: String,
    pub model: String,
    pub finish_reason: FinishReason,
}

impl RuntimeOutput {
    pub fn completed(text: impl Into<String>, model: impl Into<String>) -> Self {
        Self {
            text: text.into(),
            model: model.into(),
            finish_reason: FinishReason::Stop,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FinishReason {
    Stop,
    Length,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RuntimeError {
    pub message: String,
}

impl RuntimeError {
    pub fn new(message: impl Into<String>) -> Self {
        Self {
            message: message.into(),
        }
    }
}

impl fmt::Display for RuntimeError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.message)
    }
}

impl Error for RuntimeError {}

/// Deterministic test/runtime adapter used to prove the portable core path.
#[derive(Debug, Clone)]
pub struct MockRuntimeAdapter {
    capabilities: RuntimeCapabilities,
    response_prefix: String,
}

impl MockRuntimeAdapter {
    pub fn new(runtime_id: impl Into<String>, backend: BackendKind) -> Self {
        let runtime_id = runtime_id.into();
        let mut capabilities = RuntimeCapabilities::new(runtime_id.clone(), backend);
        capabilities.models = vec![format!("{runtime_id}/default")];
        capabilities.supports_streaming = true;
        capabilities.max_context_tokens = Some(4096);

        Self {
            capabilities,
            response_prefix: runtime_id,
        }
    }
}

impl RuntimeAdapter for MockRuntimeAdapter {
    fn capabilities(&self) -> RuntimeCapabilities {
        self.capabilities.clone()
    }

    fn execute(&self, request: &ChatRequest) -> Result<RuntimeOutput, RuntimeError> {
        let prompt = request
            .messages
            .iter()
            .rev()
            .find(|message| message.role == ChatRole::User)
            .map(|message| message.content.as_str())
            .unwrap_or("");

        Ok(RuntimeOutput::completed(
            format!("{} response: {}", self.response_prefix, prompt),
            request.model.clone(),
        ))
    }
}

/// The router decision core returns before runtime execution.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RoutingDecision {
    pub requested_model: String,
    pub runtime_id: String,
    pub backend: BackendKind,
    pub reason: RoutingReason,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum RoutingReason {
    ForcedLocalModel,
    ForcedCloudModel,
    AutoLocalDefault,
    AutoCloudKeyword(String),
}

/// Observable execution result with public output separated from trace metadata.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ExecutionResult {
    pub output: RuntimeOutput,
    pub routing: RoutingDecision,
    pub trace: Vec<TraceEvent>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TraceEvent {
    pub kind: TraceEventKind,
    pub message: String,
}

impl TraceEvent {
    pub fn new(kind: TraceEventKind, message: impl Into<String>) -> Self {
        Self {
            kind,
            message: message.into(),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TraceEventKind {
    RequestNormalized,
    RoutingDecided,
    RuntimeSelected,
    RuntimeCompleted,
}

#[derive(Debug, PartialEq, Eq)]
pub enum CoreError {
    Config(ConfigError),
    NoAdapterForRuntime(String),
    Runtime(RuntimeError),
}

impl fmt::Display for CoreError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            CoreError::Config(error) => write!(formatter, "{error}"),
            CoreError::NoAdapterForRuntime(runtime_id) => {
                write!(formatter, "no adapter registered for runtime {runtime_id}")
            }
            CoreError::Runtime(error) => write!(formatter, "{error}"),
        }
    }
}

impl Error for CoreError {}

impl From<ConfigError> for CoreError {
    fn from(error: ConfigError) -> Self {
        CoreError::Config(error)
    }
}

impl From<RuntimeError> for CoreError {
    fn from(error: RuntimeError) -> Self {
        CoreError::Runtime(error)
    }
}

/// Route and execute a normalized request through the portable runtime contract.
pub fn execute_with_adapters(
    config: &SecedaConfig,
    request: &ChatRequest,
    adapters: &[&dyn RuntimeAdapter],
) -> Result<ExecutionResult, CoreError> {
    config.validate()?;

    let mut trace = vec![TraceEvent::new(
        TraceEventKind::RequestNormalized,
        format!("normalized {} message(s)", request.messages.len()),
    )];

    let routing = route_request(config, request)?;
    trace.push(TraceEvent::new(
        TraceEventKind::RoutingDecided,
        format!("selected runtime {}", routing.runtime_id),
    ));

    let adapter = adapters
        .iter()
        .copied()
        .find(|adapter| adapter.capabilities().runtime_id == routing.runtime_id)
        .ok_or_else(|| CoreError::NoAdapterForRuntime(routing.runtime_id.clone()))?;

    trace.push(TraceEvent::new(
        TraceEventKind::RuntimeSelected,
        format!("backend {:?}", routing.backend),
    ));

    let output = adapter.execute(request)?;
    trace.push(TraceEvent::new(
        TraceEventKind::RuntimeCompleted,
        format!("finish_reason {:?}", output.finish_reason),
    ));

    Ok(ExecutionResult {
        output,
        routing,
        trace,
    })
}

pub fn route_request(
    config: &SecedaConfig,
    request: &ChatRequest,
) -> Result<RoutingDecision, CoreError> {
    config.validate()?;

    if request.model == "local/default" {
        return Ok(decision(
            request,
            &config.router.default_local_runtime_id,
            BackendKind::Local,
            RoutingReason::ForcedLocalModel,
        ));
    }

    if request.model == "remote/default" {
        return Ok(decision(
            request,
            &config.router.default_cloud_runtime_id,
            BackendKind::Cloud,
            RoutingReason::ForcedCloudModel,
        ));
    }

    if let Some(keyword) = first_cloud_keyword(&config.router, request) {
        return Ok(decision(
            request,
            &config.router.default_cloud_runtime_id,
            BackendKind::Cloud,
            RoutingReason::AutoCloudKeyword(keyword),
        ));
    }

    Ok(decision(
        request,
        &config.router.default_local_runtime_id,
        BackendKind::Local,
        RoutingReason::AutoLocalDefault,
    ))
}

fn decision(
    request: &ChatRequest,
    runtime_id: &str,
    backend: BackendKind,
    reason: RoutingReason,
) -> RoutingDecision {
    RoutingDecision {
        requested_model: request.model.clone(),
        runtime_id: runtime_id.to_string(),
        backend,
        reason,
    }
}

fn first_cloud_keyword(router: &RouterConfig, request: &ChatRequest) -> Option<String> {
    let content = request
        .messages
        .iter()
        .map(|message| message.content.as_str())
        .collect::<Vec<_>>()
        .join("\n")
        .to_ascii_lowercase();

    router
        .cloud_keywords
        .iter()
        .find(|keyword| content.contains(&keyword.to_ascii_lowercase()))
        .cloned()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn constructs_normalized_chat_request() {
        let request = ChatRequest::new("seceda/default", vec![ChatMessage::user("hello")]);

        assert_eq!(request.model, "seceda/default");
        assert_eq!(request.messages[0].role, ChatRole::User);
        assert_eq!(request.messages[0].content, "hello");
    }

    #[test]
    fn default_config_validates() {
        let config = SecedaConfig::default();

        assert_eq!(config.validate(), Ok(()));
        assert_eq!(config.router.default_local_runtime_id, "mock-local");
        assert_eq!(config.router.default_cloud_runtime_id, "mock-cloud");
    }

    #[test]
    fn config_validation_rejects_missing_default_runtime() {
        let mut config = SecedaConfig::default();
        config.runtimes.retain(|runtime| runtime.id != "mock-cloud");

        assert_eq!(
            config.validate(),
            Err(ConfigError::MissingDefaultRuntime("cloud"))
        );
    }

    #[test]
    fn mock_runtime_executes_minimal_request() {
        let adapter = MockRuntimeAdapter::new("mock-local", BackendKind::Local);
        let request = ChatRequest::new("local/default", vec![ChatMessage::user("hello")]);

        let output = adapter.execute(&request).expect("mock execution");

        assert_eq!(output.text, "mock-local response: hello");
        assert_eq!(output.model, "local/default");
        assert_eq!(output.finish_reason, FinishReason::Stop);
    }

    #[test]
    fn router_forces_remote_alias_to_cloud() {
        let config = SecedaConfig::default();
        let request = ChatRequest::new("remote/default", vec![ChatMessage::user("hello")]);

        let routing = route_request(&config, &request).expect("routing decision");

        assert_eq!(routing.runtime_id, "mock-cloud");
        assert_eq!(routing.backend, BackendKind::Cloud);
        assert_eq!(routing.reason, RoutingReason::ForcedCloudModel);
    }

    #[test]
    fn router_sends_keyword_matched_auto_request_to_cloud() {
        let config = SecedaConfig::default();
        let request = ChatRequest::new(
            "seceda/default",
            vec![ChatMessage::user("research the latest runtime options")],
        );

        let routing = route_request(&config, &request).expect("routing decision");

        assert_eq!(routing.runtime_id, "mock-cloud");
        assert_eq!(routing.backend, BackendKind::Cloud);
        assert_eq!(
            routing.reason,
            RoutingReason::AutoCloudKeyword("research".to_string())
        );
    }

    #[test]
    fn core_executes_request_and_returns_observable_result() {
        let config = SecedaConfig::default();
        let local = MockRuntimeAdapter::new("mock-local", BackendKind::Local);
        let cloud = MockRuntimeAdapter::new("mock-cloud", BackendKind::Cloud);
        let request = ChatRequest::new("seceda/default", vec![ChatMessage::user("hello")]);

        let result =
            execute_with_adapters(&config, &request, &[&local, &cloud]).expect("core execution");

        assert_eq!(result.output.text, "mock-local response: hello");
        assert_eq!(result.routing.runtime_id, "mock-local");
        assert_eq!(result.routing.reason, RoutingReason::AutoLocalDefault);
        assert_eq!(
            result
                .trace
                .iter()
                .map(|event| event.kind)
                .collect::<Vec<_>>(),
            vec![
                TraceEventKind::RequestNormalized,
                TraceEventKind::RoutingDecided,
                TraceEventKind::RuntimeSelected,
                TraceEventKind::RuntimeCompleted,
            ]
        );
    }

    #[test]
    fn core_reports_missing_adapter() {
        let config = SecedaConfig::default();
        let local = MockRuntimeAdapter::new("mock-local", BackendKind::Local);
        let request = ChatRequest::new("remote/default", vec![ChatMessage::user("hello")]);

        let error = execute_with_adapters(&config, &request, &[&local]).expect_err("core error");

        assert_eq!(
            error,
            CoreError::NoAdapterForRuntime("mock-cloud".to_string())
        );
    }
}
