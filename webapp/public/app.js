const form = document.getElementById("playlist-form");
const select = document.getElementById("playlist-select");
const usedByHint = document.getElementById("used-by-hint");
const nameInput = document.getElementById("playlist-name");
const urlsInput = document.getElementById("playlist-urls");
const urlCount = document.getElementById("url-count");
const saveBtn = document.getElementById("save-btn");
const deleteBtn = document.getElementById("delete-btn");
const statusEl = document.getElementById("status");
const buttonsStatus = document.getElementById("buttons-status");
const buttonSelects = [...document.querySelectorAll("select[data-color]")];

const BUTTON_LABELS = { red: "Red", blu: "Blue", yel: "Yellow", grn: "Green" };
const listFormat = new Intl.ListFormat("en", { type: "conjunction" });

// Summaries from GET /api/playlists: { id, name, count, updatedAt }.
let playlists = [];
// From GET /api/buttons: { color, playlistId, name } for each button on the device.
let buttons = [];

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

function setStatus(message, kind = "", el = statusEl) {
  el.textContent = message;
  el.className = `status ${kind}`;
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

// Labels of the buttons that play from the given playlist, e.g. ["Red", "Green"]
function buttonsUsing(id) {
  return buttons.filter((b) => b.playlistId === id).map((b) => BUTTON_LABELS[b.color]);
}

function renderButtons() {
  for (const buttonSelect of buttonSelects) {
    const options = playlists.map((p) => new Option(`${p.name} (${p.count})`, p.id));
    buttonSelect.replaceChildren(new Option("No playlist", ""), ...options);
    const button = buttons.find((b) => b.color === buttonSelect.dataset.color);
    buttonSelect.value = button?.playlistId ?? "";
  }
}

function renderUsedBy() {
  const id = select.value;
  const labels = buttonsUsing(id);
  if (!id) {
    usedByHint.textContent = "";
  } else if (labels.length === 0) {
    usedByHint.textContent = "Not on any button.";
  } else {
    const noun = labels.length === 1 ? "button" : "buttons";
    usedByHint.textContent = `Played by the ${listFormat.format(labels)} ${noun}.`;
  }
}

function renderSelect(selectedId = "") {
  const options = playlists.map((p) => new Option(`${p.name} (${p.count})`, p.id));
  select.replaceChildren(new Option("+ New playlist", ""), ...options);
  select.value = selectedId;
  deleteBtn.disabled = !selectedId;
  renderUsedBy();
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

async function loadPlaylists() {
  setBusy(true);
  try {
    [playlists, buttons] = await Promise.all([api("/playlists"), api("/buttons")]);
    renderButtons();
    const firstId = playlists[0]?.id ?? "";
    renderSelect(firstId);
    if (firstId) fillForm(await api(playlistPath(firstId)));
    setStatus("");
  } catch (err) {
    setStatus(`Could not load playlists: ${err.message}`, "error");
  } finally {
    setBusy(false);
  }
}

for (const buttonSelect of buttonSelects) {
  buttonSelect.addEventListener("change", async () => {
    const { color } = buttonSelect.dataset;
    const label = BUTTON_LABELS[color];
    const previous = buttons.find((b) => b.color === color)?.playlistId ?? "";
    buttonSelect.disabled = true;
    try {
      const button = await api(`/buttons/${color}`, {
        method: "PUT",
        body: JSON.stringify({ playlistId: buttonSelect.value || null }),
      });
      buttons = buttons.map((b) => (b.color === color ? button : b));
      const message = button.name ? `${label} plays "${button.name}".` : `${label} has no playlist.`;
      setStatus(message, "success", buttonsStatus);
      renderUsedBy();
    } catch (err) {
      buttonSelect.value = previous;
      setStatus(`Could not change ${label}: ${err.message}`, "error", buttonsStatus);
    } finally {
      buttonSelect.disabled = false;
    }
  });
}

select.addEventListener("change", async () => {
  const id = select.value;
  setStatus("");
  renderUsedBy();
  if (!id) {
    fillForm(null);
    deleteBtn.disabled = true;
    nameInput.focus();
    return;
  }
  setBusy(true);
  try {
    fillForm(await api(playlistPath(id)));
  } catch (err) {
    setStatus(`Could not load playlist: ${err.message}`, "error");
  } finally {
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
    buttons = buttons.map((b) => (b.playlistId === playlist.id ? { ...b, name: playlist.name } : b));
    renderButtons();
    renderSelect(playlist.id);
    fillForm(playlist);
    setStatus(`Saved "${playlist.name}".`, "success");
  } catch (err) {
    setStatus(`Save failed: ${err.message}`, "error");
  } finally {
    setBusy(false);
  }
});

deleteBtn.addEventListener("click", async () => {
  const id = select.value;
  const playlist = playlists.find((p) => p.id === id);
  if (!playlist) return;
  const labels = buttonsUsing(id);
  let question = `Delete playlist "${playlist.name}"?`;
  if (labels.length) {
    question += ` The ${listFormat.format(labels)} ${labels.length === 1 ? "button" : "buttons"}`;
    question += " will be left without a playlist.";
  }
  if (!confirm(question)) return;

  setBusy(true);
  try {
    await api(playlistPath(id), { method: "DELETE" });
    playlists = playlists.filter((p) => p.id !== id);
    // The server unmaps the playlist from any buttons that used it.
    buttons = buttons.map((b) => (b.playlistId === id ? { ...b, playlistId: null, name: null } : b));
    renderButtons();
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
