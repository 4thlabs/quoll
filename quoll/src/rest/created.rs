//! The result of a route that creates what it returns.

/// What a route answers when it creates `T`: a 201, with `T` in JSON and, when given, a `Location` header.
///
/// ```
/// use quoll::rest::Created;
///
/// struct Note {
///     id: i64,
/// }
///
/// let note = Note { id: 3 };
/// let created = Created::at(format!("/api/notes/{}", note.id), note);
/// # let _ = created;
/// ```
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct Created<T> {
    /// Where the created value is read from, sent as `Location`.
    pub(crate) location: Option<String>,
    /// The created value, the body.
    pub(crate) value: T,
}

impl<T> Created<T> {
    /// A 201 with `value` as its body and no `Location`.
    pub fn new(value: T) -> Self {
        Self {
            location: None,
            value,
        }
    }

    /// A 201 with `value` as its body and `location`, where it is read from, as its `Location`.
    pub fn at(location: impl Into<String>, value: T) -> Self {
        Self {
            location: Some(location.into()),
            value,
        }
    }

    /// Where the created value is read from, sent as `Location`, if it was given: what a test of a handler checks.
    ///
    /// ```
    /// use quoll::rest::Created;
    ///
    /// let created = Created::at("/api/notes/3", 3);
    ///
    /// assert_eq!(created.location(), Some("/api/notes/3"));
    /// assert_eq!(Created::new(3).location(), None);
    /// ```
    pub fn location(&self) -> Option<&str> {
        self.location.as_deref()
    }

    /// The created value, the body.
    ///
    /// ```
    /// use quoll::rest::Created;
    ///
    /// assert_eq!(*Created::new(3).value(), 3);
    /// ```
    pub fn value(&self) -> &T {
        &self.value
    }

    /// The created value, the body, leaving the location.
    ///
    /// ```
    /// use quoll::rest::Created;
    ///
    /// assert_eq!(Created::at("/api/notes/3", 3).into_inner(), 3);
    /// ```
    pub fn into_inner(self) -> T {
        self.value
    }
}
