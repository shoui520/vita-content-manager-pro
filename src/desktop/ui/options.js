let optionsKey = null;
let conversionStopRequested = false;
let activeTab = '';
const dialog = byId('options-dialog');
let dialogClosing = false;
let dialogCloseTimer = null;
function closeOptions() {
  if (converting || dialogClosing || !dialog.open) return;
  dialogClosing = true;
  dialog.inert = true;
  dialog.classList.add('closing');
  const delay = matchMedia('(prefers-reduced-motion: reduce)').matches ? 0 : 160;
  dialogCloseTimer = setTimeout(() => {
    dialog.close();
    resetDialogClose();
  }, delay);
}
function resetDialogClose() {
  clearTimeout(dialogCloseTimer);
  dialog.classList.remove('closing');
  dialog.inert = false;
  dialogClosing = false;
}
dialog.addEventListener('close', () => { if (!dialog.open) resetDialogClose(); });
const selectField = (key, label, choices, help = '') => ({key, label, choices, help});
const slider = (key, label, min, max, step, help = '') => ({key, label, min, max, step, help});
const sizes = [['vita', 'PS Vita screen 960 × 544'], ['1080', '1080p'], ['720', '720p']];
const videoSizes = [...sizes, ['480', '480p'], ['360', '360p'], ['240', '240p'], ['144', '144p']];
const sampleRates = [['44100', '44.1 kHz — music / CD quality'], ['48000', '48 kHz — video / studio standard']];

function currentItem() { return fileMap.get(optionsKey); }
function updateConversionProgress(update) {
  if (!converting || update.source && update.source !== currentItem()?.path) return;
  byId('conversion-progress').hidden = false;
  if (update.source && !conversionStopRequested) byId('stop-conversion').disabled = false;
  byId('conversion-phase').textContent = update.phase;
  const bar = byId('conversion-bar');
  const known = Number.isFinite(update.percent);
  bar.classList.toggle('indeterminate', !known);
  if (known) bar.setAttribute('aria-valuenow', update.percent.toFixed(1));
  else bar.removeAttribute('aria-valuenow');
  bar.querySelector('.sony-progress-fill').style.width = known ? `${update.percent}%` : '25%';
  const clock = seconds => `${Math.floor(seconds / 60)}:${String(Math.floor(seconds % 60)).padStart(2, '0')}`;
  byId('conversion-value').textContent = known ? `${Math.floor(update.percent)}% · ${clock(update.seconds)} / ${clock(update.duration)}${update.speed && update.speed !== 'N/A' ? ` · ${update.speed.trim()}` : ''}` : '';
}
function openOptions(path) {
  if (converting || dialogClosing || dialog.open) return;
  const selected = [...fileMap.entries()].filter(([, item]) => path ? item.path === path : item.selected);
  if (!selected.length) return;
  byId('options-file').replaceChildren();
  for (const [key, item] of selected) byId('options-file').add(new Option(item.name, key));
  optionsKey = selected[0][0];
  activeTab = '';
  byId('options-result').textContent = '';
  byId('conversion-progress').hidden = true;
  drawOptions();
  dialog.showModal();
}

function tabsFor(item) {
  if (item.kind === 'music') return [['mp3', 'MP3'], ['aac', 'AAC'], ['wav', 'WAV']];
  if (item.kind === 'photo') return [['jpeg', 'JPEG'], ['png', 'PNG'], ['bmp', 'BMP'], ['tiff', 'TIFF']];
  if (item.kind === 'video') return [['picture', 'Picture'], ['quality', 'Quality'], ...(item.format === 'GIF' ? [['animation', 'Animation']] : []), ['audio', 'Audio'], ['subtitles', 'Subtitles']];
  return [];
}

function fieldsFor(item, options) {
  if (item.kind === 'music' || (activeTab === 'audio' && options.output !== 'mp4')) {
    const fields = [];
    const extracting = item.kind === 'video';
    const bitrate = extracting ? 'extract_bitrate' : 'bitrate';
    if (extracting) fields.push(...videoAudioFields(item, options));
    if (options.output === 'mp3') {
      fields.push(selectField('mode', 'File size / quality', [['cbr', 'Constant bitrate (CBR)'], ['vbr', 'Variable bitrate (VBR)']], 'CBR gives a predictable file size. VBR spends more data on complex audio.'));
      if (options.mode === 'cbr') fields.push(selectField(bitrate, 'Audio bitrate', [32, 64, 96, 128, 160, 192, 224, 256, 320].map(v => [String(v), `${v} kbps`]), 'Higher bitrate uses more space and can preserve more detail.'));
      else fields.push(slider('vbr', 'Audio quality', 0, 9, 1, '0 is the highest quality / largest file. 9 is the smallest file. 2 is a good default.'));
    } else if (options.output === 'aac') fields.push(slider(bitrate, 'Audio bitrate (kbps)', 32, 320, 8, 'AAC-LC: higher bitrate preserves more detail and uses more space.'));
    fields.push(selectField('sample_rate', 'Sample rate', sampleRates, options.output === 'wav' ? 'Uncompressed 16-bit PCM. Larger files; no lossy compression. WAV may not preserve cover art or every tag.' : 'Changing this does not restore detail missing from the original.'));
    return fields;
  }
  if (item.kind === 'photo') return [
    selectField('resolution', 'Image size', [['original', 'Keep resolution (reduce only to the format limit)'], ...sizes], 'Maintain proportions. JPEG allows up to 40 megapixels; other formats have lower limits.'),
    ...(options.output === 'jpeg' ? [slider('quality', 'JPEG quality', 50, 100, 1, '90 is a good balance. Higher values use more space; baseline encoding is automatic.')] : []),
    ...(options.output !== 'png' ? [selectField('background', 'Transparent areas', [['white', 'White background'], ['black', 'Black background']], 'Used only when the source contains transparency.')] : [])
  ];
  if (activeTab === 'picture') return [
    selectField('resolution', 'Picture size', videoSizes),
    selectField('fit', 'Fit picture', [['contain', 'Fit inside — keep proportions'], ['pad', 'Fit inside — add black bars'], ['stretch', 'Stretch to fill — change proportions'], ['width', 'Fill width — crop excess height'], ['height', 'Fill height — crop excess width']], 'Fit inside avoids cropping and does not enlarge smaller sources.'),
    selectField('fps', 'Frame rate', [['auto', 'Keep source rate (cap at Vita limit)'], ...[15, 24, 25, 30, 50, 60].map(v => [String(v), `${v} fps`])], 'Lower rates can save space but reduce smoothness. Higher rates do not create new motion.')
  ];
  if (activeTab === 'quality') return [
    selectField('rate_mode', 'Video size / quality', [['quality', 'Consistent quality (recommended)'], ['bitrate', 'Target bitrate — predictable size']]),
    ...(options.rate_mode === 'quality' ? [slider('quality', 'Compression', 16, 28, 1, '16: highest quality / largest file. 19: recommended. 28: smaller file / less detail.')] : [slider('bitrate', 'Video bitrate (kbps)', 500, 16000, 500, 'Higher values preserve more detail and use more space.')]),
    selectField('preset', 'Conversion speed', [['ultrafast', 'Fastest — larger files'], ['fast', 'Fast'], ['medium', 'Balanced'], ['slow', 'Slow — recommended'], ['slower', 'Slowest — better compression']], 'Slower encoding spends more time compressing the picture efficiently.')
  ];
  if (activeTab === 'animation') return [slider('loops', 'Repeat animation', 1, 100, 1, 'Number of loops included in the MP4. Videos stop at the end; they do not loop forever.')];
  if (activeTab === 'audio') return [
    ...videoAudioFields(item, options),
    slider('audio_bitrate', 'Audio bitrate (kbps)', 32, 320, 8, 'AAC-LC stereo. 128–192 kbps is usually enough for a video.'),
    selectField('sample_rate', 'Sample rate', sampleRates)
  ];
  if (activeTab === 'subtitles') return [
    selectField('subtitle_track', 'Source subtitles', [['none', 'No subtitles'], ...(item.subtitle_tracks || []).map(t => [t.id, t.label, !t.supported])], 'Choose one track for a matching M4T sidecar. Bitmap subtitles need OCR and cannot be selected.'),
    selectField('subtitle_filter', 'ASS text to keep', [['dialogue', 'Dialogue — omit signs, songs and karaoke'], ['all', 'All text — include signs and lyrics']], 'Dialogue mode keeps background speech too. Style-name filtering is a heuristic; all positioning is replaced with the tested centered, wrapped bottom captions.'),
    selectField('subtitle_language', 'Subtitle language', [['auto', 'Use the source track language'], ['eng', 'English'], ['jpn', 'Japanese'], ['fra', 'French'], ['deu', 'German'], ['spa', 'Spanish'], ['ita', 'Italian'], ['por', 'Portuguese'], ['zho', 'Chinese'], ['kor', 'Korean'], ['rus', 'Russian'], ['und', 'Unspecified']], 'Use an override for external files without language metadata.')
  ];
  return [];
}

function videoAudioFields(item, options) {
  return [
    selectField('output', 'Save as', [['mp4', 'Video with audio (MP4)'], ...[['mp3','Audio only — MP3'], ['aac','Audio only — AAC'], ['wav','Audio only — WAV']].map(([v,l]) => [v,l,!item.audio_tracks?.length])], 'Audio only discards the picture and subtitles. Your original file is not changed.'),
    selectField('audio_track', 'Soundtrack', [...(options.output === 'mp4' ? [['-1', 'No sound']] : []), ...(item.audio_tracks || []).map(t => [String(t.index), t.label])], item.audio_tracks?.length ? '' : 'This file has no soundtrack to extract.')
  ];
}

function drawOptions() {
  const item = currentItem();
  item.options ||= {...item.defaults};
  const options = item.options;
  const tabs = tabsFor(item);
  if (!activeTab || !tabs.some(([key]) => key === activeTab)) activeTab = item.kind === 'music' || item.kind === 'photo' ? options.output : tabs[0]?.[0];
  const audioOnly = item.kind === 'video' && options.output !== 'mp4';
  if (audioOnly && activeTab !== 'audio') activeTab = 'audio';
  byId('source-description').textContent = `${item.format} · ${prettySize(item.size)}${item.width ? ` · ${item.width} × ${item.height}` : ''}${item.fps ? ` · ${item.fps.toFixed(2)} fps` : ''}`;
  byId('eligibility-detail').textContent = item.eligible ? 'Eligible — passes the Vita format checks.' : (item.reasons || []).join('. ');
  byId('eligibility-detail').className = item.eligible ? 'eligible-info' : 'ineligible-info';
  byId('format-notes').textContent = (item.notes || []).join(' · ');
  byId('options-tabs').replaceChildren();
  for (const [key, label] of tabs) {
    const button = document.createElement('button');
    button.type = 'button';
    button.setAttribute('role', 'tab');
    button.setAttribute('aria-selected', String(activeTab === key));
    button.textContent = label;
    button.disabled = audioOnly && key !== 'audio';
    button.addEventListener('click', () => {
      activeTab = key;
      if (item.kind !== 'video') {
        options.output = key;
        options.keep_compatible = false;
        if (key === 'mp3' && ![32, 64, 96, 128, 160, 192, 224, 256, 320].includes(Number(options.bitrate))) options.bitrate = 192;
      }
      drawOptions();
    });
    byId('options-tabs').append(button);
  }
  byId('options-fields').replaceChildren();
  for (const field of fieldsFor(item, options)) {
    const row = document.createElement('div');
    row.className = 'option-row';
    const label = document.createElement('label');
    label.textContent = field.label;
    label.htmlFor = `option-${field.key}`;
    const input = document.createElement(field.choices ? 'select' : 'input');
    input.id = label.htmlFor;
    if (field.choices) for (const [value, title, disabled] of field.choices) { const choice = new Option(title, value); choice.disabled = Boolean(disabled); input.add(choice); }
    else { input.type = 'range'; input.min = field.min; input.max = field.max; input.step = field.step; }
    input.value = options[field.key];
    const output = document.createElement('output');
    output.textContent = input.value;
    input.addEventListener('input', () => {
      options[field.key] = field.choices ? input.value : Number(input.value);
      options.keep_compatible = false;
      byId('keep-compatible').checked = false;
      output.textContent = input.value;
    });
    input.addEventListener('change', () => {
      if (field.key === 'output') {
        if (options.output !== 'mp4' && Number(options.audio_track) < 0) options.audio_track = item.audio_tracks?.[0]?.index ?? -1;
        if (options.output === 'mp3' && ![32,64,96,128,160,192,224,256,320].includes(Number(options.extract_bitrate))) options.extract_bitrate = 192;
      }
      if (['mode', 'rate_mode', 'output'].includes(field.key)) drawOptions();
    });
    row.append(label, input);
    if (!field.choices) row.append(output);
    if (field.help) { const help = document.createElement('small'); help.textContent = field.help; row.append(help); }
    byId('options-fields').append(row);
  }
  if (activeTab === 'subtitles') {
    const browse = document.createElement('button');
    browse.textContent = 'Choose external subtitle file…';
    browse.style.marginBottom = '12px';
    browse.addEventListener('click', async () => {
      try {
        const track = await pywebview.api.choose_subtitle();
        if (track) {
          item.subtitle_tracks ||= [];
          if (!item.subtitle_tracks.some(t => t.id === track.id)) item.subtitle_tracks.push(track);
          options.subtitle_track = track.id;
          drawOptions();
        }
      } catch (error) { byId('options-result').textContent = String(error); }
    });
    byId('options-fields').append(browse);
  }
  byId('keep-compatible').checked = Boolean(options.keep_compatible && item.eligible);
  byId('keep-compatible').disabled = !item.eligible || audioOnly;
  byId('apply-conversion').disabled = !item.defaults;
}

byId('options-file').addEventListener('change', event => { optionsKey = event.target.value; activeTab = ''; byId('options-result').textContent = ''; byId('conversion-progress').hidden = true; drawOptions(); });
byId('keep-compatible').addEventListener('change', event => { currentItem().options.keep_compatible = event.target.checked; });
byId('reset-options').addEventListener('click', () => { currentItem().options = {...currentItem().defaults}; activeTab = ''; drawOptions(); });
byId('close-options').addEventListener('click', closeOptions);
byId('stop-conversion').addEventListener('click', async () => {
  conversionStopRequested = true;
  byId('stop-conversion').disabled = true;
  byId('options-result').textContent = 'Stopping conversion…';
  try { await pywebview.api.stop_conversion(); }
  catch (error) { conversionStopRequested = false; byId('options-result').textContent = String(error); byId('stop-conversion').disabled = false; }
});
dialog.addEventListener('cancel', event => { event.preventDefault(); closeOptions(); });
byId('apply-conversion').addEventListener('click', async () => {
  if (converting || dialogClosing) return;
  const key = optionsKey;
  const item = currentItem();
  converting = true;
  conversionStopRequested = false;
  item.status = 'Converting…';
  render();
  for (const control of dialog.querySelectorAll('button, input, select')) control.disabled = true;
  byId('options-result').textContent = 'Converting and checking the output…';
  updateConversionProgress({phase: 'Inspecting source', percent: null});
  try {
    const [result] = await pywebview.api.convert_files([item.path], item.options);
    if (!result.ok) throw Object.assign(new Error(result.error), {cancelled: result.cancelled});
    updateConversionProgress({phase: 'Complete', percent: 100, seconds: 0, duration: 0});
    byId('conversion-value').textContent = '100%';
    fileMap.delete(key);
    fileMap.set(result.item.path, {...result.item, video_folder: result.item.kind === 'video' ? item.video_folder : '', selected: item.selected, status: 'Prepared', result: 'done'});
    optionsKey = result.item.path;
    byId('options-file').selectedOptions[0].value = result.item.path;
    byId('options-file').selectedOptions[0].textContent = result.item.name;
    byId('options-result').textContent = 'Prepared and verified. The list now uses this converted file.' + (result.item.sidecars?.length ? ' M4T subtitles are attached.' : '');
    setStatus(`Prepared: ${result.item.name}`);
    activeTab = '';
  } catch (error) {
    byId('conversion-phase').textContent = error.cancelled ? 'Stopped' : 'Conversion failed';
    byId('conversion-bar').classList.remove('indeterminate');
    item.status = error.cancelled ? 'Stopped' : 'Conversion failed';
    item.detail = String(error);
    item.result = error.cancelled ? '' : 'error';
    byId('options-result').textContent = error.cancelled ? 'Conversion stopped. Original file kept; partial output removed.' : String(error);
  } finally {
    converting = false;
    for (const control of dialog.querySelectorAll('button, input, select')) control.disabled = false;
    byId('stop-conversion').disabled = true;
    render();
    drawOptions();
  }
});
