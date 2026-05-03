//! Cloud provider runtime organization for Seceda.
//!
//! Provider-specific transport, auth, request transforms, and backend details
//! belong here or in child crates under this package family. `seceda-core`
//! should only see portable runtime identifiers, backend kinds, model hints,
//! and capability metadata.

use seceda_core::{BackendKind, RuntimeCapabilities, RuntimeConfig};
use serde::{Deserialize, Serialize};
use std::env;
use std::error::Error;
use std::fmt;
use std::fs;
use std::io;
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
}
