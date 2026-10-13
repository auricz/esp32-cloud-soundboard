import { errorResponse, json } from "../../lib/playlists.js";

// Every call should pick again, so nothing along the way may cache the answer.
const NO_STORE = { "Cache-Control": "no-store" };

// GET /api/random -> a random URL from the selected playlist.
//   default        302 redirect to the WAV (follow redirects to get the audio bytes)
//   ?format=text   the URL as text/plain
//   ?format=json   { url, playlist: { id, name } }
export async function onRequestGet({ request, env }) {
  const row = await env.DB.prepare(
    `SELECT p.id, p.name, p.urls FROM selection s JOIN playlists p ON p.id = s.playlist_id
     WHERE s.id = 1`
  ).first();
  if (!row) return errorResponse("No playlist selected", 404, NO_STORE);

  const urls = JSON.parse(row.urls);
  if (urls.length === 0) {
    return errorResponse(`Playlist "${row.name}" has no URLs`, 404, NO_STORE);
  }
  const url = urls[Math.floor(Math.random() * urls.length)];

  const format = new URL(request.url).searchParams.get("format");
  if (format === "json") {
    return json({ url, playlist: { id: row.id, name: row.name } }, 200, NO_STORE);
  }
  if (format === "text") {
    return new Response(url, {
      headers: { "Content-Type": "text/plain; charset=utf-8", ...NO_STORE },
    });
  }
  return new Response(null, { status: 302, headers: { Location: url, ...NO_STORE } });
}
