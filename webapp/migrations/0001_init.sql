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

-- One row per button on the device, holding the playlist /api/random?color=<color> draws from.
CREATE TABLE button_playlists (
  color TEXT PRIMARY KEY CHECK (color IN ('red', 'blu', 'yel', 'grn')),
  playlist_id TEXT REFERENCES playlists (id) ON DELETE SET NULL
);

-- Every button starts out on the playlist the single button used to play from.
INSERT INTO button_playlists (color, playlist_id)
SELECT color, (SELECT playlist_id FROM selection WHERE id = 1)
FROM (SELECT 'red' AS color UNION ALL SELECT 'blu' UNION ALL SELECT 'yel' UNION ALL SELECT 'grn');

DROP TABLE selection;
