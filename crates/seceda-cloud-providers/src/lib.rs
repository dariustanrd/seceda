//! Cloud provider runtime organization for Seceda.
//!
//! Provider-specific transport, auth, request transforms, and backend details
//! belong here or in child crates under this package family. `seceda-core`
//! should only see portable runtime identifiers, backend kinds, model hints,
//! and capability metadata.

use seceda_core::{BackendKind, RuntimeCapabilities, RuntimeConfig};

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
}
