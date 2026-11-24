//! The arguments a contract writes `&X`: the client borrows them, and the server and the document receive the owned
//! type, `String` for a `&str`, `Vec<T>` for a `&[T]` and `X` for any other `&X`, nested ones included. The macro
//! reads the `&` alone, and no type's name but `str`: an `X` that has no owned form, `&Path`, is found by rustc, on a
//! check that [`sized_checks`] writes.
//!
//! The client takes the type as it is written, as it did before the server received an owned one: what the server
//! cannot receive, a `&mut X` or a `&dyn Trait`, is refused for the server alone, so that a contract with a client
//! alone compiles as before.

use proc_macro2::{Span, TokenStream};
use quote::{ToTokens, quote_spanned};
use syn::{
    Error, GenericArgument, Lifetime, PathArguments, Result, Type, TypeReference, spanned::Spanned,
};

use crate::model::peel;

/// What the server receives for an argument: its type, and why it cannot receive it, when it cannot.
#[derive(Debug)]
pub(crate) struct Owned {
    /// The type written with each `&X` replaced by the owned type of `X`, a refused one too: `X` for `&mut X`.
    pub(crate) ty: Type,
    /// The first `&` the server cannot receive owned, which its code refuses: a `&mut X`, a `&&X`, a `&dyn Trait` or
    /// a `&impl Trait`.
    pub(crate) refused: Option<Error>,
}

/// The type the server receives for an argument written `ty`: each `&X` in it replaced by the owned type of `X`.
///
/// # Errors
///
/// A `&'a X` with a lifetime other than `'static` or `'_`, which never compiled on either side, spanned on it.
pub(crate) fn owned(ty: &Type) -> Result<Owned> {
    let mut owned = Owned {
        ty: ty.clone(),
        refused: None,
    };

    rewrite(&mut owned.ty, &mut owned.refused)?;

    Ok(owned)
}

/// Replaces each `&X` of `ty` by its owned type, keeping in `refused` the first one the server cannot receive. A
/// qualified path, `<&str as Trait>::Assoc`, is left as it is written: its `&` names the trait's impl, not a value.
fn rewrite(ty: &mut Type, refused: &mut Option<Error>) -> Result<()> {
    match ty {
        Type::Reference(reference) => {
            check_lifetime(reference)?;

            if refused.is_none() {
                *refused = check(reference).err();
            }

            *ty = owned_of(reference, refused)?;
        }
        Type::Path(path) => {
            if path.qself.is_some() {
                return Ok(());
            }

            for segment in &mut path.path.segments {
                let PathArguments::AngleBracketed(args) = &mut segment.arguments else {
                    continue;
                };
                for arg in &mut args.args {
                    match arg {
                        GenericArgument::Type(inner) => rewrite(inner, refused)?,
                        GenericArgument::AssocType(assoc) => rewrite(&mut assoc.ty, refused)?,
                        _ => {}
                    }
                }
            }
        }
        Type::Tuple(tuple) => {
            for elem in &mut tuple.elems {
                rewrite(elem, refused)?;
            }
        }
        Type::Array(array) => rewrite(&mut array.elem, refused)?,
        Type::Slice(slice) => rewrite(&mut slice.elem, refused)?,
        Type::Paren(paren) => rewrite(&mut paren.elem, refused)?,
        Type::Group(group) => rewrite(&mut group.elem, refused)?,
        _ => {}
    }

    Ok(())
}

/// A reference's lifetime is elided, `'_`, or `'static`, which compiled before in a contract that has only a client.
fn check_lifetime(reference: &TypeReference) -> Result<()> {
    let Some(lifetime) = &reference.lifetime else {
        return Ok(());
    };
    if lifetime.ident == "static" || lifetime.ident == "_" {
        return Ok(());
    }

    Err(Error::new_spanned(
        lifetime,
        "the client borrows an argument for the call alone, which needs no lifetime: write `&X`, without it",
    ))
}

/// A reference the server receives owned: shared, to a type, not a trait.
fn check(reference: &TypeReference) -> Result<()> {
    if let Some(mutability) = &reference.mutability {
        return Err(Error::new_spanned(
            quote_spanned! {reference.and_token.span=> & #mutability },
            "the client reads an argument and never writes it back, and the server receives it owned: write `&X` to borrow it, or `X` to take it",
        ));
    }

    match peel(&reference.elem) {
        Type::Reference(inner) => Err(Error::new_spanned(
            quote_spanned! {inner.and_token.span=> & },
            "the client borrows an argument once, and the server receives it owned: write `&X`",
        )),
        Type::TraitObject(_) | Type::ImplTrait(_) => Err(Error::new_spanned(
            &reference.elem,
            "the server receives each argument as a value of its own type, which a trait is not: write the type, as `&NewNote`",
        )),
        _ => Ok(()),
    }
}

/// The owned type of `&X`: `String` for a `&str`, `Vec<T>` for a `&[T]`, and `X` itself otherwise, each `&` in it
/// replaced too. Spanned on what the contract writes, so that an error on the server's type points at it.
fn owned_of(reference: &TypeReference, refused: &mut Option<Error>) -> Result<Type> {
    let start = reference.and_token.span;
    let end = last_span(&reference.elem).unwrap_or(start);

    match peel(&reference.elem) {
        Type::Slice(slice) => {
            let mut elem = (*slice.elem).clone();

            rewrite(&mut elem, refused)?;

            let vec = quote_spanned! {start=> ::std::vec::Vec< };
            let close = quote_spanned! {end=> > };

            syn::parse2(quote_spanned! {start=> #vec #elem #close })
        }
        elem if is_str(elem) => syn::parse2(quote_spanned! {end=> ::std::string::String }),
        elem => {
            let mut elem = elem.clone();

            rewrite(&mut elem, refused)?;

            Ok(elem)
        }
    }
}

/// Whether `ty` is `str`, as `str` or by its path in `std` or `core`, `std::primitive::str`.
fn is_str(ty: &Type) -> bool {
    let Type::Path(path) = ty else {
        return false;
    };
    if path.qself.is_some() {
        return false;
    }

    let segments: Vec<String> = path
        .path
        .segments
        .iter()
        .map(|segment| match segment.arguments {
            PathArguments::None => segment.ident.to_string(),
            _ => String::new(),
        })
        .collect();

    match segments.as_slice() {
        [str] => path.path.leading_colon.is_none() && str == "str",
        [krate, primitive, str] => {
            (krate == "std" || krate == "core") && primitive == "primitive" && str == "str"
        }
        _ => false,
    }
}

/// The checks that each `&X` of an argument written `ty`, which the server receives, borrows a type the server can
/// receive owned, `X` itself, but `&str` and `&[T]`, whose owned forms the rewrite knows: one call per `X`, spanned on
/// the contract's tokens. For an `X` of no size, as `Path` or `OsStr`, rustc reports there that the size of `X`
/// cannot be known at compile time. The server's code names `X` too, and rustc reports its errors beside this one:
/// they are not one error, but this one names the contract's type.
pub(crate) fn sized_checks(ty: &Type) -> Vec<TokenStream> {
    let mut checks = Vec::new();
    let mut ty = ty.clone();

    each_reference(&mut ty, &mut |reference| {
        let elem = peel(&reference.elem);
        if is_str(elem) || matches!(elem, Type::Slice(_)) {
            return;
        }

        let owned = owned(elem)
            .expect("a part of an argument that was read has no lifetime the whole would refuse")
            .ty;

        checks.push(quote_spanned! {elem.span()=> ::quoll::__private::known::sized::<#owned>(); });
    });

    checks
}

fn last_span(ty: &Type) -> Option<Span> {
    ty.to_token_stream()
        .into_iter()
        .last()
        .map(|token| token.span())
}

/// Whether `ty` holds a `&`, which the client borrows.
pub(crate) fn borrows(ty: &Type) -> bool {
    let mut any = false;
    let mut written = ty.clone();

    each_reference(&mut written, &mut |_| any = true);

    any
}

/// `ty` with the lifetime of each of its `&` replaced by `lifetime`, for a struct that holds it: `'static` too, which
/// coerces to it, so that `&'static [&str]` does not ask the struct's lifetime to outlive `'static`.
pub(crate) fn with_lifetime(ty: &Type, lifetime: &Lifetime) -> Type {
    let mut ty = ty.clone();

    each_reference(&mut ty, &mut |reference| {
        reference.lifetime = Some(lifetime.clone());
    });

    ty
}

/// Calls `f` on each `&` of `ty`, the outer one first, at each place [`rewrite`] replaces one: not in a qualified
/// path, which is left as it is written.
fn each_reference(ty: &mut Type, f: &mut impl FnMut(&mut TypeReference)) {
    match ty {
        Type::Reference(reference) => {
            f(reference);
            each_reference(&mut reference.elem, f);
        }
        Type::Path(path) => {
            if path.qself.is_some() {
                return;
            }

            for segment in &mut path.path.segments {
                let PathArguments::AngleBracketed(args) = &mut segment.arguments else {
                    continue;
                };
                for arg in &mut args.args {
                    match arg {
                        GenericArgument::Type(inner) => each_reference(inner, f),
                        GenericArgument::AssocType(assoc) => each_reference(&mut assoc.ty, f),
                        _ => {}
                    }
                }
            }
        }
        Type::Tuple(tuple) => {
            for elem in &mut tuple.elems {
                each_reference(elem, f);
            }
        }
        Type::Array(array) => each_reference(&mut array.elem, f),
        Type::Slice(slice) => each_reference(&mut slice.elem, f),
        Type::Paren(paren) => each_reference(&mut paren.elem, f),
        Type::Group(group) => each_reference(&mut group.elem, f),
        _ => {}
    }
}

#[cfg(test)]
mod tests {
    use quote::{ToTokens, quote};
    use syn::parse_quote;

    use super::*;

    /// The type the server receives, or the message of what refuses it.
    fn shown(ty: Type) -> String {
        match owned(&ty) {
            Ok(Owned {
                refused: Some(refused),
                ..
            }) => refused.to_string(),
            Ok(owned) => owned.ty.to_token_stream().to_string(),
            Err(error) => error.to_string(),
        }
    }

    #[test]
    fn each_borrow_is_replaced_by_its_owned_type() {
        for (written, expected) in [
            (quote! { &str }, quote! { ::std::string::String }),
            (quote! { &'static str }, quote! { ::std::string::String }),
            (quote! { &'_ str }, quote! { ::std::string::String }),
            (
                quote! { Option<&str> },
                quote! { Option<::std::string::String> },
            ),
            (
                quote! { &[&str] },
                quote! { ::std::vec::Vec<::std::string::String> },
            ),
            (quote! { &[String] }, quote! { ::std::vec::Vec<String> }),
            (quote! { Vec<&str> }, quote! { Vec<::std::string::String> }),
            (
                quote! { Option<&[&str]> },
                quote! { Option<::std::vec::Vec<::std::string::String> > },
            ),
            (quote! { &NewNote }, quote! { NewNote }),
            (quote! { &Form<Entry> }, quote! { Form<Entry> }),
            (quote! { Form<&Entry> }, quote! { Form<Entry> }),
            (quote! { String }, quote! { String }),
            (quote! { i64 }, quote! { i64 }),
            (
                quote! { <&str as Trait>::Assoc },
                quote! { <&str as Trait>::Assoc },
            ),
        ] {
            let written: Type = syn::parse2(written).expect("a type");
            let expected: Type = syn::parse2(expected).expect("a type");

            assert_eq!(
                shown(written.clone()),
                expected.to_token_stream().to_string(),
                "{}",
                written.to_token_stream()
            );
        }
    }

    #[test]
    fn what_the_server_cannot_receive_owned_is_refused_for_it_alone() {
        for (written, refused, server) in [
            (
                parse_quote!(&mut NewNote),
                "never writes it back",
                "NewNote",
            ),
            (
                parse_quote!(&&str),
                "borrows an argument once",
                ":: std :: string :: String",
            ),
            (
                parse_quote!(&dyn Debug),
                "which a trait is not",
                "dyn Debug",
            ),
            (
                parse_quote!(Option<&impl Debug>),
                "which a trait is not",
                "Option < impl Debug >",
            ),
        ] {
            let written: Type = written;
            let owned = owned(&written).expect("no lifetime to refuse");
            let message = owned.refused.map(|error| error.to_string());

            assert!(
                message
                    .as_deref()
                    .is_some_and(|message| message.contains(refused)),
                "{}: {message:?}",
                written.to_token_stream()
            );
            assert_eq!(owned.ty.to_token_stream().to_string(), server);
        }
    }

    #[test]
    fn a_named_lifetime_is_refused_on_both_sides() {
        for written in [parse_quote!(&'a str), parse_quote!(Option<&'a str>)] {
            let written: Type = written;

            assert!(
                shown(written.clone()).contains("needs no lifetime"),
                "{}",
                written.to_token_stream()
            );
        }
    }

    #[test]
    fn each_lifetime_is_the_structs() {
        let lifetime = Lifetime::new("'q", proc_macro2::Span::call_site());
        let ty: Type = parse_quote!(Option<&'static [&'_ str]>);
        let assoc: Type = parse_quote!(Box<dyn Iterator<Item = &str>>);
        let qself: Type = parse_quote!(<&str as Trait>::Assoc);

        assert!(borrows(&ty));
        assert!(borrows(&parse_quote!(&'static str)));
        assert!(!borrows(&parse_quote!(Option<String>)));
        assert!(!borrows(&qself));
        assert_eq!(
            with_lifetime(&ty, &lifetime).to_token_stream().to_string(),
            "Option < & 'q [& 'q str] >"
        );
        assert_eq!(
            with_lifetime(&parse_quote!(Option<Vec<(&str,)>>), &lifetime)
                .to_token_stream()
                .to_string(),
            "Option < Vec < (& 'q str ,) > >"
        );
        assert_eq!(with_lifetime(&assoc, &lifetime), assoc);
        assert_eq!(with_lifetime(&qself, &lifetime), qself);
    }

    #[test]
    fn str_is_read_through_parentheses_and_its_path() {
        for written in [
            quote! { &(str) },
            quote! { &std::primitive::str },
            quote! { &::core::primitive::str },
        ] {
            let written: Type = syn::parse2(written).expect("a type");

            assert_eq!(shown(written), ":: std :: string :: String");
        }

        // A type of the user's named `str` in a module of its own is another type.
        assert_eq!(shown(parse_quote!(&my::str)), "my :: str");
    }

    #[test]
    fn each_borrowed_type_but_str_and_a_slice_is_checked_to_have_a_size() {
        let checks = |ty: Type| -> Vec<String> {
            sized_checks(&ty).iter().map(ToString::to_string).collect()
        };

        assert!(checks(parse_quote!(&str)).is_empty());
        assert!(checks(parse_quote!(&[&str])).is_empty());
        assert!(checks(parse_quote!(String)).is_empty());
        assert_eq!(
            checks(parse_quote!(&Path)),
            [":: quoll :: __private :: known :: sized :: < Path > () ;"]
        );
        assert_eq!(
            checks(parse_quote!(&Option<&Note>)),
            [
                ":: quoll :: __private :: known :: sized :: < Option < Note > > () ;",
                ":: quoll :: __private :: known :: sized :: < Note > () ;",
            ]
        );
    }
}
