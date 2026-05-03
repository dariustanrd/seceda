//! Core Seceda contracts shared by the server, runtimes, and CLI.

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
}
