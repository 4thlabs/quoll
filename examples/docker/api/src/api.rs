use quoll::{ByteStream, JsonLines};

use crate::{
    ContainerInspect, ContainerSummary, CreatedContainer, DockerError, Event, NewContainer,
    PullProgress, Version,
};

quoll::api! {
    /// The Docker Engine API, version 1.47, which Docker 27.3 and later serve.
    #[openapi(none)]
    #[error(DockerError)]
    pub api Docker = "/v1.47";

    pub group Containers {
        GET    "/containers/json?all={all}" => list(all: bool) -> Vec<ContainerSummary>;
        GET    "/containers/{id}/json"      => inspect(id: &str) -> ContainerInspect;
        /// Answers 304 when the container is running already.
        POST   "/containers/{id}/start"     => start(id: &str);
        /// Answers 304 when the container is stopped already.
        POST   "/containers/{id}/stop?t={t}" => stop(id: &str, t: Option<i32>);
        /// Multiplexed frames, `application/vnd.docker.multiplexed-stream`, or the bytes as they are for a container
        /// with a TTY, `application/vnd.docker.raw-stream`.
        GET    "/containers/{id}/logs?follow={follow}&stdout={stdout}&stderr={stderr}"
               => logs(id: &str, follow: bool, stdout: bool, stderr: bool) -> ByteStream;
        POST   "/containers/create?name={name}" => create(name: Option<&str>, container: NewContainer) -> CreatedContainer;
        DELETE "/containers/{id}?force={force}" => remove(id: &str, force: bool);
    }

    pub group Images {
        /// Progress sent as `application/json`, one value per line: a failure is a line with an `error`.
        POST "/images/create?fromImage={image}&tag={tag}" => pull(image: &str, tag: &str) -> JsonLines<PullProgress>;
    }

    pub group System {
        GET "/version" => version() -> Version;
        /// Sent as `application/jsonl` by Docker 29 and later, as `application/json` before.
        GET "/events?since={since}" => events(since: Option<i64>) -> JsonLines<Event>;
    }
}
