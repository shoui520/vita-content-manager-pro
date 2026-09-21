const fileMap = new Map();
let apiReady = false;
let converting = false;
let connected = false;
let connectionProbe = 0;
const byId = id => document.getElementById(id);
const prettySize = bytes => bytes < 1048576 ? `${(bytes / 1024).toFixed(0)} KB` : `${(bytes / 1048576).toFixed(1)} MB`;
const setStatus = message => { byId('status-line').textContent = message; };
function setConnection(online, ip) {
  if (ip !== byId('vita-address').value.trim()) return;
  connected = online;
  byId('connection-label').textContent = online ? 'Connected' : 'Not connected';
  byId('connection-dot').classList.toggle('online', online);
}
function noteVitaConnection(ip) {
  // A completed Vita request is stronger evidence than an older probe result.
  if (ip !== byId('vita-address').value.trim()) return;
  ++connectionProbe;
  setConnection(true, ip);
}
async function probeVita(silent = false) {
  if (!apiReady) return;
  const ip = byId('vita-address').value.trim();
  const probe = ++connectionProbe;
  if (!silent) {
    byId('probe').disabled = true;
    setStatus('Checking connection…');
  }
  try {
    const result = await pywebview.api.probe_vita(ip);
    if (probe !== connectionProbe || ip !== byId('vita-address').value.trim()) return;
    setConnection(Boolean(result.online), ip);
    if (!silent) setStatus(result.online ? 'Ready to transfer and sync.' : result.message);
  } catch (error) {
    if (probe !== connectionProbe || ip !== byId('vita-address').value.trim()) return;
    setConnection(false, ip);
    if (!silent) setStatus(String(error));
  } finally {
    if (!silent) byId('probe').disabled = false;
  }
}
const sortCollator = new Intl.Collator(undefined, {numeric: true, sensitivity: 'base'});
let sortColumn = null;
let sortDirection = 1;
const statusText = item => (item.status || (item.eligible ? 'Ready' : item.defaults ? 'Needs conversion' : 'Cannot read file')) + (item.sidecars?.length ? ' + M4T' : '');
function sortValue(item, column) {
  if (column === 'size') return item.size || 0;
  if (column === 'eligible') return Number(Boolean(item.eligible));
  if (column === 'status') return statusText(item);
  if (column === 'format') return item.format || 'Unknown';
  return item.name || '';
}
const icons = {
  photo: '<rect x="3" y="6" width="18" height="14" rx="2"/><path d="M8 6l2-3h4l2 3"/><circle cx="12" cy="13" r="4"/>',
  music: '<path d="M9 17V5l11-2v12M9 8l11-2"/><ellipse cx="6" cy="18" rx="3" ry="2"/><ellipse cx="17" cy="16" rx="3" ry="2"/>',
  video: '<rect x="3" y="4" width="18" height="16" rx="1"/><path d="M7 4v16M17 4v16M3 9h4M3 15h4M17 9h4M17 15h4"/>'
};

function render() {
  const list = byId('file-list');
  list.replaceChildren();
  const items = [...fileMap.values()];
  if (sortColumn) items.sort((a, b) => {
    const first = sortValue(a, sortColumn);
    const second = sortValue(b, sortColumn);
    const comparison = typeof first === 'number' ? first - second : sortCollator.compare(first, second);
    return comparison * sortDirection;
  });
  for (const item of items) {
    const row = document.createElement('tr');
    row.className = item.selected ? 'selected' : '';
    row.dataset.path = item.path;
    const checkCell = document.createElement('td');
    const checkbox = document.createElement('input');
    checkbox.type = 'checkbox';
    checkbox.checked = item.selected;
    checkbox.disabled = converting;
    checkbox.setAttribute('aria-label', `Select ${item.name}`);
    checkbox.addEventListener('change', () => {
      item.selected = checkbox.checked;
      updateSelection();
    });
    checkCell.append(checkbox);
    const nameCell = document.createElement('td');
    const name = document.createElement('div');
    name.className = 'file-name';
    const icon = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    icon.setAttribute('viewBox', '0 0 24 24');
    icon.setAttribute('class', `media-icon ${item.kind}`);
    icon.setAttribute('fill', 'none');
    icon.setAttribute('stroke', 'currentColor');
    icon.setAttribute('stroke-width', '1.5');
    icon.innerHTML = icons[item.kind] || '';
    const filename = document.createElement('span');
    filename.textContent = item.name;
    filename.title = item.path + (item.video_folder ? `\nVideos folder: ${item.video_folder}` : '');
    name.append(icon, filename);
    if (item.video_folder) {
      const folder = document.createElement('small');
      folder.className = 'video-folder-label';
      folder.textContent = `Folder: ${item.video_folder}`;
      name.append(folder);
    }
    nameCell.append(name);
    const type = document.createElement('td');
    type.textContent = item.format || 'Unknown';
    type.title = {photo: 'Photo', music: 'Music', video: 'Video'}[item.kind] || 'Unrecognized file';
    const size = document.createElement('td');
    size.textContent = prettySize(item.size);
    const progress = document.createElement('td');
    progress.className = `file-status ${item.result || ''}`;
    progress.textContent = statusText(item);
    progress.title = item.detail || progress.textContent;
    const eligible = document.createElement('td');
    const light = document.createElement('span');
    light.className = `eligibility ${item.eligible ? 'yes' : 'no'}`;
    light.textContent = item.eligible ? 'Yes' : 'No';
    eligible.title = item.eligible ? 'Passes Vita format checks' : (item.reasons || []).join('\n');
    eligible.append(light);
    row.addEventListener('click', event => {
      if (converting || event.target === checkbox) return;
      item.selected = !item.selected;
      updateSelection();
    });
    row.addEventListener('dblclick', () => openOptions(item.path));
    row.append(checkCell, nameCell, type, size, eligible, progress);
    list.append(row);
  }
  updateSelection();
  for (const header of document.querySelectorAll('th[data-sort]')) {
    const active = header.dataset.sort === sortColumn;
    header.setAttribute('aria-sort', active ? (sortDirection === 1 ? 'ascending' : 'descending') : 'none');
    header.querySelector('button').title = `Sort ${header.textContent.trim()} ${active && sortDirection === 1 ? 'descending' : 'ascending'}`;
  }
  byId('empty-state').hidden = fileMap.size > 0;
  byId('choose-files').disabled = converting || !apiReady;
  byId('choose-video-folder').disabled = converting || !apiReady;
  byId('empty-add').disabled = converting || !apiReady;
}

function updateSelection() {
  for (const row of byId('file-list').rows) {
    const checkbox = row.querySelector('input[type="checkbox"]');
    const selected = Boolean(fileMap.get(row.dataset.path)?.selected);
    checkbox.checked = selected;
    row.classList.toggle('selected', selected);
    row.setAttribute('aria-selected', String(selected));
  }
  const selected = [...fileMap.values()].filter(item => item.selected).length;
  byId('file-count').textContent = `${fileMap.size} ${fileMap.size === 1 ? 'file' : 'files'}${fileMap.size ? ` · ${selected} selected` : ''}`;
  byId('select-all').checked = fileMap.size > 0 && selected === fileMap.size;
  byId('select-all').indeterminate = selected > 0 && selected < fileMap.size;
  byId('select-all').disabled = converting || !fileMap.size;
  byId('remove-selected').disabled = converting || !selected;
  byId('convert').disabled = converting || !apiReady || !selected;
  byId('copy-to-vita').disabled = converting || !apiReady || !selected;
}

function addItems(items) {
  for (const item of items) {
    if (!fileMap.has(item.path)) fileMap.set(item.path, {...item, selected: false});
    else if (item.video_folder) fileMap.get(item.path).video_folder = item.video_folder;
  }
  render();
  setStatus('Ready');
}
function refreshBridgeState() {
  const wasReady = apiReady;
  apiReady = Boolean(window.pywebview && window.pywebview.api &&
                     typeof window.pywebview.api.choose_files === 'function');
  render();
  if (apiReady && !wasReady) queueMicrotask(() => probeVita(true));
  return apiReady;
}
window.addEventListener('pywebviewready', refreshBridgeState);
if (!refreshBridgeState()) {
  const bridgePoll = setInterval(() => { if (refreshBridgeState()) clearInterval(bridgePoll); }, 100);
}
async function chooseFiles() {
  if (!apiReady) return;
  setStatus('Inspecting selected files…');
  try { addItems(await pywebview.api.choose_files()); setStatus('Ready'); }
  catch (error) { setStatus(String(error)); }
}
byId('choose-files').addEventListener('click', chooseFiles);
byId('choose-video-folder').addEventListener('click', async () => {
  if (!apiReady || converting) return;
  setStatus('Inspecting video folder…');
  try {
    const items = await pywebview.api.choose_video_folder();
    addItems(items);
    setStatus(items.length ? `Added ${items.length} videos to folder “${items[0].video_folder}”.` : 'No videos added.');
  } catch (error) { setStatus(String(error)); }
});
byId('empty-add').addEventListener('click', chooseFiles);
for (const header of document.querySelectorAll('th[data-sort]')) {
  header.querySelector('button').addEventListener('click', () => {
    if (sortColumn === header.dataset.sort) sortDirection *= -1;
    else { sortColumn = header.dataset.sort; sortDirection = 1; }
    render();
  });
}
byId('select-all').addEventListener('change', event => {
  for (const item of fileMap.values()) item.selected = event.target.checked;
  render();
});
byId('remove-selected').addEventListener('click', () => {
  for (const [path, item] of fileMap) if (item.selected) fileMap.delete(path);
  render();
});
byId('convert').addEventListener('click', () => openOptions());
byId('vita-address').addEventListener('input', () => {
  ++connectionProbe;
  setConnection(false, byId('vita-address').value.trim());
  render();
});
byId('probe').addEventListener('click', () => probeVita());
function updateSyncProgress(update) {
  byId('sync-progress').hidden = false;
  const setBar = (id, value) => {
    value = Math.max(0, Math.min(100, value || 0));
    byId(id).querySelector('.sony-progress-fill').style.width = `${value}%`;
    byId(id).setAttribute('aria-valuenow', String(Math.round(value)));
  };
  setBar('transfer-progress', update.total ? 100 * update.transferred / update.total : 0);
  setBar('import-progress', update.count ? 100 * ((update.imported || 0) + (update.failed || 0)) / update.count : 0);
  byId('transfer-value').textContent = `${prettySize(update.transferred || 0)} / ${prettySize(update.total || 0)}`;
  byId('import-value').textContent = `${(update.imported || 0) + (update.failed || 0)} / ${update.count || 0} files`;
  byId('sync-description').textContent = update.phase === 'complete' ? (update.failed ? 'Completed with failed items' : 'Sync complete') : update.phase === 'import' ? 'Importing into the Vita media library…' : `Transferring${update.name ? ': ' + update.name : '…'}`;
  const states = ['Waiting', 'Transferring', 'Transferred', 'Importing', 'Imported', 'Import failed'];
  for (const result of update.items || []) {
    const item = fileMap.get(result.path);
    if (item) item.status = states[result.state];
  }
  for (const row of byId('file-list').rows) {
    const item = fileMap.get(row.dataset.path);
    if (item) row.querySelector('.file-status').textContent = statusText(item);
  }
}
byId('cancel-sync').addEventListener('click', async () => {
  byId('cancel-sync').disabled = true;
  byId('sync-description').textContent = 'Stopping after the current operation…';
  await pywebview.api.cancel_transfer();
});
byId('copy-to-vita').addEventListener('click', async () => {
  const ip = byId('vita-address').value.trim();
  const selected = [...fileMap.values()].filter(item => item.selected);
  if (selected.some(item => !item.eligible)) {
    setStatus('Convert the selected ineligible files before sending them.');
    return;
  }
  converting = true;
  byId('cancel-sync').disabled = false;
  updateSyncProgress({phase: 'transfer', count: selected.length, total: selected.reduce((sum, item) => sum + item.size, 0)});
  setStatus('Preparing transfer…');
  render();
  try {
    const results = await pywebview.api.send_files(ip,
      selected.map(item => ({path: item.path, sidecars: item.sidecars || [], video_folder: item.video_folder || ''})));
    for (const result of results) {
      const item = [...fileMap.values()].find(value => value.path === result.path);
      if (!item) continue;
      item.status = result.ok ? 'Imported' : 'Import failed';
      item.result = result.ok ? 'done' : 'error';
      item.detail = result.ok ? 'Imported into the Vita media library.' : result.error;
      if (result.ok) item.selected = false;
    }
    const received = results.filter(result => result.ok).length;
    const failed = results.length - received;
    setStatus(failed ? `${received} imported; ${failed} failed.` : `${received} imported into the Vita media library.`);
    noteVitaConnection(ip);
  } catch (error) { setStatus(String(error)); byId('sync-description').textContent = String(error); }
  finally { converting = false; byId('cancel-sync').disabled = true; render(); }
});
const zone = byId('dropzone');
let dropReady = false;
let dropTimer = null;
function acknowledgeDrop() {
  clearTimeout(dropTimer);
  if (converting) return false;
  setStatus('Inspecting dropped files…');
  return true;
}
function finishDrop(items, error) {
  clearTimeout(dropTimer);
  if (items.length) addItems(items);
  setStatus(error || 'Ready');
}
// No frame loop: only visibility/focus events control compositor animations.
let ambientVisible = true;
function updateAmbient() {
  document.body.classList.toggle('ambient-paused', document.hidden || !document.hasFocus() || !ambientVisible);
}
document.addEventListener('visibilitychange', updateAmbient);
window.addEventListener('focus', updateAmbient);
window.addEventListener('blur', updateAmbient);
new IntersectionObserver(entries => {
  ambientVisible = entries[0].isIntersecting && entries[0].intersectionRect.height > 0;
  updateAmbient();
}).observe(document.querySelector('.ambient-space'));
updateAmbient();
zone.addEventListener('dragover', event => { event.preventDefault(); if (!converting) zone.classList.add('drag'); });
zone.addEventListener('dragleave', () => zone.classList.remove('drag'));
zone.addEventListener('drop', event => {
  event.preventDefault();
  zone.classList.remove('drag');
  if (converting || !apiReady) return;
  if (!dropReady) { setStatus('Drag and drop is not ready. Please use Add files.'); return; }
  // The Python DOM drop handler supplies real paths on Windows/WebView2.
  setStatus('Inspecting dropped files…');
  clearTimeout(dropTimer);
  dropTimer = setTimeout(() => setStatus('Dropped files were not received. Please try again or use Add files.'), 5000);
});
