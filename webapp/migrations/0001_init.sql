CREATE TABLE playlists (
  id TEXT PRIMARY KEY,
  name TEXT NOT NULL,
  urls TEXT NOT NULL, -- JSON array of URL strings
  updated_at TEXT NOT NULL
);

-- Single row holding the playlist that /api/random draws from.
CREATE TABLE selection (
  id INTEGER PRIMARY KEY CHECK (id = 1),
  playlist_id TEXT REFERENCES playlists (id) ON DELETE SET NULL
);
