import { BUTTON_COLORS, errorResponse, json } from "../../../lib/playlists.js";

// PUT /api/buttons/:color { playlistId: string | null } -> { color, playlistId, name }
export async function onRequestPut({ request, params, env }) {
  const { color } = params;
  if (!BUTTON_COLORS.includes(color)) return errorResponse("Button not found", 404);

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
    `INSERT INTO button_playlists (color, playlist_id) VALUES (?, ?)
     ON CONFLICT (color) DO UPDATE SET playlist_id = excluded.playlist_id`
  )
    .bind(color, playlistId)
    .run();
  return json({ color, playlistId, name });
}
