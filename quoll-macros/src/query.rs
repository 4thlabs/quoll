//! `#[rest::query]`: a query struct whose fields take `#[join(sep)]`, as the arguments of a route do. Each one is
//! rewritten into serde's `with`, naming the type of quoll's that writes and reads the list with the field's own `T`:
//! serde infers nothing more, and schemars takes the type as it is written, so that the schema is the list's.

use proc_macro2::TokenStream;
use quote::quote;
use syn::punctuated::Punctuated;
use syn::{Attribute, Data, DeriveInput, Error, Fields, Meta, Result, Token};

use crate::model::{JoinAttr, check_joined_type, vec_inner};
use crate::syntax::parse_join;

/// The attributes of a field that already say how serde or schemars writes it, which `#[join]` would replace.
const WITH: &[(&str, &[&str])] = &[
    ("serde", &["with", "serialize_with", "deserialize_with"]),
    ("schemars", &["with", "schema_with"]),
];

/// The struct, each `#[join(sep)]` of its fields rewritten, or the mistakes in them beside the struct without them, so
/// that the derives after it report nothing more.
pub(crate) fn query(attr: TokenStream, item: TokenStream) -> TokenStream {
    let mut errors: Option<Error> = None;
    let mut push = |error: Error| match &mut errors {
        Some(errors) => errors.combine(error),
        None => errors = Some(error),
    };

    if !attr.is_empty() {
        push(Error::new_spanned(
            &attr,
            "`#[rest::query]` takes no argument: write it alone, before the derives",
        ));
    }

    let expected = "`#[rest::query]` goes on a query struct, whose fields are named as its keys: write `struct Filter { labels: Vec<String> }`";
    let mut item: DeriveInput = match syn::parse2(item.clone()) {
        Ok(item) => item,
        Err(error) => {
            let error = Error::new(error.span(), expected).into_compile_error();

            return quote! { #error #item };
        }
    };
    let Data::Struct(syn::DataStruct {
        fields: Fields::Named(fields),
        ..
    }) = &mut item.data
    else {
        push(Error::new_spanned(&item.ident, expected));
        strip_joins(&mut item.data);
        let errors = errors.map(|error| error.into_compile_error());

        return quote! { #errors #item };
    };

    for field in &mut fields.named {
        let (joins, others): (Vec<Attribute>, Vec<Attribute>) = field
            .attrs
            .drain(..)
            .partition(|attr| attr.path().is_ident("join"));
        field.attrs = others;

        let mut joins = joins.into_iter();
        let Some(first) = joins.next() else {
            continue;
        };
        for twice in joins {
            push(Error::new_spanned(
                twice,
                "a field takes one `#[join(...)]`: remove this one",
            ));
        }

        let name = field
            .ident
            .as_ref()
            .map(ToString::to_string)
            .unwrap_or_default();
        match rewrite(first, &name, field) {
            Ok(with) => field.attrs.push(with),
            Err(error) => push(error),
        }
    }

    let errors = errors.map(|error| error.into_compile_error());

    quote! { #errors #item }
}

/// Removes every `#[join]` of an item `#[rest::query]` does not read, which rustc would report again as an unknown
/// attribute.
fn strip_joins(data: &mut Data) {
    let fields: Vec<&mut syn::Field> = match data {
        Data::Struct(data) => data.fields.iter_mut().collect(),
        Data::Enum(data) => data
            .variants
            .iter_mut()
            .flat_map(|variant| variant.fields.iter_mut())
            .collect(),
        Data::Union(data) => data.fields.named.iter_mut().collect(),
    };

    for field in fields {
        field.attrs.retain(|attr| !attr.path().is_ident("join"));
    }
}

/// The `#[serde(with = "...")]` that writes the field as its `#[join(sep)]` says.
fn rewrite(attr: Attribute, name: &str, field: &syn::Field) -> Result<Attribute> {
    let join = parse_join(attr)?;

    check_joined_type(&join, name, &field.ty)?;
    check_no_with(&join, &field.attrs)?;

    let element = vec_inner(&field.ty).expect("checked to be a `Vec`");
    let with = join.join.with(element);

    Ok(syn::parse_quote! { #[serde(with = #with)] })
}

/// A field that `#[join]` writes says nowhere else how serde or schemars writes it.
fn check_no_with(join: &JoinAttr, attrs: &[Attribute]) -> Result<()> {
    for attr in attrs {
        let Some((owner, keys)) = WITH.iter().find(|(owner, _)| attr.path().is_ident(owner)) else {
            continue;
        };
        let Ok(metas) = attr.parse_args_with(Punctuated::<Meta, Token![,]>::parse_terminated)
        else {
            continue;
        };
        let with = metas
            .iter()
            .find_map(|meta| keys.iter().find(|key| meta.path().is_ident(key)));

        if let Some(with) = with {
            return Err(Error::new_spanned(
                &join.attr,
                format!(
                    "`#[join]` writes the field itself, and its `#[{owner}({with} = ...)]` too: remove one of them"
                ),
            ));
        }
    }

    Ok(())
}

#[cfg(test)]
mod tests {
    use quote::quote;

    use super::*;

    fn rewritten(item: TokenStream) -> String {
        query(TokenStream::new(), item).to_string()
    }

    #[test]
    fn a_join_becomes_serde_with_of_the_fields_type() {
        let tokens = rewritten(quote! {
            struct Filter {
                #[join(",")]
                #[serde(default, rename = "label")]
                labels: Vec<String>,
                #[join(" ")]
                ids: Vec<u32>,
                state: Option<State>,
            }
        });

        assert!(
            tokens.contains(r#"# [serde (default , rename = "label")] # [serde (with = "::quoll::__private::join::Comma::<String>")] labels"#),
            "{tokens}"
        );
        assert!(
            tokens.contains(r#"# [serde (with = "::quoll::__private::join::Space::<u32>")] ids"#),
            "{tokens}"
        );
        assert!(!tokens.contains("join ("), "{tokens}");
        assert!(tokens.contains("state : Option < State >"), "{tokens}");
    }

    #[test]
    fn a_struct_without_join_is_left_as_it_is() {
        let item = quote! { struct Filter { #[serde(default)] page: Option<u32> } };

        assert_eq!(rewritten(item.clone()), item.to_string());
    }
}
