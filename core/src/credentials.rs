/// Future OS-keychain providers implement this interface. No secret belongs to a Profile.
pub trait CredentialProvider {
    fn password(&self, profile_id: &str) -> Result<Option<Secret>, String>;
}

pub struct Secret(Vec<u8>);
impl Secret {
    pub fn new(bytes: Vec<u8>) -> Self { Self(bytes) }
    pub fn expose(&self) -> &[u8] { &self.0 }
    pub(crate) fn expose_mut(&mut self) -> &mut [u8] { &mut self.0 }
    pub(crate) fn truncate(&mut self, length: usize) {
        if length < self.0.len() { self.0[length..].fill(0); self.0.truncate(length); }
    }
}
impl std::fmt::Debug for Secret {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result { f.write_str("Secret([REDACTED])") }
}
impl Drop for Secret {
    fn drop(&mut self) { self.0.fill(0); } // Best-effort; not a locked/guaranteed-zeroized allocation.
}

pub struct UnavailableCredentials;
impl CredentialProvider for UnavailableCredentials {
    fn password(&self, _: &str) -> Result<Option<Secret>, String> { Ok(None) }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn secret_debug_is_redacted() { assert!(!format!("{:?}", Secret::new(b"PASSWORD".to_vec())).contains("PASSWORD")); }
}
