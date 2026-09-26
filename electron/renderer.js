'use strict';
const $ = id => document.getElementById(id);
let state, dragging = false, listKey = '', presetKey = '', promptAction;
const title = path => path.split('/').at(-1).replace(/\.[^.]+$/, '');
const fold = text => text.normalize('NFKC').toLocaleLowerCase();
const time = seconds => {
  const n = Math.max(0, Math.floor(seconds || 0));
  return n >= 3600 ? `${Math.floor(n/3600)}:${String(Math.floor(n/60)%60).padStart(2,'0')}:${String(n%60).padStart(2,'0')}` : `${Math.floor(n/60)}:${String(n%60).padStart(2,'0')}`;
};
function message(text) { $('notice').textContent = text; $('notice-box').hidden = !text; }
async function act(type, value) {
  try { const result = await window.loop.action(type, value); if (!result.ok) message(result.error); return result.ok; }
  catch (error) { message(error.message); return false; }
}
function audioView(audio) {
  if (!state) return;
  state.audio = audio;
  $('status').textContent = audio.playing ? (state.multiple ? 'LOOPING SELECTED TRACKS' : 'PLAYING ON REPEAT') : 'READY WHEN YOU ARE';
  $('play-label').textContent = audio.playing ? 'Pause' : 'Play';
  $('play-icon').setAttribute('href', audio.playing ? '#i-pause' : '#i-play');
  $('play').disabled = !audio.playing && (state.multiple ? !state.queue.length : !state.selected);
  $('restart').disabled = !audio.ready; $('seek').disabled = !audio.ready || !audio.duration;
  $('next').disabled = !state.multiple || !state.queue.length;
  if (!dragging) { $('seek').max = Math.max(1, audio.duration); $('seek').value = audio.position; $('elapsed').textContent = time(audio.position); }
  $('duration').textContent = audio.duration ? time(audio.duration) : '—:——';
}
function downloadView(download) {
  $('download').disabled = download.busy; $('youtube-url').disabled = download.busy;
  $('cancel-download').hidden = !download.busy;
  $('download-status').textContent = download.message;
  const progress = $('download-progress'); progress.hidden = !download.busy && download.fraction === 0;
  // Static indicator when the server cannot provide a total; no animation loop.
  progress.value = download.fraction ?? 0;
  if (download.message.startsWith('Added to your library')) $('youtube-url').value = '';
}
function svg(name) {
  const element = document.createElementNS('http://www.w3.org/2000/svg','svg');
  const use = document.createElementNS('http://www.w3.org/2000/svg','use');
  use.setAttribute('href', '#i-'+name); element.append(use); element.setAttribute('aria-hidden','true'); return element;
}
function renderTracks() {
  const query = fold($('search').value.trim()), fragment = document.createDocumentFragment(); let visible = 0;
  for (const path of state.paths) {
    if (!fold(path).includes(query)) continue;
    visible++;
    const row = document.createElement('div'); row.className = 'track'+(state.selected === path ? ' current' : ''); row.setAttribute('role','listitem');
    const check = document.createElement('input'); check.type = 'checkbox'; check.checked = state.queue.includes(path);
    check.setAttribute('aria-label', 'Include '+title(path)+' in loop'); check.title = 'Include in loop; checked order';
    check.addEventListener('change', () => act('check', { path, checked: check.checked }));
    const icon = svg('music'); icon.classList.add('music');
    const button = document.createElement('button'); button.className = 'track-select'; button.title = path;
    const name = document.createElement('span'); name.className = 'track-name'; name.textContent = title(path);
    const location = document.createElement('span'); location.className = 'track-path'; location.textContent = path;
    button.append(name, location); button.addEventListener('click', () => act('select', path));
    const order = document.createElement('span'); order.className = 'order'; const index = state.queue.indexOf(path); order.textContent = index < 0 ? 'MP3' : '#'+(index+1);
    row.append(check, icon, button, order); fragment.append(row);
  }
  const scroll = $('tracks').scrollTop; $('tracks').replaceChildren(fragment); $('tracks').scrollTop = scroll;
  $('empty').hidden = !!visible; $('empty').textContent = state.paths.length ? 'No tracks match your search.' : 'Add your first MP3 above.';
  $('count').textContent = (query ? visible+' of ' : '')+state.paths.length+(state.paths.length === 1 ? ' saved track' : ' saved tracks');
}
function render(next) {
  state = next;
  $('track-title').textContent = state.selected ? title(state.selected) : 'Make yourself a loop';
  $('track-title').title = state.selected ? title(state.selected) : '';
  $('track-path').textContent = state.selected || 'Add an MP3 to get started.'; $('track-path').title = state.selected || '';
  if (document.activeElement !== $('volume')) $('volume').value = Math.round(state.volume*100);
  $('mode').value = state.multiple ? 'multiple' : 'one';
  $('queue-count').textContent = state.queue.length+' in loop · check tracks in play order';
  $('remove').disabled = !state.selected;
  const key = JSON.stringify([state.paths, state.queue, state.selected]);
  if (key !== listKey) { listKey = key; renderTracks(); }
  const pkey = JSON.stringify(state.presets.map(p => p.name));
  if (pkey !== presetKey) {
    presetKey = pkey; $('preset').replaceChildren(new Option('Custom selection',''));
    for (const preset of state.presets) $('preset').append(new Option(preset.name,preset.name));
  }
  $('preset').value = state.preset; $('delete-preset').disabled = !state.preset;
  audioView(state.audio); downloadView(state.download);
}
function prompt(type, heading, description, placeholder, value = '') {
  promptAction = type; $('prompt-title').textContent = heading; $('prompt-description').textContent = description;
  $('prompt-input').placeholder = placeholder; $('prompt-input').value = value;
  $('prompt-input').maxLength = type === 'preset-save' ? 80 : 8192;
  $('prompt-submit').textContent = type === 'preset-save' ? 'Save' : 'Add track';
  $('prompt').showModal(); $('prompt-input').focus(); $('prompt-input').select();
}
$('play').onclick = () => act('toggle'); $('restart').onclick = () => act('restart'); $('next').onclick = () => act('next');
$('browse').onclick = () => act('browse'); $('remove').onclick = () => act('remove');
$('mode').onchange = event => act('mode', event.target.value);
$('search').oninput = () => { if (state) renderTracks(); };
$('volume').onchange = event => act('volume', Number(event.target.value)/100);
$('seek').oninput = event => { dragging = true; $('elapsed').textContent = time(Number(event.target.value)); };
$('seek').onchange = async event => { await act('seek', Number(event.target.value)); dragging = false; };
$('seek').addEventListener('blur', () => { dragging = false; });
$('preset').onchange = event => { $('search').value = ''; listKey = ''; act('preset-load', event.target.value); };
$('delete-preset').onclick = () => act('preset-delete');
$('save-preset').onclick = () => prompt('preset-save','Save preset','Save track order, loop mode, and volume. An existing name updates that preset.','e.g. Deep focus',state?.preset || '');
$('paste').onclick = () => prompt('add-path','Add a file path','Paste the path to an MP3 on your computer.','~/Music/my track.mp3');
$('prompt-cancel').onclick = () => $('prompt').close();
$('prompt-form').onsubmit = async event => { event.preventDefault(); if (await act(promptAction, $('prompt-input').value)) $('prompt').close(); };
$('download-form').onsubmit = event => { event.preventDefault(); act('download', $('youtube-url').value); };
$('cancel-download').onclick = () => act('download-cancel'); $('dismiss').onclick = () => message('');
document.addEventListener('keydown', event => {
  if ($('prompt').open) return;
  if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'f') { event.preventDefault(); $('search').focus(); $('search').select(); }
  else if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'o') { event.preventDefault(); act('browse'); }
  else if (event.code === 'Space' && !event.repeat && !event.ctrlKey && !event.altKey && !event.metaKey && !['INPUT','SELECT','TEXTAREA','BUTTON','SUMMARY'].includes(document.activeElement.tagName)) { event.preventDefault(); act('toggle'); }
});
window.loop.subscribe(({type,value}) => {
  if (type === 'state') render(value); else if (type === 'audio') audioView(value);
  else if (type === 'download') { if (state) state.download = value; downloadView(value); }
  else if (type === 'notice') message(value);
});
window.loop.initial().then(render).catch(error => message(error.message));
