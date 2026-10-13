// Shared helpers for the /api Pages Functions. Playlists live in the D1
// `playlists` table (schema in migrations/), with urls stored as a JSON array.

// The buttons on the device, as passed in /api/random?color=
export const BUTTON_COLORS = ["red", "blu", "yel", "grn"];

const MAX_NAME_LENGTH = 100;
const MAX_URLS = 500;
const MAX_URL_LENGTH = 2048;

export function json(data, status = 200, headers = {}) {
  return Response.json(data, { status, headers });
}

export function errorResponse(message, status, headers = {}) {
  return json({ error: message }, status, headers);
}

export function rowToPlaylist(row) {
  return { id: row.id, name: row.name, urls: JSON.parse(row.urls), updatedAt: row.updated_at };
}

function isHttpUrl(value) {
  try {
    const { protocol } = new URL(value);
    return protocol === "http:" || protocol === "https:";
  } catch {
    return false;
  }
}

// Validates a request body of the form { name: string, urls: string[] }.
// Returns { value } on success or { error } with a user-facing message.
export async function parsePlaylist(request) {
  let body;
  try {
    body = await request.json();
  } catch {
    return { error: "Request body must be JSON" };
  }

  const name = typeof body?.name === "string" ? body.name.trim() : "";
  if (!name) return { error: "Playlist label is required" };
  if (name.length > MAX_NAME_LENGTH) {
    return { error: `Playlist label must be at most ${MAX_NAME_LENGTH} characters` };
  }

  if (!Array.isArray(body.urls)) return { error: "urls must be an array" };
  const urls = body.urls
    .map((url) => (typeof url === "string" ? url.trim() : ""))
    .filter(Boolean);
  if (urls.length > MAX_URLS) {
    return { error: `A playlist can hold at most ${MAX_URLS} URLs` };
  }
  const invalid = urls.find((url) => url.length > MAX_URL_LENGTH || !isHttpUrl(url));
  if (invalid) return { error: `Not a valid http(s) URL: ${invalid}` };

  return { value: { name, urls } };
}
