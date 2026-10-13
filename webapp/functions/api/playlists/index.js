import { errorResponse, json, parsePlaylist } from "../../../lib/playlists.js";

// GET /api/playlists -> [{ id, name, count, updatedAt }]
export async function onRequestGet({ env }) {
  const { results } = await env.DB.prepare(
    `SELECT id, name, json_array_length(urls) AS count, updated_at AS updatedAt
     FROM playlists ORDER BY name COLLATE NOCASE`
  ).all();
  return json(results);
}

// POST /api/playlists { name, urls } -> created playlist
export async function onRequestPost({ request, env }) {
  const { value, error } = await parsePlaylist(request);
  if (error) return errorResponse(error, 400);

  const playlist = { id: crypto.randomUUID(), ...value, updatedAt: new Date().toISOString() };
  await env.DB.prepare("INSERT INTO playlists (id, name, urls, updated_at) VALUES (?, ?, ?, ?)")
    .bind(playlist.id, playlist.name, JSON.stringify(playlist.urls), playlist.updatedAt)
    .run();
  return json(playlist, 201);
}
