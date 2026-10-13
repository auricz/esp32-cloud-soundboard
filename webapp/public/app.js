const form = document.getElementById("playlist-form");
const select = document.getElementById("playlist-select");
const activeHint = document.getElementById("active-hint");
const nameInput = document.getElementById("playlist-name");
const urlsInput = document.getElementById("playlist-urls");
const urlCount = document.getElementById("url-count");
const saveBtn = document.getElementById("save-btn");
const deleteBtn = document.getElementById("delete-btn");
const statusEl = document.getElementById("status");

// Summaries from GET /api/playlists: { id, name, count, updatedAt }.
let playlists = [];
// The playlist /api/random draws from (server-side selection), or null.
let activeId = null;

async function api(path, options = {}) {
  const res = await fetch(`/api${path}`, {
    headers: { "Content-Type": "application/json" },
    ...options,
  });
  if (res.status === 204) return null;
  const data = await res.json().catch(() => ({}));
  if (!res.ok) throw new Error(data.error || `Request failed (${res.status})`);
  return data;
}

const playlistPath = (id) => `/playlists/${encodeURIComponent(id)}`;

function setStatus(message, kind = "") {
  statusEl.textContent = message;
  statusEl.className = `status ${kind}`;
}

function setBusy(busy) {
  for (const el of [select, nameInput, urlsInput, saveBtn]) el.disabled = busy;
  deleteBtn.disabled = busy || !select.value;
}

function parseUrls() {
  return urlsInput.value.split(/\s+/).filter(Boolean);
}

function isWavUrl(value) {
  try {
    return new URL(value).pathname.toLowerCase().endsWith(".wav");
  } catch {
    return false;
  }
}

function updateUrlCount() {
  const urls = parseUrls();
  const nonWav = urls.filter((url) => !isWavUrl(url)).length;
  let text = `${urls.length} URL${urls.length === 1 ? "" : "s"}`;
  if (nonWav) text += ` · ${nonWav} not .wav`;
  urlCount.textContent = text;
  urlCount.classList.toggle("warning", nonWav > 0);
}

function renderActiveHint() {
  const active = playlists.find((p) => p.id === activeId);
  if (!active) {
    activeHint.textContent = "No playlist is active, so /api/random has nothing to play.";
    activeHint.classList.add("warning");
    return;
  }
  const link = document.createElement("a");
  link.href = "/api/random?format=json";
  link.target = "_blank";
  link.textContent = "/api/random";
  activeHint.replaceChildren(link, ` plays from "${active.name}"`);
  activeHint.classList.toggle("warning", active.count === 0);
  if (active.count === 0) activeHint.append(", which has no URLs");
}

function renderSelect(selectedId = "") {
  const options = playlists.map((p) => new Option(`${p.name} (${p.count})`, p.id));
  select.replaceChildren(new Option("+ New playlist", ""), ...options);
  select.value = selectedId;
  deleteBtn.disabled = !selectedId;
  renderActiveHint();
}

function fillForm(playlist) {
  nameInput.value = playlist?.name ?? "";
  urlsInput.value = playlist?.urls.join("\n") ?? "";
  updateUrlCount();
}

function upsertSummary(playlist) {
  const summary = {
    id: playlist.id,
    name: playlist.name,
    count: playlist.urls.length,
    updatedAt: playlist.updatedAt,
  };
  playlists = playlists.filter((p) => p.id !== playlist.id).concat(summary);
  playlists.sort((a, b) => a.name.localeCompare(b.name));
}

async function setActive(id) {
  const selection = await api("/selection", {
    method: "PUT",
    body: JSON.stringify({ playlistId: id }),
  });
  activeId = selection.playlistId;
}

async function loadPlaylists() {
  setBusy(true);
  try {
    const [list, selection] = await Promise.all([api("/playlists"), api("/selection")]);
    playlists = list;
    activeId = selection.playlistId;
    renderSelect(activeId ?? "");
    if (activeId) fillForm(await api(playlistPath(activeId)));
    setStatus("");
  } catch (err) {
    setStatus(`Could not load playlists: ${err.message}`, "error");
  } finally {
    setBusy(false);
  }
}

// Picking an existing playlist makes it the one /api/random draws from.
// "+ New playlist" leaves the active playlist alone until the new one is saved.
select.addEventListener("change", async () => {
  const id = select.value;
  if (!id) {
    fillForm(null);
    setStatus("");
    deleteBtn.disabled = true;
    nameInput.focus();
    return;
  }
  setBusy(true);
  try {
    const [playlist] = await Promise.all([api(playlistPath(id)), setActive(id)]);
    fillForm(playlist);
    setStatus(`"${playlist.name}" is now active.`, "success");
  } catch (err) {
    setStatus(`Could not switch playlist: ${err.message}`, "error");
  } finally {
    renderActiveHint();
    setBusy(false);
  }
});

urlsInput.addEventListener("input", updateUrlCount);

form.addEventListener("submit", async (event) => {
  event.preventDefault();
  const name = nameInput.value.trim();
  if (!name) {
    setStatus("Give the playlist a label first.", "error");
    nameInput.focus();
    return;
  }

  const id = select.value;
  setBusy(true);
  try {
    const playlist = await api(id ? playlistPath(id) : "/playlists", {
      method: id ? "PUT" : "POST",
      body: JSON.stringify({ name, urls: parseUrls() }),
    });
    upsertSummary(playlist);
    renderSelect(playlist.id);
    fillForm(playlist);
    setStatus(`Saved "${playlist.name}".`, "success");
    if (!id) {
      try {
        await setActive(playlist.id);
        renderActiveHint();
        setStatus(`Saved "${playlist.name}" and made it active.`, "success");
      } catch (err) {
        setStatus(`Saved "${playlist.name}", but could not make it active: ${err.message}`, "error");
      }
    }
  } catch (err) {
    setStatus(`Save failed: ${err.message}`, "error");
  } finally {
    setBusy(false);
  }
});

deleteBtn.addEventListener("click", async () => {
  const id = select.value;
  const playlist = playlists.find((p) => p.id === id);
  if (!playlist || !confirm(`Delete playlist "${playlist.name}"?`)) return;

  setBusy(true);
  try {
    await api(playlistPath(id), { method: "DELETE" });
    playlists = playlists.filter((p) => p.id !== id);
    // The server clears the selection when the active playlist is deleted.
    if (activeId === id) activeId = null;
    renderSelect();
    fillForm(null);
    setStatus(`Deleted "${playlist.name}".`, "success");
  } catch (err) {
    setStatus(`Delete failed: ${err.message}`, "error");
  } finally {
    setBusy(false);
  }
});

loadPlaylists();
