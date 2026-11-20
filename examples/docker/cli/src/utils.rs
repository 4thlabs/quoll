use docker_api::DockerError;
use quoll::rest;

/// The 12 first characters of an id, as Docker shows them.
pub fn short_id(id: &str) -> &str {
    id.get(..12).unwrap_or(id)
}

/// `error`, said with Docker's own message when the daemon gave one: Docker answers `{"message": "..."}`, not an RFC
/// 9457 problem, which quoll's `Display` would show.
pub fn docker_error(error: rest::Error) -> anyhow::Error {
    match error.body_as::<DockerError>() {
        Some(DockerError { message }) => anyhow::Error::new(error).context(message),
        None => error.into(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn an_id_is_shown_by_its_first_12_characters() {
        assert_eq!(short_id("0123456789abcdef"), "0123456789ab");
        assert_eq!(short_id("short"), "short");
    }
}
