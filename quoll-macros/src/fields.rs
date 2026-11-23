//! What the fixed fields of a body become: the fields of each route, as the client, the server, the descriptor and
//! the document name them, whatever the features.

use proc_macro2::{Literal, TokenStream};
use quote::quote;

use crate::model::{BodyField, FieldValue, Route};

/// A fixed field, `::quoll::__private::fields::Field`.
fn field(field: &BodyField) -> TokenStream {
    let name = field.name.value();
    let value = match &field.value {
        FieldValue::Bool(value) => quote! { Bool(#value) },
        FieldValue::Int(value) => {
            let value = Literal::i64_suffixed(*value);

            quote! { Int(#value) }
        }
        FieldValue::Uint(value) => {
            let value = Literal::u64_suffixed(*value);

            quote! { Uint(#value) }
        }
        FieldValue::Float(value) => {
            let value = Literal::f64_suffixed(*value);

            quote! { Float(#value) }
        }
        FieldValue::Str(value) => quote! { Str(#value) },
    };

    quote! {
        ::quoll::__private::fields::Field {
            name: #name,
            value: ::quoll::__private::fields::Value::#value,
        }
    }
}

/// The fixed fields of `route`'s body, a `&'static [Field]`.
pub(crate) fn route_fields(route: &Route) -> TokenStream {
    let fields = route.fields.iter().map(field);

    quote! { const { &[#(#fields),*] } }
}
