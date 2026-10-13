import { BUTTON_COLORS, errorResponse, json } from "../../lib/playlists.js";

// Every call should pick again, so nothing along the way may cache the answer.
const NO_STORE = { "Cache-Control": "no-store" };

// GET /api/random?color=<red|blu|yel|grn> -> a random URL from that button's playlist.
//   default        302 redirect to the WAV (follow redirects to get the audio bytes)
//   &format=text   the URL as text/plain
//   &format=json   { url, color, playlist: { id, name } }
export async function onRequestGet({ request, env }) {
  const params = new URL(request.url).searchParams;
  const color = params.get("color");
  if (!BUTTON_COLORS.includes(color)) {
    return errorResponse(`color must be one of: ${BUTTON_COLORS.join(", ")}`, 400, NO_STORE);
  }

  const row = await env.DB.prepare(
    `SELECT p.id, p.name, p.urls FROM button_playlists b JOIN playlists p ON p.id = b.playlist_id
     WHERE b.color = ?`
  )
    .bind(color)
    .first();
  if (!row) return errorResponse(`No playlist is mapped to the ${color} button`, 404, NO_STORE);

  const urls = JSON.parse(row.urls);
  if (urls.length === 0) {
    return errorResponse(`Playlist "${row.name}" has no URLs`, 404, NO_STORE);
  }
  const url = urls[Math.floor(Math.random() * urls.length)];

  const format = params.get("format");
  if (format === "json") {
    return json({ url, color, playlist: { id: row.id, name: row.name } }, 200, NO_STORE);
  }
  if (format === "text") {
    return new Response(url, {
      headers: { "Content-Type": "text/plain; charset=utf-8", ...NO_STORE },
    });
  }
  return new Response(null, { status: 302, headers: { Location: url, ...NO_STORE } });
}
