//! Server boundary for Seceda's OpenAI-compatible localhost API.

use seceda_core::{BackendKind, ModelDescriptor};

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

#[cfg(test)]
mod tests {
    use super::*;

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
}
