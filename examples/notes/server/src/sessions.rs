use std::collections::HashMap;
use std::fmt;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use crate::User;
use crate::utils::{lock, random_hex};

pub const SESSION_LIFETIME: Duration = Duration::from_secs(24 * 60 * 60);

/// An expired session goes when its token is met, and all of them when a session opens, so that they do not pile up.
#[derive(Clone)]
pub struct Sessions {
    lifetime: Duration,
    open: Arc<Mutex<HashMap<String, Session>>>,
}

struct Session {
    user: User,
    opened: Instant,
}

impl Session {
    fn is_past(&self, lifetime: Duration) -> bool {
        self.opened.elapsed() >= lifetime
    }
}

impl Default for Sessions {
    fn default() -> Self {
        Self::with_lifetime(SESSION_LIFETIME)
    }
}

impl Sessions {
    pub fn with_lifetime(lifetime: Duration) -> Self {
        Self {
            lifetime,
            open: Arc::default(),
        }
    }

    pub fn open(&self, user: User) -> String {
        let token = random_hex();
        let mut open = lock(&self.open);

        open.retain(|_, session| !session.is_past(self.lifetime));
        open.insert(
            token.clone(),
            Session {
                user,
                opened: Instant::now(),
            },
        );
        token
    }

    pub fn user(&self, token: &str) -> Option<User> {
        let mut open = lock(&self.open);
        let session = open.get(token)?;

        if session.is_past(self.lifetime) {
            open.remove(token);
            return None;
        }

        Some(session.user.clone())
    }
}

/// Never shows the tokens.
impl fmt::Debug for Sessions {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Sessions")
            .field("lifetime", &self.lifetime)
            .field("open", &lock(&self.open).len())
            .finish()
    }
}
