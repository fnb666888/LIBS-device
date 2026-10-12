use serde::Serialize;
use serde_json::{Value, json};

#[derive(Debug, Clone, Serialize)]
pub struct Error {
    pub kind: &'static str,
    pub code: String,
    pub message: String,
    #[serde(skip)]
    payload: Option<Value>,
}

pub type Result<T> = std::result::Result<T, Error>;

impl Error {
    pub fn new(kind: &'static str, code: impl Into<String>, message: impl Into<String>) -> Self {
        Self {
            kind,
            code: code.into(),
            message: message.into(),
            payload: None,
        }
    }
    pub fn invalid(code: impl Into<String>, message: impl Into<String>) -> Self {
        Self::new("invalid_argument", code, message)
    }
    pub fn response(&self) -> Value {
        let mut response = json!({"ok": false, "payload": {"error": self}, "error": self.message,
            "error_kind": self.kind, "error_code": self.code});
        if let Some(Value::Object(extra)) = &self.payload {
            response["payload"]
                .as_object_mut()
                .unwrap()
                .extend(extra.clone());
        }
        response
    }
    pub fn with_payload(mut self, payload: Value) -> Self {
        self.payload = Some(payload);
        self
    }
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "[{}/{}] {}", self.kind, self.code, self.message)
    }
}
impl std::error::Error for Error {}

pub fn response(result: Result<Value>) -> Value {
    match result {
        Ok(payload) => json!({"ok": true, "payload": payload}),
        Err(error) => error.response(),
    }
}
