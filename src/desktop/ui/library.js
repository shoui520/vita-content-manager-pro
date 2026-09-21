const vitaLibrary = {kind: 'photo', items: [], selected: new Set(), next: 0, loading: false,
  generation: 0, copying: false, selectingAll: false, loadPromise: null, thumbnails: new Map(), transport: 'wifi'};

function updateLibrarySelection() {
  const count = vitaLibrary.selected.size;
  byId('library-count').textContent = `${vitaLibrary.items.length} shown · ${count} selected`;
  byId('copy-from-vita').disabled = !count || vitaLibrary.copying;
  const selectAll = byId('select-all-library');
  selectAll.disabled = vitaLibrary.copying || vitaLibrary.selectingAll;
  selectAll.textContent = vitaLibrary.selectingAll ? 'Selecting…' :
    vitaLibrary.next < 0 && count === vitaLibrary.items.length && count > 0 ? 'Deselect all' : 'Select all';
}

function photoImage(source, item) {
  const image = document.createElement('img');
  image.src = source;
  image.alt = '';
  const ratio = item.width > 0 && item.height > 0 ? item.width / item.height : 1;
  image.style.width = `${100 * Math.max(1, ratio)}%`;
  image.style.height = `${100 * Math.max(1, 1 / ratio)}%`;
  return image;
}

function setLibraryItemSelected(item, selected) {
  if (selected) vitaLibrary.selected.add(item.id);
  else vitaLibrary.selected.delete(item.id);
  const element = document.querySelector(
    vitaLibrary.kind === 'photo' ? `.library-photo[data-id="${item.id}"]` : `#library-rows tr[data-id="${item.id}"]`);
  if (!element) return;
  element.classList.toggle('selected', selected);
  element.setAttribute(vitaLibrary.kind === 'photo' ? 'aria-checked' : 'aria-selected', String(selected));
  element.querySelector('input[type="checkbox"]').checked = selected;
}

function toggleLibraryItem(item, element, checkbox) {
  if (vitaLibrary.selected.has(item.id)) vitaLibrary.selected.delete(item.id);
  else vitaLibrary.selected.add(item.id);
  const selected = vitaLibrary.selected.has(item.id);
  element.classList.toggle('selected', selected);
  element.setAttribute('aria-selected', String(selected));
  checkbox.checked = selected;
  updateLibrarySelection();
}

function addLibraryItem(item) {
  vitaLibrary.items.push(item);
  if (vitaLibrary.kind === 'photo') {
    const tile = document.createElement('div');
    tile.className = 'library-photo';
    tile.dataset.id = item.id;
    tile.tabIndex = 0;
    tile.setAttribute('role', 'checkbox');
    tile.setAttribute('aria-checked', 'false');
    const checkbox = document.createElement('input');
    checkbox.type = 'checkbox';
    checkbox.setAttribute('aria-label', `Select ${item.title || item.name}`);
    const cached = vitaLibrary.thumbnails.get(`${byId('vita-address').value.trim()}|${item.id}`);
    const thumb = cached ? photoImage(cached, item) : document.createElement('span');
    if (!cached) { thumb.className = 'thumb-placeholder'; thumb.textContent = item.thumbnail ? 'Loading…' : 'No preview'; }
    const crop = document.createElement('span');
    crop.className = 'thumb-crop';
    crop.append(thumb);
    const name = document.createElement('span');
    name.className = 'photo-name';
    name.textContent = item.title || item.name;
    name.title = item.name;
    tile.append(checkbox, crop, name);
    const toggle = () => {
      toggleLibraryItem(item, tile, checkbox);
      tile.setAttribute('aria-checked', String(checkbox.checked));
    };
    tile.addEventListener('click', event => { if (event.target !== checkbox) toggle(); });
    checkbox.addEventListener('change', toggle);
    tile.addEventListener('keydown', event => {
      if (event.key === ' ' || event.key === 'Enter') { event.preventDefault(); toggle(); }
    });
    byId('library-photos').append(tile);
  } else {
    const row = document.createElement('tr');
    row.dataset.id = item.id;
    const checkCell = document.createElement('td');
    const checkbox = document.createElement('input');
    checkbox.type = 'checkbox';
    checkbox.setAttribute('aria-label', `Select ${item.title || item.name}`);
    checkCell.append(checkbox);
    const name = document.createElement('td');
    const primary = document.createElement('span');
    primary.className = 'library-primary';
    primary.textContent = item.title || item.name;
    name.append(primary);
    if (vitaLibrary.kind === 'video' && item.name !== item.title) {
      const secondary = document.createElement('span');
      secondary.className = 'library-secondary';
      secondary.textContent = item.name;
      name.append(secondary);
    }
    const detail = document.createElement('td');
    detail.textContent = vitaLibrary.kind === 'music' ? [item.artist, item.album].filter(Boolean).join(' — ') : item.created;
    const size = document.createElement('td');
    size.textContent = prettySize(item.size);
    row.append(checkCell, name, detail, size);
    row.addEventListener('click', event => { if (event.target !== checkbox) toggleLibraryItem(item, row, checkbox); });
    checkbox.addEventListener('change', () => toggleLibraryItem(item, row, checkbox));
    byId('library-rows').append(row);
  }
  updateLibrarySelection();
}

async function loadLibraryPage() {
  if (vitaLibrary.loading) return vitaLibrary.loadPromise;
  if (vitaLibrary.next < 0 || !document.body.classList.contains('from-vita')) return true;
  if (!apiReady) { setStatus('Desktop bridge is not ready.'); return; }
  vitaLibrary.loading = true;
  vitaLibrary.loadPromise = loadLibraryPageBody();
  return vitaLibrary.loadPromise;
}

async function loadLibraryPageBody() {
  const generation = vitaLibrary.generation;
  const offset = vitaLibrary.next;
  const kind = vitaLibrary.kind;
  const ip = byId('vita-address').value.trim();
  setStatus(`Loading ${kind} from PS Vita…`);
  try {
    const page = await pywebview.api.library_page(ip, kind, offset);
    if (generation !== vitaLibrary.generation) return;
    vitaLibrary.transport = page.transport || 'wifi';
    for (const item of page.items) addLibraryItem(item);
    vitaLibrary.next = page.next_offset;
    byId('library-empty').hidden = vitaLibrary.items.length > 0 || vitaLibrary.next >= 0;
    setStatus(`${vitaLibrary.items.length} ${kind} items loaded from PS Vita over ${vitaLibrary.transport.toUpperCase()}.`);
    if (kind === 'photo' && page.items.some(item => item.thumbnail &&
        !vitaLibrary.thumbnails.has(`${ip}|${item.id}`))) {
      try {
        const thumbs = await pywebview.api.photo_thumbnails(ip, offset);
        if (generation === vitaLibrary.generation) for (const [id, source] of Object.entries(thumbs)) {
          vitaLibrary.thumbnails.set(`${ip}|${id}`, source);
          const tile = [...byId('library-photos').children].find(element => element.dataset.id === id);
          if (tile) {
            const item = vitaLibrary.items.find(entry => entry.id === id);
            const image = photoImage(source, item);
            tile.querySelector('.thumb-placeholder')?.replaceWith(image);
          }
        }
        if (generation === vitaLibrary.generation) for (const item of page.items) {
          if (!item.thumbnail || thumbs[item.id]) continue;
          const tile = [...byId('library-photos').children].find(element => element.dataset.id === item.id);
          const placeholder = tile?.querySelector('.thumb-placeholder');
          if (placeholder) placeholder.textContent = 'Preview unavailable';
        }
      } catch (error) { if (generation === vitaLibrary.generation) setStatus(`Photos loaded; previews unavailable: ${error}`); }
    }
    return true;
  } catch (error) {
    if (generation === vitaLibrary.generation) setStatus(`Could not load ${kind}: ${error}`);
    return false;
  } finally {
    if (generation === vitaLibrary.generation) {
      vitaLibrary.loading = false;
      vitaLibrary.loadPromise = null;
      updateLibrarySelection();
      const scroll = byId('library-scroll');
      if (vitaLibrary.next >= 0 && scroll.scrollHeight <= scroll.clientHeight + 100) loadLibraryPage();
    }
  }
}

function openLibrary(kind = 'photo') {
  document.body.classList.add('from-vita');
  byId('vita-library').hidden = false;
  byId('library-actions').hidden = false;
  selectLibraryKind(kind);
}
function closeLibrary() {
  if (vitaLibrary.copying) return;
  vitaLibrary.generation++;
  document.body.classList.remove('from-vita');
  byId('vita-library').hidden = true;
  byId('library-actions').hidden = true;
  byId('download-progress').hidden = true;
  setStatus('Ready');
}
function selectLibraryKind(kind) {
  if (vitaLibrary.copying) return;
  vitaLibrary.kind = kind;
  vitaLibrary.generation++;
  vitaLibrary.items = [];
  vitaLibrary.selected.clear();
  vitaLibrary.next = 0;
  vitaLibrary.loading = false;
  vitaLibrary.loadPromise = null;
  vitaLibrary.selectingAll = false;
  byId('library-photos').replaceChildren();
  byId('library-rows').replaceChildren();
  byId('library-photos').hidden = kind !== 'photo';
  byId('library-table').hidden = kind === 'photo';
  byId('library-detail-heading').textContent = kind === 'music' ? 'Artist / album' : 'Date';
  byId('library-empty').hidden = true;
  byId('library-scroll').scrollTop = 0;
  for (const button of document.querySelectorAll('[data-library-kind]'))
    button.setAttribute('aria-pressed', String(button.dataset.libraryKind === kind));
  updateLibrarySelection();
  loadLibraryPage();
}

function updateDownloadProgress(update) {
  byId('download-progress').hidden = false;
  const percent = update.total ? Math.min(100, 100 * update.transferred / update.total) : 0;
  const bar = byId('download-bar');
  bar.querySelector('.sony-progress-fill').style.width = `${percent}%`;
  bar.setAttribute('aria-valuenow', String(Math.round(percent)));
  byId('download-value').textContent = `${prettySize(update.transferred)} / ${prettySize(update.total)}`;
  byId('download-description').textContent = `Copying ${update.name} · ${update.copied}/${update.count} files`;
}

byId('browse-vita').addEventListener('click', () => openLibrary());
byId('back-to-vita').addEventListener('click', closeLibrary);
for (const button of document.querySelectorAll('[data-library-kind]'))
  button.addEventListener('click', () => selectLibraryKind(button.dataset.libraryKind));
byId('refresh-library').addEventListener('click', () => selectLibraryKind(vitaLibrary.kind));
byId('select-all-library').addEventListener('click', async () => {
  if (vitaLibrary.selectingAll || vitaLibrary.copying) return;
  if (vitaLibrary.next < 0 && vitaLibrary.items.length > 0 &&
      vitaLibrary.selected.size === vitaLibrary.items.length) {
    for (const item of vitaLibrary.items) setLibraryItemSelected(item, false);
    updateLibrarySelection();
    return;
  }
  const generation = vitaLibrary.generation;
  vitaLibrary.selectingAll = true;
  updateLibrarySelection();
  try {
    for (const item of vitaLibrary.items) setLibraryItemSelected(item, true);
    while (vitaLibrary.next >= 0 && generation === vitaLibrary.generation) {
      if (!await loadLibraryPage()) break;
      for (const item of vitaLibrary.items) setLibraryItemSelected(item, true);
    }
  } finally {
    vitaLibrary.selectingAll = false;
    updateLibrarySelection();
  }
});
byId('library-scroll').addEventListener('scroll', () => {
  const area = byId('library-scroll');
  if (area.scrollTop + area.clientHeight >= area.scrollHeight - 250) loadLibraryPage();
});
byId('vita-address').addEventListener('change', () => {
  vitaLibrary.thumbnails.clear();
  if (document.body.classList.contains('from-vita')) selectLibraryKind(vitaLibrary.kind);
});
byId('cancel-download').addEventListener('click', async () => {
  byId('cancel-download').disabled = true;
  byId('download-description').textContent = 'Stopping after the current network read…';
  await pywebview.api.cancel_transfer();
});
byId('copy-from-vita').addEventListener('click', async () => {
  if (!vitaLibrary.selected.size || vitaLibrary.copying) return;
  const selected = vitaLibrary.items.filter(item => vitaLibrary.selected.has(item.id));
  vitaLibrary.copying = true;
  byId('cancel-download').disabled = false;
  updateLibrarySelection();
  try {
    const result = await pywebview.api.copy_from_vita(byId('vita-address').value.trim(),
      vitaLibrary.kind, selected.map(item => ({id: item.id,
        name: vitaLibrary.kind === 'music' && item.title
          ? `${item.title}${item.name.lastIndexOf('.') >= 0 ? item.name.slice(item.name.lastIndexOf('.')) : ''}` : item.name,
        size: item.size, sidecar_size: item.sidecar_size || 0,
        sidecar_id: item.sidecar_id || ''})), vitaLibrary.transport);
    if (result.cancelled && !result.copied.length) setStatus('Copy cancelled.');
    else setStatus(`${result.copied.length} copied to PC${result.failed.length ? `; ${result.failed.length} failed: ${result.failed[0].error}` : '.'}`);
    for (const item of result.copied) vitaLibrary.selected.delete(item.id);
  } catch (error) { setStatus(`Copy failed: ${error}`); }
  finally {
    vitaLibrary.copying = false;
    byId('cancel-download').disabled = true;
    updateLibrarySelection();
  }
});
