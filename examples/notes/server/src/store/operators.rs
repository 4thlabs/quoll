use rusqlite::{OptionalExtension, params};
use sha2::{Digest, Sha256};

use super::{Store, StoreError};
use crate::Operator;
use crate::utils::{hex, random_hex};

impl Store {
    pub async fn add_operator(&self, name: impl Into<String>) -> Result<String, StoreError> {
        let key = random_hex();
        let hash = key_hash(&key);
        let name = name.into();

        self.run(move |inner| {
            inner.connection.execute(
                "INSERT INTO operators (name, key_hash) VALUES (?1, ?2)",
                params![name, hash],
            )?;

            Ok(())
        })
        .await?;

        Ok(key)
    }

    pub async fn operator(&self, key: &str) -> Result<Option<Operator>, StoreError> {
        let hash = key_hash(key);

        self.run(move |inner| {
            let name = inner
                .connection
                .query_row(
                    "SELECT name FROM operators WHERE key_hash = ?1",
                    params![hash],
                    |row| row.get(0),
                )
                .optional()?;

            Ok(name.map(|name| Operator { name }))
        })
        .await
    }
}

/// A fast hash keeps 32 random bytes out of reach, and a lookup by hash compares no byte of the key.
fn key_hash(key: &str) -> String {
    hex(&Sha256::digest(key.as_bytes()))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_key_is_stored_as_its_sha_256() {
        assert_eq!(
            key_hash("abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        );
    }
}
