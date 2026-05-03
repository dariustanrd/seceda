//! llama.cpp runtime integration scaffold.

use seceda_core::BackendKind;

/// Configuration for a future llama.cpp-backed local runtime.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LlamaRuntimeConfig {
    pub model_path: String,
    pub context_size: usize,
}

impl LlamaRuntimeConfig {
    pub fn new(model_path: impl Into<String>) -> Self {
        Self {
            model_path: model_path.into(),
            context_size: 4096,
        }
    }

    pub fn backend_kind(&self) -> BackendKind {
        BackendKind::Local
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn llama_runtime_is_local_backend() {
        let config = LlamaRuntimeConfig::new("models/local.gguf");

        assert_eq!(config.backend_kind(), BackendKind::Local);
        assert_eq!(config.context_size, 4096);
    }
}
