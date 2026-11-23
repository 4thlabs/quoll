//! The string of a route, `"/notes/{id}?contains={contains}"`, read into its segments and its query.
//!
//! Both spellings of a contract write the same string, so this reading is shared by them.

use syn::{Error, Ident, LitStr, Result};

/// What a route's string says: where it goes and which names it takes.
#[derive(Debug, PartialEq, Eq)]
pub(crate) struct Template {
    /// The segments below the api's prefix, in order. `"/"` has none.
    pub(crate) segments: Vec<Segment>,
    /// The `key={name}` pairs of the query, in order.
    pub(crate) query: Vec<QueryItem>,
}

/// One segment of a path.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum Segment {
    /// Text that must match as written.
    Literal(String),
    /// A `{name}`, the name of an argument.
    Param(String),
}

/// A `key={name}` of the query.
#[derive(Debug, PartialEq, Eq)]
pub(crate) struct QueryItem {
    /// The key on the wire.
    pub(crate) key: String,
    /// The name of the argument that takes its value.
    pub(crate) name: String,
}

impl Template {
    /// Reads the string of a route.
    ///
    /// # Errors
    ///
    /// An error spanned on the string, when it does not start with `/`, has an empty segment, a `{name}` that is not a
    /// whole segment or a query item that is not `key={name}`, or names an argument twice.
    pub(crate) fn parse(lit: &LitStr) -> Result<Self> {
        let value = lit.value();
        let fail = |message: String| Error::new(lit.span(), message);

        let Some(rest) = value.strip_prefix('/') else {
            return Err(fail(format!(
                "a route's path starts with `/`: write \"/{value}\""
            )));
        };
        let (path, query) = match rest.split_once('?') {
            Some((path, query)) => (path, Some(query)),
            None => (rest, None),
        };

        let segments = parse_segments(path).map_err(&fail)?;
        let query = match query {
            Some(query) => parse_query(query).map_err(&fail)?,
            None => Vec::new(),
        };
        let template = Self { segments, query };
        template.check_names_once().map_err(&fail)?;

        Ok(template)
    }

    /// The names of the path's `{name}`s, in order.
    pub(crate) fn path_names(&self) -> impl Iterator<Item = &str> {
        self.segments.iter().filter_map(|segment| match segment {
            Segment::Param(name) => Some(name.as_str()),
            Segment::Literal(_) => None,
        })
    }

    fn check_names_once(&self) -> std::result::Result<(), String> {
        let mut keys: Vec<&str> = Vec::new();
        for item in &self.query {
            if keys.contains(&item.key.as_str()) {
                return Err(format!(
                    "the query key `{}` appears twice: a key takes one argument",
                    item.key
                ));
            }
            keys.push(&item.key);
        }

        let mut seen: Vec<&str> = Vec::new();
        let query_names = self.query.iter().map(|item| item.name.as_str());

        for name in self.path_names().chain(query_names) {
            if seen.contains(&name) {
                return Err(format!(
                    "`{{{name}}}` appears twice in the string: each argument takes one place, in the path or in the query"
                ));
            }
            seen.push(name);
        }

        Ok(())
    }
}

fn parse_segments(path: &str) -> std::result::Result<Vec<Segment>, String> {
    if path.is_empty() {
        return Ok(Vec::new());
    }

    path.split('/').map(parse_segment).collect()
}

fn parse_segment(text: &str) -> std::result::Result<Segment, String> {
    if text.is_empty() {
        return Err(
            "a path has an empty segment (`//` or a final `/`): write \"/notes/{id}\", not \"/notes//{id}\" or \"/notes/\""
                .into(),
        );
    }
    if let Some(name) = text
        .strip_prefix('{')
        .and_then(|inner| inner.strip_suffix('}'))
    {
        return valid_name(name).map(|name| Segment::Param(name.into()));
    }
    if text.contains(['{', '}']) {
        return Err(format!(
            "`{text}` mixes text and a `{{name}}`: a `{{name}}` is a whole segment, as in \"/notes/{{id}}\""
        ));
    }
    if let Some(problem) = literal_problem(text) {
        return Err(problem);
    }

    Ok(Segment::Literal(text.into()))
}

/// What is wrong with a literal segment, if anything. A router reads other characters as syntax of its own (`:x`, `*x`)
/// or as escapes, so a literal is limited to the characters every router and client agree on.
pub(crate) fn literal_problem(text: &str) -> Option<String> {
    let plain = |c: char| c.is_ascii_alphanumeric() || matches!(c, '-' | '_' | '.' | '~');
    if text.chars().all(plain) && text != "." && text != ".." {
        return None;
    }

    Some(format!(
        "`{text}` is not a plain segment: a literal segment holds ASCII letters, digits, `-`, `_`, `.` and `~`, and is not `.` or `..`"
    ))
}

fn parse_query(query: &str) -> std::result::Result<Vec<QueryItem>, String> {
    if query.is_empty() {
        return Err(
            "the query is empty: remove the `?`, or write `key={name}` pairs after it, as in \"/notes?page={page}\""
                .into(),
        );
    }

    query.split('&').map(parse_query_item).collect()
}

fn parse_query_item(item: &str) -> std::result::Result<QueryItem, String> {
    let expected = || {
        format!(
            "`{item}` is not a `key={{name}}` pair: write the key, then the argument that takes it in braces, as in `page={{page}}`"
        )
    };
    let Some((key, value)) = item.split_once('=') else {
        return Err(expected());
    };
    let Some(name) = value
        .strip_prefix('{')
        .and_then(|inner| inner.strip_suffix('}'))
    else {
        return Err(expected());
    };
    if key.is_empty() || key.contains(['{', '}']) {
        return Err(expected());
    }
    if let Some(problem) = key_problem(key) {
        return Err(problem);
    }

    Ok(QueryItem {
        key: key.into(),
        name: valid_name(name)?.into(),
    })
}

/// What is wrong with a literal key of the query, if anything. A key is what a literal segment holds, then brackets
/// that hold the same or nothing, `expand[]`, `filter[name]`, `a[b][c]`: the keys of Rails, Stripe and JSON:API, which
/// serde_html_form reads and writes as any other, escaped as `%5B` and `%5D`, as a browser's form writes them.
fn key_problem(key: &str) -> Option<String> {
    let plain = |c: char| c.is_ascii_alphanumeric() || matches!(c, '-' | '_' | '.' | '~');
    let (base, mut brackets) = key.split_at(key.find('[').unwrap_or(key.len()));
    let mut well_formed = !base.is_empty() && literal_problem(base).is_none();
    while well_formed && !brackets.is_empty() {
        let inner = brackets
            .strip_prefix('[')
            .and_then(|rest| rest.split_once(']'));
        let Some((inside, rest)) = inner else {
            well_formed = false;
            break;
        };

        well_formed = inside.chars().all(plain);
        brackets = rest;
    }
    if well_formed {
        return None;
    }

    Some(format!(
        "`{key}` is not a plain key: a literal key holds ASCII letters, digits, `-`, `_`, `.` and `~`, is not `.` or `..`, and may end in brackets that hold the same or nothing, as in `expand[]` or `filter[name]`"
    ))
}

fn valid_name(name: &str) -> std::result::Result<&str, String> {
    let mut chars = name.chars();
    let starts_well = chars
        .next()
        .is_some_and(|c| c.is_ascii_alphabetic() || c == '_');
    // Parsing as an identifier refuses what an argument cannot be named: `_` and the keywords.
    let is_ident = syn::parse_str::<Ident>(name).is_ok();
    if starts_well && chars.all(|c| c.is_ascii_alphanumeric() || c == '_') && is_ident {
        return Ok(name);
    }

    Err(format!(
        "`{{{name}}}` is not the name of an argument: a name is ASCII letters, digits and `_`, is not a keyword, and is written as the argument is, as in `{{id}}`"
    ))
}

#[cfg(test)]
mod tests {
    use proc_macro2::Span;

    use super::*;

    fn parse(text: &str) -> Result<Template> {
        Template::parse(&LitStr::new(text, Span::call_site()))
    }

    fn message(text: &str) -> String {
        parse(text).expect_err("the string is wrong").to_string()
    }

    #[test]
    fn root_has_no_segment() {
        let template = parse("/").unwrap();

        assert!(template.segments.is_empty());
        assert!(template.query.is_empty());
    }

    #[test]
    fn segments_and_query_are_read_in_order() {
        let template = parse("/repos/{owner}/issues?state={state}&page={page}").unwrap();

        assert_eq!(
            template.segments,
            [
                Segment::Literal("repos".into()),
                Segment::Param("owner".into()),
                Segment::Literal("issues".into()),
            ]
        );
        let query: Vec<_> = template
            .query
            .iter()
            .map(|item| (item.key.as_str(), item.name.as_str()))
            .collect();
        assert_eq!(query, [("state", "state"), ("page", "page")]);
    }

    #[test]
    fn the_key_may_differ_from_the_name() {
        let template = parse("/notes?q={contains}").unwrap();

        assert_eq!(template.query[0].key, "q");
        assert_eq!(template.query[0].name, "contains");
    }

    #[test]
    fn a_path_starts_with_a_slash() {
        assert!(message("notes").contains("starts with `/`"));
    }

    #[test]
    fn empty_segments_are_refused() {
        assert!(message("/notes/").contains("empty segment"));
        assert!(message("/notes//x").contains("empty segment"));
    }

    #[test]
    fn a_param_is_a_whole_segment() {
        assert!(message("/notes/id-{id}").contains("whole segment"));
        assert!(message("/notes/{id").contains("whole segment"));
    }

    #[test]
    fn names_are_checked() {
        assert!(message("/notes/{}").contains("not the name"));
        assert!(message("/notes/{1}").contains("not the name"));
        assert!(message("/notes/{a-b}").contains("not the name"));
    }

    #[test]
    fn query_items_are_key_then_name() {
        assert!(message("/notes?page").contains("not a `key={name}` pair"));
        assert!(message("/notes?page=1").contains("not a `key={name}` pair"));
        assert!(message("/notes?={page}").contains("not a `key={name}` pair"));
        assert!(message("/notes?").contains("query is empty"));
        assert!(message("/notes?a={a}&").contains("not a `key={name}` pair"));
    }

    #[test]
    fn a_query_key_is_taken_once() {
        assert!(message("/notes?tag={a}&tag={b}").contains("query key `tag` appears twice"));
    }

    #[test]
    fn literal_segments_are_plain() {
        for text in [
            "/a/./b",
            "/a/..",
            "/:id",
            "/*rest",
            "/50%25",
            "/my notes",
            "/a#b",
            "/é",
        ] {
            assert!(message(text).contains("not a plain segment"), "{text}");
        }
        for text in ["/a.b", "/v1.2-beta_3", "/~user", "/.well-known"] {
            assert!(parse(text).is_ok(), "{text}");
        }
        assert!(message("/notes?a%b={x}").contains("not a plain key"));
    }

    #[test]
    fn a_literal_key_may_end_in_brackets() {
        for (text, key) in [
            ("/customers?expand[]={expand}", "expand[]"),
            ("/articles?filter[name]={name}", "filter[name]"),
            ("/articles?page[size]={size}", "page[size]"),
            ("/a?a[b][c]={x}", "a[b][c]"),
            ("/a?a[][b]={x}", "a[][b]"),
            ("/a?v1.2[-_.~]={x}", "v1.2[-_.~]"),
        ] {
            let template = parse(text).unwrap_or_else(|error| panic!("{text}: {error}"));

            assert_eq!(template.query[0].key, key);
        }
    }

    #[test]
    fn the_brackets_of_a_key_are_well_formed() {
        for key in [
            "filter[name",
            "filter]",
            "filter]name[",
            "[name]",
            "[]",
            "a[b]c",
            "a[[b]]",
            "a[b[c]]",
            "a[b c]",
            "a[b%5D]",
            "..[x]",
            "a[b]]",
        ] {
            assert!(
                message(&format!("/a?{key}={{x}}"))
                    .contains("may end in brackets that hold the same or nothing"),
                "{key}"
            );
        }
    }

    #[test]
    fn a_bracketed_key_is_taken_once() {
        assert!(
            message("/a?expand[]={a}&expand[]={b}").contains("query key `expand[]` appears twice")
        );
        assert!(parse("/a?page[size]={size}&page[number]={number}").is_ok());
    }

    #[test]
    fn names_are_what_an_argument_can_bear() {
        for name in ["_", "self", "Self", "crate", "super", "type", "é", "a b"] {
            assert!(
                message(&format!("/notes/{{{name}}}")).contains("not the name"),
                "{name}"
            );
        }
        assert!(parse("/notes/{_id}").is_ok());
    }

    #[test]
    fn a_name_is_taken_once() {
        assert!(message("/notes/{id}/{id}").contains("appears twice"));
        assert!(message("/notes/{id}?id={id}").contains("appears twice"));
        assert!(message("/notes?a={x}&b={x}").contains("appears twice"));
    }
}
