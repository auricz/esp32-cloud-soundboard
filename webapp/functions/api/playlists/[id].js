import { errorResponse, json, parsePlaylist, rowToPlaylist } from "../../../lib/playlists.js";

// GET /api/playlists/:id -> { id, name, urls, updatedAt }
export async function onRequestGet({ params, env }) {
  const row = await env.DB.prepare("SELECT * FROM playlists WHERE id = ?").bind(params.id).first();
  if (!row) return errorResponse("Playlist not found", 404);
  return json(rowToPlaylist(row));
}

// PUT /api/playlists/:id { name, urls } -> updated playlist
export async function onRequestPut({ request, params, env }) {
  const { value, error } = await parsePlaylist(request);
  if (error) return errorResponse(error, 400);

  const playlist = { id: params.id, ...value, updatedAt: new Date().toISOString() };
  const { meta } = await env.DB.prepare(
    "UPDATE playlists SET name = ?, urls = ?, updated_at = ? WHERE id = ?"
  )
    .bind(playlist.name, JSON.stringify(playlist.urls), playlist.updatedAt, playlist.id)
    .run();
  if (meta.changes === 0) return errorResponse("Playlist not found", 404);
  return json(playlist);
}

// DELETE /api/playlists/:id (buttons mapped to this playlist are left without one)
export async function onRequestDelete({ params, env }) {
  await env.DB.prepare("DELETE FROM playlists WHERE id = ?").bind(params.id).run();
  return new Response(null, { status: 204 });
}
