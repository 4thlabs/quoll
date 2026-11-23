//! The comments the macro writes on the items it generates: the contract's lines as `#[doc]` attributes, and a route's
//! method and path, with the schemes it takes.

use proc_macro2::TokenStream;
use quote::quote;
use syn::ext::IdentExt;

use crate::model::Route;

/// `#[doc = ...]` attributes, one per line.
pub(crate) fn doc_attrs(lines: &[String]) -> TokenStream {
    quote! { #(#[doc = #lines])* }
}

/// The comments of a route's method, on the server's trait and the client's: the contract's, then the method and the
/// path, `GET /api/notes/{id}` under the prefix of the api of the block, or below the prefix of the api that lists
/// the group, and the scheme it takes.
pub(crate) fn route_docs(prefix: Option<&str>, route: &Route) -> TokenStream {
    let mut line = format!(
        " `{} {}`",
        route.method.as_str(),
        shown_path(prefix.unwrap_or_default(), route)
    );
    if prefix.is_none() {
        line.push_str(", under the prefix of the api");
    }
    if let Some(auth) = &route.auth {
        let words: Vec<String> = auth
            .schemes
            .iter()
            .map(|scheme| format!("`{}`", scheme.word()))
            .collect();

        line.push_str(&format!(", authenticated with {}", words.join(" or ")));
        if auth.optional {
            line.push_str(", or by no credential");
        }
        if !auth.scopes.is_empty() {
            let scopes: Vec<String> = auth
                .scopes
                .iter()
                .map(|scope| format!("`{scope}`"))
                .collect();
            let noun = if scopes.len() == 1 { "scope" } else { "scopes" };

            line.push_str(&format!(
                ", for a caller with the {noun} {}",
                scopes.join(" and ")
            ));
        }
    }

    if !route.fields.is_empty() {
        let fields: Vec<String> = route
            .fields
            .iter()
            .map(|field| format!("`{:?}: {}`", field.name.value(), field.value))
            .collect();

        line.push_str(&format!(", its body holding {}", fields.join(" and ")));
    }

    let mut lines = route.docs.0.clone();
    if !lines.is_empty() {
        lines.push(String::new());
    }

    lines.push(line);

    doc_attrs(&lines)
}

/// The path with its query, as the docs show it: `/api/notes?page={page}`.
fn shown_path(prefix: &str, route: &Route) -> String {
    let mut path = route.full_path(prefix);
    let query = route
        .query_params()
        .map(|(param, key)| format!("{key}={{{}}}", param.name.unraw()));

    for (index, pair) in query.enumerate() {
        path.push(if index == 0 { '?' } else { '&' });
        path.push_str(&pair);
    }

    path
}
