import { BUTTON_COLORS, json } from "../../../lib/playlists.js";

// GET /api/buttons -> [{ color, playlistId, name }] in BUTTON_COLORS order
// (playlistId and name are null for a button with no playlist)
export async function onRequestGet({ env }) {
  const { results } = await env.DB.prepare(
    `SELECT b.color, p.id AS playlistId, p.name FROM button_playlists b
     LEFT JOIN playlists p ON p.id = b.playlist_id`
  ).all();
  return json(
    BUTTON_COLORS.map((color) => {
      const row = results.find((r) => r.color === color);
      return { color, playlistId: row?.playlistId ?? null, name: row?.name ?? null };
    })
  );
}
