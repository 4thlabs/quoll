//! The result of a route that accepts work it has not done yet.

/// What a route answers when it accepts work to do later: a 202, with `T` in JSON, which says what was accepted, as a
/// job and where to follow it.
///
/// ```
/// use quoll::rest::Accepted;
///
/// struct Export {
///     job: u64,
/// }
///
/// let accepted = Accepted::new(Export { job: 7 });
/// # let _ = accepted;
/// ```
///
/// On the client, the route returns the `T`: the 202 is only the server's answer.
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct Accepted<T> {
    /// What was accepted, the body.
    pub(crate) value: T,
}

impl<T> Accepted<T> {
    /// A 202 with `value` as its body.
    pub fn new(value: T) -> Self {
        Self { value }
    }
}
