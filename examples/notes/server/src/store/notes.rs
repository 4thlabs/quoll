use std::collections::BTreeSet;

use notes_api::{NewNote, Note, NoteFilter};
use rusqlite::{Connection, OptionalExtension, params};

use super::{Store, StoreError};

pub const PAGE_SIZE: u32 = 20;

impl Store {
    pub async fn notes(&self, owner: i64, filter: &NoteFilter) -> Result<Vec<Note>, StoreError> {
        // `LIKE` compares ASCII letters whatever their case, and reads `%` and `_` as wildcards, escaped here.
        let contains = filter
            .contains
            .as_deref()
            .filter(|text| !text.is_empty())
            .map(|text| format!("%{}%", escape_like(text)));
        let tag = filter
            .tag
            .as_deref()
            .and_then(|tag| tag_of(tag.strip_prefix('#').unwrap_or(tag)));
        let offset = i64::from(filter.page.unwrap_or(1).max(1) - 1) * i64::from(PAGE_SIZE);

        self.run(move |inner| {
            let mut statement = inner.connection.prepare_cached(
                "SELECT id, title, body FROM notes
                 WHERE owner = ?1
                   AND (?2 IS NULL OR title LIKE ?2 ESCAPE '\\' OR body LIKE ?2 ESCAPE '\\')
                   AND (?3 IS NULL OR id IN (SELECT note FROM tags WHERE tag = ?3))
                 ORDER BY id
                 LIMIT ?4 OFFSET ?5",
            )?;
            let notes = statement
                .query_map(params![owner, contains, tag, PAGE_SIZE, offset], read_note)?
                .collect::<Result<_, _>>()?;

            Ok(notes)
        })
        .await
    }

    pub async fn note(&self, owner: i64, id: i64) -> Result<Option<Note>, StoreError> {
        self.run(move |inner| Ok(find(&inner.connection, owner, id)?))
            .await
    }

    pub async fn add(&self, owner: i64, note: NewNote) -> Result<Note, StoreError> {
        self.run(move |inner| {
            let transaction = inner.connection.transaction()?;

            transaction.execute(
                "INSERT INTO notes (owner, title, body) VALUES (?1, ?2, ?3)",
                params![owner, note.title, note.body],
            )?;

            let id = transaction.last_insert_rowid();

            tag(&transaction, id, &note)?;
            transaction.commit()?;

            Ok(Note {
                id,
                title: note.title,
                body: note.body,
            })
        })
        .await
    }

    pub async fn replace(
        &self,
        owner: i64,
        id: i64,
        note: NewNote,
    ) -> Result<Option<Note>, StoreError> {
        self.run(move |inner| {
            let transaction = inner.connection.transaction()?;
            let changed = transaction.execute(
                "UPDATE notes SET title = ?3, body = ?4 WHERE owner = ?1 AND id = ?2",
                params![owner, id, note.title, note.body],
            )?;

            if changed == 0 {
                return Ok(None);
            }

            transaction.execute("DELETE FROM tags WHERE note = ?1", params![id])?;
            tag(&transaction, id, &note)?;
            transaction.commit()?;

            let note = Note {
                id,
                title: note.title,
                body: note.body,
            };

            inner.watchers.edited(&note);
            Ok(Some(note))
        })
        .await
    }

    pub async fn remove(&self, owner: i64, id: i64) -> Result<(), StoreError> {
        self.run(move |inner| {
            let transaction = inner.connection.transaction()?;
            let removed = transaction.execute(
                "DELETE FROM notes WHERE owner = ?1 AND id = ?2",
                params![owner, id],
            )?;

            if removed == 0 {
                return Ok(());
            }

            transaction.execute("DELETE FROM tags WHERE note = ?1", params![id])?;
            transaction.commit()?;
            inner.watchers.removed(id);

            Ok(())
        })
        .await
    }
}

pub(super) fn find(connection: &Connection, owner: i64, id: i64) -> rusqlite::Result<Option<Note>> {
    connection
        .query_row(
            "SELECT id, title, body FROM notes WHERE owner = ?1 AND id = ?2",
            params![owner, id],
            read_note,
        )
        .optional()
}

fn read_note(row: &rusqlite::Row<'_>) -> rusqlite::Result<Note> {
    Ok(Note {
        id: row.get(0)?,
        title: row.get(1)?,
        body: row.get(2)?,
    })
}

fn tag(connection: &Connection, id: i64, note: &NewNote) -> rusqlite::Result<()> {
    let mut statement =
        connection.prepare_cached("INSERT INTO tags (tag, note) VALUES (?1, ?2)")?;

    for tag in tags(note) {
        statement.execute(params![tag, id])?;
    }

    Ok(())
}

fn tags(note: &NewNote) -> BTreeSet<String> {
    [&note.title, &note.body]
        .into_iter()
        .flat_map(|text| text.split_whitespace())
        .filter_map(|word| word.strip_prefix('#').and_then(tag_of))
        .collect()
}

fn tag_of(word: &str) -> Option<String> {
    let end = word
        .find(|c: char| !(c.is_alphanumeric() || c == '-' || c == '_'))
        .unwrap_or(word.len());

    (end > 0).then(|| word[..end].to_lowercase())
}

fn escape_like(text: &str) -> String {
    let mut escaped = String::with_capacity(text.len());

    for c in text.chars() {
        if matches!(c, '\\' | '%' | '_') {
            escaped.push('\\');
        }

        escaped.push(c);
    }

    escaped
}

#[cfg(test)]
mod tests {
    use super::*;

    fn note(title: &str, body: &str) -> NewNote {
        NewNote {
            title: title.into(),
            body: body.into(),
        }
    }

    #[test]
    fn the_tags_of_a_note_are_its_hash_words() {
        let tags = tags(&note(
            "#Shopping list",
            "milk, #dairy, #2026-11 a#b # #fresh!",
        ));

        assert_eq!(
            tags.into_iter().collect::<Vec<_>>(),
            ["2026-11", "dairy", "fresh", "shopping"]
        );
    }

    #[test]
    fn a_tag_is_cut_at_its_first_other_character() {
        assert_eq!(tag_of("Milk,").as_deref(), Some("milk"));
        assert_eq!(tag_of("oat-milk_2!").as_deref(), Some("oat-milk_2"));
        assert_eq!(tag_of(""), None);
        assert_eq!(tag_of(",milk"), None);
    }

    #[test]
    fn the_wildcards_of_like_are_escaped() {
        assert_eq!(escape_like(r"50%_off\"), r"50\%\_off\\");
    }
}
