//! What a client's method takes for an argument of the contract's type: [`Arg`].

/// A value a client's method takes for an argument of the contract's type `T`: each method of a group's trait takes
/// its arguments as `impl Arg<T>`, so that a call writes `"rust-lang"` for a `String`, with no `.into()`.
///
/// An argument of type `T` takes:
///
/// - a `T`, the contract's type: `3` for an `i64`, `Some(5)` or `None` for an `Option<u32>`;
/// - a `&str` for a `String`;
/// - a `&T`, which is cloned: `&owner` for a `String`, `&labels` for a `Vec<String>`;
/// - an array or a slice of what an item takes, for a `Vec<T>`: `["C-bug", "T-compiler"]` or `&labels[..]` for a
///   `Vec<String>`.
///
/// No other conversion is made. An `Option<T>` takes `Some(..)` or `None`, and an `Option<String>` takes
/// `Some("x".into())`: taking `Some("x")` too would make `None` ambiguous. A `Vec<String>` takes `["a"]` rather than
/// `vec!["a"]`, for the same reason with `vec![]`. No number becomes another, so that `5` keeps the type of the
/// argument.
///
/// ```no_run
/// quoll::api! {
///     #[openapi(none)]
///     pub api GitHub = "";
///
///     pub group Issues {
///         GET "/repos/{owner}/{repo}/issues?labels={labels}&per_page={per_page}" => list(
///             owner: String,
///             repo: String,
///             #[join(",")] labels: Vec<String>,
///             per_page: Option<u32>,
///         ) -> Vec<serde_json::Value>;
///     }
/// }
///
/// # async fn run() -> quoll::Result<()> {
/// let github = GitHub::client("https://api.github.com")?;
///
/// let issues = github.issues().list("rust-lang", "rust", ["C-bug"], Some(5)).await?;
///
/// let owner = String::from("rust-lang");
/// let labels = vec![String::from("C-bug"), String::from("T-compiler")];
/// let issues = github.issues().list(&owner, "rust", &labels, None).await?;
/// # let _ = issues;
/// # Ok(())
/// # }
/// ```
///
/// A fake of a group's trait takes its arguments as the trait does, and reads each one with
/// [`into_value`](Arg::into_value):
///
/// ```
/// use quoll::{Arg, Call};
///
/// quoll::api! {
///     pub api Api = "/api";
///
///     pub group Notes {
///         GET "/notes/{id}" => get(id: i64) -> String;
///     }
/// }
///
/// struct FakeNotes;
///
/// impl Notes for FakeNotes {
///     fn get(&self, id: impl Arg<i64>) -> impl Call<String> {
///         let id = id.into_value();
///
///         async move {
///             if id == 1 {
///                 return Ok("Milk".to_owned());
///             }
///
///             Err(quoll::Error::not_found("no such note"))
///         }
///     }
/// }
/// ```
///
/// An application may implement it for a type of its own, `Arg<Name> for &str`, which Rust's orphan rule allows.
///
/// A call's type names the types of its arguments, so a call keeps borrowed what they borrow until it is awaited: a
/// call given `&owner`, a `String`, keeps `owner` borrowed, and dropping `owner` before the `.await` does not compile.
#[diagnostic::on_unimplemented(
    message = "`{Self}` is not an argument of type `{T}`",
    label = "not an argument of type `{T}`",
    note = "an argument takes its type in the contract, a `&str` for a `String`, a reference, which is cloned, or an array or a slice for a `Vec`: write `[\"a\"]` rather than `vec![\"a\"]`, and `Some(\"x\".into())` for an `Option<String>`"
)]
pub trait Arg<T>: Sized + Send {
    /// The value of the contract's type that the argument gives.
    fn into_value(self) -> T;
}

impl<T: Send> Arg<T> for T {
    fn into_value(self) -> T {
        self
    }
}

impl Arg<String> for &str {
    fn into_value(self) -> String {
        self.to_owned()
    }
}

impl<T: Clone + Sync> Arg<T> for &T {
    fn into_value(self) -> T {
        self.clone()
    }
}

impl<T, A: Arg<T>, const N: usize> Arg<Vec<T>> for [A; N] {
    fn into_value(self) -> Vec<T> {
        self.into_iter().map(Arg::into_value).collect()
    }
}

impl<T, A: Arg<T> + Clone + Sync> Arg<Vec<T>> for &[A] {
    fn into_value(self) -> Vec<T> {
        self.iter().cloned().map(Arg::into_value).collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn take<T>(arg: impl Arg<T>) -> T {
        arg.into_value()
    }

    #[test]
    fn a_value_of_the_contract_s_type_is_itself() {
        assert_eq!(take::<i64>(3), 3);
        assert_eq!(take::<u32>(5), 5);
        assert_eq!(take::<Option<u32>>(Some(5)), Some(5));
        assert_eq!(take::<Option<String>>(None), None);
        assert_eq!(take::<Vec<String>>(vec![]), Vec::<String>::new());
        assert_eq!(take::<String>(String::from("rust")), "rust");
    }

    #[test]
    fn a_str_is_a_string() {
        assert_eq!(take::<String>("rust"), "rust");
    }

    #[test]
    fn a_reference_is_cloned() {
        let owner = String::from("rust-lang");
        let labels = vec![String::from("C-bug")];

        assert_eq!(take::<String>(&owner), "rust-lang");
        assert_eq!(take::<Vec<String>>(&labels), ["C-bug"]);
        assert_eq!(take::<Option<String>>(&Some(owner.clone())), Some(owner));
    }

    #[test]
    fn an_array_or_a_slice_is_a_vec() {
        let labels = [String::from("C-bug")];
        let names = ["C-bug", "T-compiler"];

        assert_eq!(take::<Vec<String>>(["C-bug", "T-compiler"]), names);
        assert_eq!(take::<Vec<String>>(&names[..]), names);
        assert_eq!(take::<Vec<String>>(&labels[..]), ["C-bug"]);
        assert_eq!(take::<Vec<i64>>([1, 2]), [1, 2]);
    }
}
