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
    pub features: RequestFeatures,
}

impl ChatRequest {
    pub fn new(model: impl Into<String>, messages: Vec<ChatMessage>) -> Self {
        Self {
            model: model.into(),
            messages,
            features: RequestFeatures::default(),
        }
    }

    pub fn with_features(mut self, features: RequestFeatures) -> Self {
        self.features = features;
        self
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

/// Transport-normalized feature flags that require stronger runtime capability.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct RequestFeatures {
    pub tools: bool,
    pub tool_choice: bool,
    pub structured_output: bool,
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
                RuntimeConfig::new("local/llama.cpp", BackendKind::Local, true)
                    .with_model_hint("local/default"),
                RuntimeConfig::new("remote/modal-default", BackendKind::Cloud, true)
                    .with_model_hint("remote/default"),
            ],
        }
    }
}

impl SecedaConfig {
    pub fn validate(&self) -> Result<(), ConfigError> {
        if self.router.prompt_char_limit == 0 {
            return Err(ConfigError::InvalidRouterLimit("prompt_char_limit"));
        }

        if self.router.estimated_prompt_token_limit == 0 {
            return Err(ConfigError::InvalidRouterLimit(
                "estimated_prompt_token_limit",
            ));
        }

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
    pub prompt_char_limit: usize,
    pub estimated_prompt_token_limit: usize,
    pub structured_output_keywords: Vec<String>,
    pub freshness_keywords: Vec<String>,
    pub complexity_keywords: Vec<String>,
}

impl Default for RouterConfig {
    fn default() -> Self {
        Self {
            default_local_runtime_id: "local/llama.cpp".to_string(),
            default_cloud_runtime_id: "remote/modal-default".to_string(),
            prompt_char_limit: 800,
            estimated_prompt_token_limit: 256,
            structured_output_keywords: vec![
                "json".to_string(),
                "schema".to_string(),
                "sql".to_string(),
                "typescript".to_string(),
                "javascript".to_string(),
                "python".to_string(),
                "yaml".to_string(),
                "xml".to_string(),
                "csv".to_string(),
                "formal proof".to_string(),
            ],
            freshness_keywords: vec![
                "today".to_string(),
                "latest".to_string(),
                "current".to_string(),
                "recent".to_string(),
                "news".to_string(),
                "weather".to_string(),
                "price".to_string(),
                "stock".to_string(),
            ],
            complexity_keywords: vec![
                "analyze".to_string(),
                "compare".to_string(),
                "plan".to_string(),
                "strategy".to_string(),
                "architecture".to_string(),
                "reason".to_string(),
                "step by step".to_string(),
                "research".to_string(),
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
    pub model_hint: Option<String>,
}

impl RuntimeConfig {
    pub fn new(id: impl Into<String>, backend: BackendKind, enabled: bool) -> Self {
        Self {
            id: id.into(),
            backend,
            enabled,
            model_hint: None,
        }
    }

    pub fn with_model_hint(mut self, model_hint: impl Into<String>) -> Self {
        self.model_hint = Some(model_hint.into());
        self
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ConfigError {
    MissingDefaultRuntime(&'static str),
    InvalidRouterLimit(&'static str),
}

impl fmt::Display for ConfigError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            ConfigError::MissingDefaultRuntime(kind) => {
                write!(formatter, "missing enabled default {kind} runtime")
            }
            ConfigError::InvalidRouterLimit(name) => {
                write!(formatter, "router limit {name} must be greater than zero")
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
    pub target: RouteTarget,
    pub runtime_id: String,
    pub backend: BackendKind,
    pub reason: RoutingReason,
    pub matched_rules: Vec<String>,
    pub estimated_prompt_tokens: usize,
    pub preferred_runtime_id: String,
    pub preferred_model: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum RoutingReason {
    ForcedLocalModel,
    ForcedCloudModel,
    RemoteCapabilityRequired,
    PromptTooLong,
    EstimatedTokensTooHigh,
    StructuredOutputKeyword,
    FreshnessKeyword,
    ComplexityKeyword,
    AutoLocalDefault,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RouteTarget {
    Local,
    Cloud,
}

impl RouteTarget {
    fn backend(self) -> BackendKind {
        match self {
            RouteTarget::Local => BackendKind::Local,
            RouteTarget::Cloud => BackendKind::Cloud,
        }
    }
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
    HeuristicRouter::new(config).route(request)
}

/// Baseline debuggable local-vs-cloud routing policy.
pub struct HeuristicRouter<'a> {
    config: &'a SecedaConfig,
}

impl<'a> HeuristicRouter<'a> {
    pub fn new(config: &'a SecedaConfig) -> Self {
        Self { config }
    }

    pub fn route(&self, request: &ChatRequest) -> Result<RoutingDecision, CoreError> {
        self.config.validate()?;

        let estimated_prompt_tokens = estimate_prompt_tokens(request);

        if request.model == "local/default" {
            return Ok(self.decision(
                request,
                RouteTarget::Local,
                RoutingReason::ForcedLocalModel,
                vec!["model:local/default".to_string()],
                estimated_prompt_tokens,
            ));
        }

        if request.model == "remote/default" {
            return Ok(self.decision(
                request,
                RouteTarget::Cloud,
                RoutingReason::ForcedCloudModel,
                vec!["model:remote/default".to_string()],
                estimated_prompt_tokens,
            ));
        }

        if request.features.tools
            || request.features.tool_choice
            || request.features.structured_output
        {
            return Ok(self.decision(
                request,
                RouteTarget::Cloud,
                RoutingReason::RemoteCapabilityRequired,
                remote_capability_rules(request),
                estimated_prompt_tokens,
            ));
        }

        let prompt_chars = prompt_char_count(request);
        if prompt_chars > self.config.router.prompt_char_limit {
            return Ok(self.decision(
                request,
                RouteTarget::Cloud,
                RoutingReason::PromptTooLong,
                vec!["max_prompt_chars".to_string()],
                estimated_prompt_tokens,
            ));
        }

        if estimated_prompt_tokens > self.config.router.estimated_prompt_token_limit {
            return Ok(self.decision(
                request,
                RouteTarget::Cloud,
                RoutingReason::EstimatedTokensTooHigh,
                vec!["max_estimated_tokens".to_string()],
                estimated_prompt_tokens,
            ));
        }

        let structured_matches =
            keyword_matches(&self.config.router.structured_output_keywords, request);
        if !structured_matches.is_empty() {
            return Ok(self.decision(
                request,
                RouteTarget::Cloud,
                RoutingReason::StructuredOutputKeyword,
                structured_matches,
                estimated_prompt_tokens,
            ));
        }

        let freshness_matches = keyword_matches(&self.config.router.freshness_keywords, request);
        if !freshness_matches.is_empty() {
            return Ok(self.decision(
                request,
                RouteTarget::Cloud,
                RoutingReason::FreshnessKeyword,
                freshness_matches,
                estimated_prompt_tokens,
            ));
        }

        let complexity_matches = keyword_matches(&self.config.router.complexity_keywords, request);
        if !complexity_matches.is_empty() {
            return Ok(self.decision(
                request,
                RouteTarget::Cloud,
                RoutingReason::ComplexityKeyword,
                complexity_matches,
                estimated_prompt_tokens,
            ));
        }

        Ok(self.decision(
            request,
            RouteTarget::Local,
            RoutingReason::AutoLocalDefault,
            vec!["default:local".to_string()],
            estimated_prompt_tokens,
        ))
    }

    fn decision(
        &self,
        request: &ChatRequest,
        target: RouteTarget,
        reason: RoutingReason,
        matched_rules: Vec<String>,
        estimated_prompt_tokens: usize,
    ) -> RoutingDecision {
        let runtime_id = match target {
            RouteTarget::Local => &self.config.router.default_local_runtime_id,
            RouteTarget::Cloud => &self.config.router.default_cloud_runtime_id,
        };
        let preferred_model = self
            .config
            .runtime(runtime_id)
            .and_then(|runtime| runtime.model_hint.clone());

        RoutingDecision {
            requested_model: request.model.clone(),
            target,
            runtime_id: runtime_id.clone(),
            backend: target.backend(),
            reason,
            matched_rules,
            estimated_prompt_tokens,
            preferred_runtime_id: runtime_id.clone(),
            preferred_model,
        }
    }
}

fn prompt_text(request: &ChatRequest) -> String {
    request
        .messages
        .iter()
        .map(|message| message.content.as_str())
        .collect::<Vec<_>>()
        .join("\n")
}

fn prompt_char_count(request: &ChatRequest) -> usize {
    prompt_text(request).chars().count()
}

fn estimate_prompt_tokens(request: &ChatRequest) -> usize {
    let chars = prompt_char_count(request);
    let word_count = prompt_text(request).split_whitespace().count();
    let char_estimate = chars.div_ceil(4);
    word_count.max(char_estimate)
}

fn keyword_matches(keywords: &[String], request: &ChatRequest) -> Vec<String> {
    let content = prompt_text(request).to_ascii_lowercase();

    keywords
        .iter()
        .filter(|keyword| !keyword.is_empty())
        .filter(|keyword| content.contains(&keyword.to_ascii_lowercase()))
        .cloned()
        .collect()
}

fn remote_capability_rules(request: &ChatRequest) -> Vec<String> {
    let mut rules = Vec::new();
    if request.features.tools {
        rules.push("tools".to_string());
    }
    if request.features.tool_choice {
        rules.push("tool_choice".to_string());
    }
    if request.features.structured_output {
        rules.push("response_format".to_string());
    }
    rules
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
        assert_eq!(config.router.default_local_runtime_id, "local/llama.cpp");
        assert_eq!(
            config.router.default_cloud_runtime_id,
            "remote/modal-default"
        );
        assert_eq!(config.router.prompt_char_limit, 800);
        assert_eq!(config.router.estimated_prompt_token_limit, 256);
    }

    #[test]
    fn config_validation_rejects_missing_default_runtime() {
        let mut config = SecedaConfig::default();
        config
            .runtimes
            .retain(|runtime| runtime.id != "remote/modal-default");

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

        assert_eq!(routing.target, RouteTarget::Cloud);
        assert_eq!(routing.runtime_id, "remote/modal-default");
        assert_eq!(routing.backend, BackendKind::Cloud);
        assert_eq!(routing.reason, RoutingReason::ForcedCloudModel);
        assert_eq!(routing.matched_rules, vec!["model:remote/default"]);
        assert_eq!(routing.estimated_prompt_tokens, 2);
        assert_eq!(routing.preferred_runtime_id, "remote/modal-default");
    }

    #[test]
    fn router_sends_complex_keyword_matched_auto_request_to_cloud() {
        let config = SecedaConfig::default();
        let request = ChatRequest::new(
            "seceda/default",
            vec![ChatMessage::user("research the latest runtime options")],
        );

        let routing = route_request(&config, &request).expect("routing decision");

        assert_eq!(routing.runtime_id, "remote/modal-default");
        assert_eq!(routing.backend, BackendKind::Cloud);
        assert_eq!(routing.reason, RoutingReason::FreshnessKeyword);
        assert_eq!(routing.matched_rules, vec!["latest"]);
    }

    #[test]
    fn router_sends_tools_and_tool_choice_to_cloud() {
        let config = SecedaConfig::default();
        let request = ChatRequest::new("seceda/default", vec![ChatMessage::user("hello")])
            .with_features(RequestFeatures {
                tools: true,
                tool_choice: true,
                structured_output: false,
            });

        let routing = route_request(&config, &request).expect("routing decision");

        assert_eq!(routing.target, RouteTarget::Cloud);
        assert_eq!(routing.reason, RoutingReason::RemoteCapabilityRequired);
        assert_eq!(routing.matched_rules, vec!["tools", "tool_choice"]);
    }

    #[test]
    fn router_sends_structured_output_capability_to_cloud() {
        let config = SecedaConfig::default();
        let request = ChatRequest::new("seceda/default", vec![ChatMessage::user("hello")])
            .with_features(RequestFeatures {
                structured_output: true,
                ..RequestFeatures::default()
            });

        let routing = route_request(&config, &request).expect("routing decision");

        assert_eq!(routing.target, RouteTarget::Cloud);
        assert_eq!(routing.reason, RoutingReason::RemoteCapabilityRequired);
        assert_eq!(routing.matched_rules, vec!["response_format"]);
    }

    #[test]
    fn router_sends_long_prompt_to_cloud() {
        let mut config = SecedaConfig::default();
        config.router.prompt_char_limit = 16;
        config.router.estimated_prompt_token_limit = 100;
        let request = ChatRequest::new(
            "seceda/default",
            vec![ChatMessage::user("this prompt is definitely too long")],
        );

        let routing = route_request(&config, &request).expect("routing decision");

        assert_eq!(routing.target, RouteTarget::Cloud);
        assert_eq!(routing.reason, RoutingReason::PromptTooLong);
        assert_eq!(routing.matched_rules, vec!["max_prompt_chars"]);
    }

    #[test]
    fn router_sends_high_estimated_token_prompt_to_cloud() {
        let mut config = SecedaConfig::default();
        config.router.prompt_char_limit = 1_000;
        config.router.estimated_prompt_token_limit = 4;
        let request = ChatRequest::new(
            "seceda/default",
            vec![ChatMessage::user("this prompt is above token threshold")],
        );

        let routing = route_request(&config, &request).expect("routing decision");

        assert_eq!(routing.target, RouteTarget::Cloud);
        assert_eq!(routing.reason, RoutingReason::EstimatedTokensTooHigh);
        assert_eq!(routing.matched_rules, vec!["max_estimated_tokens"]);
        assert!(routing.estimated_prompt_tokens > 4);
    }

    #[test]
    fn router_sends_structured_output_keyword_to_cloud() {
        let config = SecedaConfig::default();
        let request = ChatRequest::new(
            "seceda/default",
            vec![ChatMessage::user("return json with a stable schema")],
        );

        let routing = route_request(&config, &request).expect("routing decision");

        assert_eq!(routing.target, RouteTarget::Cloud);
        assert_eq!(routing.reason, RoutingReason::StructuredOutputKeyword);
        assert_eq!(routing.matched_rules, vec!["json", "schema"]);
    }

    #[test]
    fn router_sends_freshness_keyword_to_cloud() {
        let config = SecedaConfig::default();
        let request = ChatRequest::new(
            "seceda/default",
            vec![ChatMessage::user("what happened in the news today?")],
        );

        let routing = route_request(&config, &request).expect("routing decision");

        assert_eq!(routing.target, RouteTarget::Cloud);
        assert_eq!(routing.reason, RoutingReason::FreshnessKeyword);
        assert_eq!(routing.matched_rules, vec!["today", "news"]);
    }

    #[test]
    fn simple_requests_default_to_local() {
        let config = SecedaConfig::default();
        let request = ChatRequest::new("seceda/default", vec![ChatMessage::user("hello")]);

        let routing = route_request(&config, &request).expect("routing decision");

        assert_eq!(routing.target, RouteTarget::Local);
        assert_eq!(routing.runtime_id, "local/llama.cpp");
        assert_eq!(routing.reason, RoutingReason::AutoLocalDefault);
        assert_eq!(routing.matched_rules, vec!["default:local"]);
    }

    #[test]
    fn router_preserves_preferred_model_hint() {
        let mut config = SecedaConfig::default();
        config.router.default_local_runtime_id = "mock-local".to_string();
        config.router.default_cloud_runtime_id = "mock-cloud".to_string();
        config.runtimes = vec![
            RuntimeConfig::new("mock-local", BackendKind::Local, true)
                .with_model_hint("llama/local"),
            RuntimeConfig::new("mock-cloud", BackendKind::Cloud, true)
                .with_model_hint("modal/cloud"),
        ];
        let request = ChatRequest::new("remote/default", vec![ChatMessage::user("hello")]);

        let routing = route_request(&config, &request).expect("routing decision");

        assert_eq!(routing.preferred_runtime_id, "mock-cloud");
        assert_eq!(routing.preferred_model, Some("modal/cloud".to_string()));
    }

    #[test]
    fn token_estimate_matches_previous_word_or_char_heuristic() {
        let request = ChatRequest::new(
            "seceda/default",
            vec![ChatMessage::user("one two three four five")],
        );

        let routing = route_request(&SecedaConfig::default(), &request).expect("routing decision");

        assert_eq!(routing.estimated_prompt_tokens, 6);
    }

    #[test]
    fn core_executes_request_and_returns_observable_result() {
        let mut config = SecedaConfig::default();
        config.router.default_local_runtime_id = "mock-local".to_string();
        config.router.default_cloud_runtime_id = "mock-cloud".to_string();
        config.runtimes = vec![
            RuntimeConfig::new("mock-local", BackendKind::Local, true),
            RuntimeConfig::new("mock-cloud", BackendKind::Cloud, true),
        ];
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
        let mut config = SecedaConfig::default();
        config.router.default_local_runtime_id = "mock-local".to_string();
        config.router.default_cloud_runtime_id = "mock-cloud".to_string();
        config.runtimes = vec![
            RuntimeConfig::new("mock-local", BackendKind::Local, true),
            RuntimeConfig::new("mock-cloud", BackendKind::Cloud, true),
        ];
        let local = MockRuntimeAdapter::new("mock-local", BackendKind::Local);
        let request = ChatRequest::new("remote/default", vec![ChatMessage::user("hello")]);

        let error = execute_with_adapters(&config, &request, &[&local]).expect_err("core error");

        assert_eq!(
            error,
            CoreError::NoAdapterForRuntime("mock-cloud".to_string())
        );
    }
}
