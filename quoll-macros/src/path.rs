//! The string of a route, `"/notes/{id}?contains={contains}"`, read into its segments and its query.
//!
//! Both spellings of a contract write the same string, so this reading is shared by them.

use syn::{Error, LitStr, Result};

/// What a route's string says: where it goes and which names it takes.
#[derive(Debug, PartialEq, Eq)]
pub struct Template {
    /// The segments below the api's prefix, in order. `"/"` has none.
    pub segments: Vec<Segment>,
    /// The `key={name}` pairs of the query, in order.
    pub query: Vec<QueryItem>,
}

/// One segment of a path.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Segment {
    /// Text that must match as written.
    Literal(String),
    /// A `{name}`, the name of an argument.
    Param(String),
}

/// A `key={name}` of the query.
#[derive(Debug, PartialEq, Eq)]
pub struct QueryItem {
    /// The key on the wire.
    pub key: String,
    /// The name of the argument that takes its value.
    pub name: String,
}

impl Template {
    /// Reads the string of a route.
    ///
    /// # Errors
    ///
    /// An error spanned on the string, when it does not start with `/`, has an empty segment, a `{name}` that is not a
    /// whole segment or a query item that is not `key={name}`, or names an argument twice.
    pub fn parse(lit: &LitStr) -> Result<Self> {
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
    pub fn path_names(&self) -> impl Iterator<Item = &str> {
        self.segments.iter().filter_map(|segment| match segment {
            Segment::Param(name) => Some(name.as_str()),
            Segment::Literal(_) => None,
        })
    }

    fn check_names_once(&self) -> std::result::Result<(), String> {
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
    if text.contains(|c: char| c.is_whitespace() || c == '#') {
        return Err(format!(
            "`{text}` has a space or a `#`: a literal segment is plain text, as in \"/notes\""
        ));
    }

    Ok(Segment::Literal(text.into()))
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

    Ok(QueryItem {
        key: key.into(),
        name: valid_name(name)?.into(),
    })
}

fn valid_name(name: &str) -> std::result::Result<&str, String> {
    let mut chars = name.chars();
    let starts_well = chars.next().is_some_and(|c| c.is_alphabetic() || c == '_');
    if starts_well && chars.all(|c| c.is_alphanumeric() || c == '_') {
        return Ok(name);
    }

    Err(format!(
        "`{{{name}}}` is not the name of an argument: a name is letters, digits and `_`, as in `{{id}}`"
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
    fn a_name_is_taken_once() {
        assert!(message("/notes/{id}/{id}").contains("appears twice"));
        assert!(message("/notes/{id}?id={id}").contains("appears twice"));
        assert!(message("/notes?a={x}&b={x}").contains("appears twice"));
    }
}
