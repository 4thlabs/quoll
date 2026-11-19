//! The spelling of `rest::api!`: a block of items, read into the model.
//!
//! ```text
//! /// Notes, kept per user.
//! pub api Api = "/api";
//!
//! /// The signed-in user's notes.
//! pub group Notes {
//!     /// One note.
//!     GET "/notes/{id}" => get(id: i64) -> Note;
//! }
//! ```

use syn::{
    Attribute, Error, Expr, Ident, Lit, LitStr, Meta, Result, Token, Type, Visibility, braced,
    ext::IdentExt,
    parenthesized,
    parse::{Parse, ParseStream},
    punctuated::Punctuated,
};

use crate::model::{Api, Docs, ErrorList, Group, Method, Route, RouteDecl};

/// What the macro is given: one api, with its groups.
#[derive(Debug)]
pub struct Contract(pub Api);

impl Parse for Contract {
    fn parse(input: ParseStream) -> Result<Self> {
        let mut api: Option<ApiDecl> = None;
        let mut groups = Vec::new();
        let mut errors = ErrorList::default();

        while !input.is_empty() {
            let docs = parse_docs(input)?;
            let vis: Visibility = input.parse()?;
            let keyword = input.call(Ident::parse_any).map_err(|error| {
                Error::new(
                    error.span(),
                    "expected `api` or `group`: an item of a contract starts with one of them",
                )
            })?;

            if keyword == "api" {
                let decl = ApiDecl::parse(input, docs, vis, &keyword)?;
                if let Some(first) = &api {
                    errors.push(Error::new(
                        keyword.span(),
                        format!(
                            "the contract has one api, and `{}` is already it: remove this one",
                            first.name
                        ),
                    ));
                } else {
                    api = Some(decl);
                }
            } else if keyword == "group" {
                match parse_group(input, docs, vis, &mut errors) {
                    Ok(group) => groups.push(group),
                    Err(error) => return Err(combined(errors, error)),
                }
            } else {
                return Err(combined(errors, unexpected_item(&keyword)));
            }
        }

        let Some(api) = api else {
            return Err(combined(
                errors,
                Error::new(
                    proc_macro2::Span::call_site(),
                    "a contract names its api: write `pub api Api = \"/api\";`",
                ),
            ));
        };
        let built = Api::new(api.docs, api.vis, api.name, &api.prefix, groups);
        match built {
            Ok(api) => errors.finish().map(|()| Self(api)),
            Err(error) => Err(combined(errors, error)),
        }
    }
}

fn unexpected_item(keyword: &Ident) -> Error {
    let hint = if Method::from_name(&keyword.to_string()).is_some() {
        "a route goes inside a group: `pub group Name { ... }`"
    } else {
        "an item of a contract starts with `api` or `group`"
    };

    Error::new(keyword.span(), format!("unexpected `{keyword}`: {hint}"))
}

/// All the errors gathered so far, then `last`.
fn combined(errors: ErrorList, last: Error) -> Error {
    let mut all = errors;
    all.push(last);

    all.finish().expect_err("an error was just pushed")
}

struct ApiDecl {
    docs: Docs,
    vis: Visibility,
    name: Ident,
    prefix: LitStr,
}

impl ApiDecl {
    /// After the `api` keyword: `Name = "/prefix";`.
    fn parse(input: ParseStream, docs: Docs, vis: Visibility, keyword: &Ident) -> Result<Self> {
        let name: Ident = input.parse().map_err(|error| {
            Error::new(
                error.span(),
                format!(
                    "`{keyword}` takes the name of the api's type: write `api Api = \"/api\";`"
                ),
            )
        })?;
        input.parse::<Token![=]>()?;
        let prefix: LitStr = input.parse().map_err(|error| {
            Error::new(
                error.span(),
                "the api takes its prefix as a string: write `api Api = \"/api\";`",
            )
        })?;
        input.parse::<Token![;]>()?;

        Ok(Self {
            docs,
            vis,
            name,
            prefix,
        })
    }
}

/// After the `group` keyword: `Name { routes }`. A mistake in one route is gathered, the others are still read.
fn parse_group(
    input: ParseStream,
    docs: Docs,
    vis: Visibility,
    errors: &mut ErrorList,
) -> Result<Group> {
    let name: Ident = input.parse().map_err(|error| {
        Error::new(
            error.span(),
            "`group` takes a name: write `group Notes { ... }`",
        )
    })?;
    let content;
    braced!(content in input);

    let mut routes = Vec::new();
    while !content.is_empty() {
        match parse_route(&content) {
            Ok(Ok(route)) => routes.push(route),
            Ok(Err(error)) => errors.push(error),
            Err(error) => return Err(error),
        }
    }

    Ok(Group {
        docs,
        vis,
        name,
        routes,
    })
}

/// One route. The outer error is a mistake of syntax, after which nothing can be read; the inner one a mistake of the
/// route's meaning, which leaves the next route readable.
fn parse_route(input: ParseStream) -> Result<Result<Route>> {
    let docs = parse_docs(input)?;
    let method = parse_method(input)?;
    let path: LitStr = input.parse().map_err(|error| {
        Error::new(error.span(), "a route's path is a string after its method: write `GET \"/notes/{id}\" => get(id: i64);`")
    })?;
    input.parse::<Token![=>]>()?;
    let name: Ident = input.parse().map_err(|error| {
        Error::new(
            error.span(),
            "`=>` is followed by the name of the route's function: write `=> get(id: i64)`",
        )
    })?;

    let content;
    parenthesized!(content in input);
    let args = Punctuated::<Arg, Token![,]>::parse_terminated(&content)?;
    let reply = if input.peek(Token![->]) {
        input.parse::<Token![->]>()?;
        Some(input.parse::<Type>()?)
    } else {
        None
    };
    input.parse::<Token![;]>()?;

    Ok(Route::resolve(RouteDecl {
        docs,
        method,
        path,
        name,
        args: args.into_iter().map(|arg| (arg.name, arg.ty)).collect(),
        reply,
    }))
}

fn parse_method(input: ParseStream) -> Result<Method> {
    let expected = || {
        let names: Vec<_> = Method::ALL.iter().map(|method| method.as_str()).collect();
        format!("expected a method, one of {}", names.join(", "))
    };
    let ident = input
        .call(Ident::parse_any)
        .map_err(|error| Error::new(error.span(), expected()))?;

    Method::from_name(&ident.to_string()).ok_or_else(|| Error::new(ident.span(), expected()))
}

struct Arg {
    name: Ident,
    ty: Type,
}

impl Parse for Arg {
    fn parse(input: ParseStream) -> Result<Self> {
        let name: Ident = input
            .parse()
            .map_err(|error| Error::new(error.span(), "an argument is written `name: Type`"))?;
        input.parse::<Token![:]>().map_err(|error| {
            Error::new(
                error.span(),
                format!("expected `:` after `{name}`: an argument is written `name: Type`"),
            )
        })?;

        Ok(Self {
            name,
            ty: input.parse()?,
        })
    }
}

/// The `///` comments before an item. Any other attribute is refused: only the comments are read.
fn parse_docs(input: ParseStream) -> Result<Docs> {
    let attrs = input.call(Attribute::parse_outer)?;
    let mut lines = Vec::with_capacity(attrs.len());

    for attr in &attrs {
        let line = match &attr.meta {
            Meta::NameValue(meta) if meta.path.is_ident("doc") => match &meta.value {
                Expr::Lit(expr) => match &expr.lit {
                    Lit::Str(line) => Some(line.value()),
                    _ => None,
                },
                _ => None,
            },
            _ => None,
        };
        let Some(line) = line else {
            return Err(Error::new_spanned(
                attr,
                "an attribute is not supported here: only `///` comments are kept, on an api, a group or a route",
            ));
        };
        lines.push(line);
    }

    Ok(Docs(lines))
}

#[cfg(test)]
mod tests {
    use quote::{ToTokens, quote};

    use super::*;
    use crate::model::{ParamKind, Reply};
    use crate::path::Segment;

    fn parse(tokens: proc_macro2::TokenStream) -> Result<Api> {
        syn::parse2::<Contract>(tokens).map(|contract| contract.0)
    }

    fn messages(tokens: proc_macro2::TokenStream) -> Vec<String> {
        parse(tokens)
            .expect_err("the contract is wrong")
            .into_iter()
            .map(|error| error.to_string())
            .collect()
    }

    fn text(ty: &impl ToTokens) -> String {
        ty.to_token_stream().to_string()
    }

    #[test]
    fn the_notes_contract_is_read_into_its_model() {
        let api = parse(quote! {
            /// Notes, kept per user.
            pub api Api = "/api";

            pub group Auth {
                /// Trades a login for a token.
                POST "/token" => token(login: Login) -> TokenResponse;
            }

            /// The signed-in user's notes.
            pub group Notes {
                GET "/notes?contains={contains}&page={page}"
                    => list(contains: Option<String>, page: Option<u32>) -> Vec<Note>;
                GET "/notes/{id}" => get(id: i64) -> Note;
                POST "/notes" => create(note: NewNote) -> Created<Note>;
                DELETE "/notes/{id}" => remove(id: i64);
            }
        })
        .unwrap();

        assert_eq!(api.name, "Api");
        assert_eq!(api.prefix, "/api");
        assert_eq!(api.docs.0, [" Notes, kept per user."]);
        assert_eq!(api.groups.len(), 2);

        let notes = &api.groups[1];
        assert_eq!(notes.docs.0, [" The signed-in user's notes."]);
        assert_eq!(notes.routes.len(), 4);

        assert_eq!(text(&notes.vis), "pub");

        let list = &notes.routes[0];
        assert!(list.docs.0.is_empty());
        assert_eq!(text(&list.params[1].ty), "Option < u32 >");
        assert_eq!(list.method, Method::Get);
        assert_eq!(list.name, "list");
        assert!(list.path.iter().eq([&Segment::Literal("notes".into())]));
        assert_eq!(
            list.params
                .iter()
                .map(|param| param.kind.clone())
                .collect::<Vec<_>>(),
            [
                ParamKind::Query {
                    key: "contains".into()
                },
                ParamKind::Query { key: "page".into() },
            ]
        );
        assert!(list.body().is_none());
        assert!(matches!(&list.reply, Reply::Json(ty) if text(ty) == "Vec < Note >"));

        let create = &notes.routes[2];
        assert!(matches!(&create.reply, Reply::Created(ty) if text(ty) == "Note"));
        assert_eq!(
            create.body().map(|body| body.name.to_string()),
            Some("note".into())
        );

        let token = &api.groups[0].routes[0];
        assert_eq!(token.docs.0, [" Trades a login for a token."]);

        let remove = &notes.routes[3];
        assert!(matches!(remove.reply, Reply::None));
        assert_eq!(remove.params[0].kind, ParamKind::Path);
    }

    #[test]
    fn arguments_are_matched_by_name_not_by_position() {
        let api = parse(quote! {
            pub api Api = "/api";
            pub group Repos {
                PUT "/repos/{owner}/{repo}?force={force}"
                    => update(patch: Patch, force: bool, repo: String, owner: String);
            }
        })
        .unwrap();

        let kinds: Vec<_> = api.groups[0].routes[0]
            .params
            .iter()
            .map(|param| (param.name.to_string(), param.kind.clone()))
            .collect();
        assert_eq!(
            kinds,
            [
                ("patch".into(), ParamKind::Body),
                (
                    "force".into(),
                    ParamKind::Query {
                        key: "force".into()
                    }
                ),
                ("repo".into(), ParamKind::Path),
                ("owner".into(), ParamKind::Path),
            ]
        );
    }

    #[test]
    fn a_delete_may_carry_a_body() {
        let api = parse(quote! {
            pub api Api = "";
            pub group Notes { DELETE "/notes" => remove_many(ids: Vec<i64>); }
        })
        .unwrap();

        assert!(api.groups[0].routes[0].body().is_some());
    }

    #[test]
    fn one_body_only_and_never_on_a_get() {
        let two = messages(quote! {
            pub api Api = "/api";
            pub group G { POST "/x" => f(a: A, b: B); }
        });
        assert!(two[0].contains("one body"), "{two:?}");

        let get = messages(quote! {
            pub api Api = "/api";
            pub group G { GET "/x" => f(a: A); }
        });
        assert!(get[0].contains("GET takes no body"), "{get:?}");
    }

    #[test]
    fn mistakes_in_several_routes_are_all_reported() {
        let all = messages(quote! {
            pub api Api = "/api";
            pub group G {
                GET "/a/{x}" => a();
                GET "/b/{y}" => b();
            }
        });

        assert_eq!(all.len(), 2, "{all:?}");
    }

    #[test]
    fn the_same_method_and_path_twice_is_refused_across_groups() {
        let all = messages(quote! {
            pub api Api = "/api";
            pub group A { GET "/notes/{id}" => get(id: i64); }
            pub group B { GET "/notes/{note}" => other(note: i64); }
        });

        assert!(all[0].contains("already the route `A::get`"), "{all:?}");
    }

    #[test]
    fn a_function_name_is_taken_once_per_group() {
        let all = messages(quote! {
            pub api Api = "/api";
            pub group A { GET "/a" => f(); GET "/b" => f(); }
        });

        assert!(all[0].contains("`f` already"), "{all:?}");
    }

    #[test]
    fn a_contract_needs_exactly_one_api() {
        let none = messages(quote! { pub group G { GET "/a" => f(); } });
        assert!(none[0].contains("names its api"), "{none:?}");

        let two = messages(quote! { pub api A = "/a"; pub api B = "/b"; });
        assert!(two[0].contains("has one api"), "{two:?}");
    }

    #[test]
    fn what_is_left_for_later_says_what_is_supported() {
        let auth = messages(quote! {
            pub api Api = "/api";
            #[auth(Bearer)]
            pub group G { GET "/a" => f(); }
        });
        assert!(auth[0].contains("only `///` comments"), "{auth:?}");

        let events = messages(quote! {
            pub api Api = "/api";
            pub group G { GET "/a" => f() -> Events<Note>; }
        });
        assert!(
            events[0].contains("nothing (no `->`), `T` or `Created<T>`"),
            "{events:?}"
        );
    }
}
