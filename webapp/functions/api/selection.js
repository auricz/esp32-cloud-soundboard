import { errorResponse, json } from "../../lib/playlists.js";

// GET /api/selection -> { playlistId, name } (both null when nothing is selected)
export async function onRequestGet({ env }) {
  const row = await env.DB.prepare(
    `SELECT p.id, p.name FROM selection s JOIN playlists p ON p.id = s.playlist_id
     WHERE s.id = 1`
  ).first();
  return json({ playlistId: row?.id ?? null, name: row?.name ?? null });
}

// PUT /api/selection { playlistId: string | null } -> { playlistId, name }
export async function onRequestPut({ request, env }) {
  let body;
  try {
    body = await request.json();
  } catch {
    return errorResponse("Request body must be JSON", 400);
  }
  const playlistId = body?.playlistId ?? null;
  if (playlistId !== null && typeof playlistId !== "string") {
    return errorResponse("playlistId must be a string or null", 400);
  }

  let name = null;
  if (playlistId !== null) {
    const row = await env.DB.prepare("SELECT name FROM playlists WHERE id = ?")
      .bind(playlistId)
      .first();
    if (!row) return errorResponse("Playlist not found", 404);
    name = row.name;
  }

  await env.DB.prepare(
    `INSERT INTO selection (id, playlist_id) VALUES (1, ?)
     ON CONFLICT (id) DO UPDATE SET playlist_id = excluded.playlist_id`
  )
    .bind(playlistId)
    .run();
  return json({ playlistId, name });
}
