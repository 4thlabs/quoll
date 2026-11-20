use std::collections::HashMap;

use futures_util::{Stream, stream};
use notes_api::{Note, NoteEvent};
use tokio::sync::broadcast;

use super::notes::find;
use super::{Store, StoreError};

const CHANGES_KEPT: usize = 16;

#[derive(Debug, Default)]
pub(super) struct Watchers {
    senders: HashMap<i64, broadcast::Sender<NoteEvent>>,
}

impl Watchers {
    fn subscribe(&mut self, id: i64) -> broadcast::Receiver<NoteEvent> {
        // The senders of notes no one watches any more go, so that they do not pile up.
        self.senders
            .retain(|_, watchers| watchers.receiver_count() > 0);

        self.senders
            .entry(id)
            .or_insert_with(|| broadcast::channel(CHANGES_KEPT).0)
            .subscribe()
    }

    pub(super) fn edited(&self, note: &Note) {
        if let Some(watchers) = self.senders.get(&note.id) {
            // No watcher left is no error: the next `watch` drops the sender.
            let _ = watchers.send(NoteEvent::Edited(note.clone()));
        }
    }

    pub(super) fn removed(&mut self, id: i64) {
        // The sender dropped here ends each watcher's stream, once it has read the removal.
        if let Some(watchers) = self.senders.remove(&id) {
            let _ = watchers.send(NoteEvent::Removed { id });
        }
    }
}

impl Store {
    pub async fn watch(
        &self,
        owner: i64,
        id: i64,
    ) -> Result<Option<impl Stream<Item = NoteEvent> + Send + 'static>, StoreError> {
        let Some(changes) = self
            .run(move |inner| {
                if find(&inner.connection, owner, id)?.is_none() {
                    return Ok(None);
                }

                Ok(Some(inner.watchers.subscribe(id)))
            })
            .await?
        else {
            return Ok(None);
        };

        Ok(Some(stream::unfold(changes, |mut changes| async move {
            loop {
                match changes.recv().await {
                    Ok(event) => return Some((event, changes)),
                    // The oldest changes were missed: the ones that follow say the note as it is.
                    Err(broadcast::error::RecvError::Lagged(_)) => {}
                    Err(broadcast::error::RecvError::Closed) => return None,
                }
            }
        })))
    }
}
