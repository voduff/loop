'use strict';
const { app, BrowserWindow, ipcMain, dialog, protocol, net, nativeTheme, Menu } = require('electron');
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const { spawn, execFile } = require('node:child_process');
const { promisify } = require('node:util');
const readline = require('node:readline');
const { pathToFileURL, fileURLToPath } = require('node:url');
const execFileAsync = promisify(execFile);
const dataDir = path.join(process.env.XDG_DATA_HOME || path.join(os.homedir(), '.local/share'), 'loop');
const configDir = path.join(process.env.XDG_CONFIG_HOME || path.join(os.homedir(), '.config'), 'loop');
const configFile = path.join(configDir, 'settings.json');
const audioBinary = fs.existsSync(path.join(__dirname, '../loop-audio')) ? path.join(__dirname, '../loop-audio') : path.join(__dirname, '../build/loop-audio');
const entryURL = 'loop://app/index.html';
protocol.registerSchemesAsPrivileged([{ scheme: 'loop', privileges: { standard: true, secure: true, supportFetchAPI: true } }]);
app.setName('loop');
app.setPath('userData', path.join(configDir, 'electron'));
// A small static interface does not need accelerated compositing or GPU rasterisation.
app.disableHardwareAcceleration();
app.commandLine.appendSwitch('disk-cache-size', '1048576');
app.commandLine.appendSwitch('media-cache-size', '1048576');
app.commandLine.appendSwitch('js-flags', '--max-old-space-size=96');
let win, quitting = false, model, downloadJob = null, sequence = Promise.resolve();
let downloadView = { busy: false, message: 'Downloads an MP3 and adds it to your library.', fraction: 0 };
let audioState = { ready: false, playing: false, position: 0, duration: 0 };
const clamp = value => Number.isFinite(Number(value)) ? Math.max(0, Math.min(1, Number(value))) : .65;
const strings = value => Array.isArray(value) ? [...new Set(value.filter(p => typeof p === 'string' && p.length && !p.includes('\0')))] : [];
function send(type, value) { if (win && !win.isDestroyed() && !win.webContents.isDestroyed()) win.webContents.send('loop:event', { type, value }); }
function snapshot() { return { ...model, audio: audioState, download: downloadView }; }
function update() { send('state', snapshot()); }
function notice(message) { send('notice', String(message)); }
function save() {
  fs.mkdirSync(configDir, { recursive: true, mode: 0o700 });
  const temp = configFile + '.tmp';
  fs.writeFileSync(temp, JSON.stringify(model, null, 2), { mode: 0o600 });
  fs.renameSync(temp, configFile);
}
function clean(raw) {
  const paths = strings(raw.paths);
  const queue = strings(raw.queue).filter(p => paths.includes(p));
  return { version: 2, paths, queue, selected: paths.includes(raw.selected) ? raw.selected : paths[0] || '',
    volume: clamp(raw.volume ?? .65), multiple: !!raw.multiple, preset: typeof raw.preset === 'string' ? raw.preset : '',
    presets: Array.isArray(raw.presets) ? raw.presets.filter(p => typeof p.name === 'string').map(p => ({
      name: p.name.slice(0, 80), tracks: strings(p.tracks).filter(t => paths.includes(t)), selected: typeof p.selected === 'string' ? p.selected : '',
      multiple: !!p.multiple, volume: clamp(p.volume)
    })) : [] };
}
async function loadSettings() {
  if (fs.existsSync(configFile)) return clean(JSON.parse(fs.readFileSync(configFile, 'utf8')));
  const legacyPath = path.join(configDir, 'settings.ini');
  let raw;
  if (fs.existsSync(legacyPath)) {
    const { stdout } = await execFileAsync(audioBinary, ['--export-settings', legacyPath], { maxBuffer: 8*1024*1024 });
    const legacy = JSON.parse(stdout), base = legacy.loop || {};
    raw = { ...base, selected: base.paths?.[Number(base.selected)] || '', multiple: base.multiple === 'true',
      presets: Object.entries(legacy).filter(([name]) => name.startsWith('preset-')).map(([, p]) => ({ ...p, multiple: p.multiple === 'true' })) };
  } else {
    const defaultPath = path.join(dataDir, 'what it feels like to be a memory (playlist).mp3');
    raw = { paths: fs.existsSync(defaultPath) ? [defaultPath] : [], queue: fs.existsSync(defaultPath) ? [defaultPath] : [], volume: .65 };
  }
  return clean(raw);
}
class NativeAudio {
  child = null; pending = new Map(); id = 0; generation = 0;
  start() {
    if (this.child) return;
    const child = this.child = spawn(audioBinary, [], { stdio: ['pipe', 'pipe', 'pipe'] });
    child.stdin.on('error', () => {});
    let diagnostics = '';
    child.stderr.on('data', chunk => { diagnostics = (diagnostics+chunk).slice(-2000); });
    const lines = readline.createInterface({ input: child.stdout });
    lines.on('line', line => {
      let result; try { result = JSON.parse(line); } catch { return; }
      this.generation = result.generation;
      audioState = { ready: result.ready, playing: result.playing, position: result.position, duration: result.duration };
      if (win?.isVisible() && !win.isMinimized()) send('audio', audioState);
      if (result.id && this.pending.has(result.id)) {
        const pending = this.pending.get(result.id); this.pending.delete(result.id);
        result.ok ? pending.resolve(result) : pending.reject(new Error(result.error));
      }
      if (result.event === 'ended') {
        const finished = model.selected, generation = result.generation;
        enqueue(async () => { if (model.multiple && model.selected === finished && this.generation === generation && !audioState.playing) await next(true); });
      }
    });
    const failed = message => {
      if (this.child !== child) return;
      this.child = null; lines.close();
      for (const request of this.pending.values()) request.reject(new Error(message));
      this.pending.clear(); audioState = { ready: false, playing: false, position: 0, duration: 0 };
      if (!quitting) { notice(message); update(); }
    };
    child.on('error', e => failed('Audio engine: '+e.message));
    child.on('exit', code => failed('Audio engine stopped'+(code ? ': '+(diagnostics || code) : '.')));
  }
  command(command, value = '') {
    this.start();
    return new Promise((resolve, reject) => {
      const id = ++this.id; this.pending.set(id, { resolve, reject });
      this.child.stdin.write(`${id}\t${command}\t${value}\n`, error => {
        if (error && this.pending.has(id)) { this.pending.delete(id); reject(error); }
      });
    });
  }
  stop() { if (this.child) this.child.stdin.end('0\tquit\t\n'); }
}
const audio = new NativeAudio();
function enqueue(operation) {
  const result = sequence.then(operation);
  sequence = result.catch(error => { if (!quitting) notice(error.message); });
  return result;
}
function b64(value) { return Buffer.from(value).toString('base64'); }
async function policy() {
  if (audioState.ready) await audio.command('loop', !model.multiple || (model.queue.length === 1 && model.queue[0] === model.selected) ? '1' : '0');
}
async function loadTrack(track, resume) {
  await audio.command('clear');
  await audio.command('load', b64(track));
  model.selected = track;
  await policy(); await audio.command('volume', model.volume);
  if (resume) await audio.command('play');
}
async function playQueue(start, resume) {
  if (audio.child) await audio.command('clear');
  let skipped = 0;
  for (let i=0; i<model.queue.length; i++) {
    const track = model.queue[(start+i)%model.queue.length];
    // Skip file/decoder failures, but stop on an unavailable output device.
    try { await loadTrack(track, resume); save(); update(); if (skipped) notice(`Skipped ${skipped} unavailable track(s).`); return; }
    catch (error) { if (/audio output|start playback/i.test(error.message)) throw error; skipped++; }
  }
  save(); update(); notice(model.queue.length ? 'No playable tracks in this loop. Check the file paths.' : 'Check some tracks to build your loop.');
}
async function next(resume) { await playQueue(Math.max(0, model.queue.indexOf(model.selected)+1), resume); }
async function addPaths(input, choose = true) {
  const added = [], errors = [];
  for (let track of input) {
    try {
      if (typeof track !== 'string' || track.includes('\0')) throw new Error('Invalid file path.');
      track = track.trim().replace(/^(['"])(.*)\1$/, '$2');
      if (track.startsWith('file://')) track = fileURLToPath(track);
      if (track.startsWith('~/')) track = path.join(os.homedir(), track.slice(2));
      track = path.resolve(track);
      if (!(await fs.promises.stat(track)).isFile()) throw new Error('Not a file.');
      await audio.command('validate', b64(track));
      if (!model.paths.includes(track)) model.paths.push(track);
      added.push(track);
    } catch (error) { errors.push(`${path.basename(String(track))}: ${error.message}`); }
  }
  if (added.length && (choose || !model.selected)) {
    const track = added.at(-1), resume = audioState.playing;
    if (track !== model.selected) {
      if (audio.child) await audio.command('clear');
      model.selected = track; model.preset = '';
      if (model.multiple && !model.queue.includes(track)) model.queue.push(track);
      if (resume) await loadTrack(track, true);
    }
  }
  save(); update(); if (errors.length) notice(errors.join('\n'));
  return added;
}
function youtubeURL(input) {
  const url = new URL(input.includes('://') ? input : 'https://'+input);
  if (!['https:', 'http:'].includes(url.protocol)) throw new Error('Paste a YouTube video link.');
  const host = url.hostname.toLowerCase(); let id = '';
  if (['youtu.be', 'www.youtu.be'].includes(host)) id = url.pathname.split('/')[1];
  else if (host === 'youtube.com' || host.endsWith('.youtube.com')) {
    id = url.pathname === '/watch' ? url.searchParams.get('v') : /^\/(shorts|live|embed)\//.test(url.pathname) ? url.pathname.split('/')[2] : '';
  }
  if (!/^[\w-]{11}$/.test(id || '')) throw new Error('Paste a YouTube video or Shorts link.');
  return 'https://www.youtube.com/watch?v='+id;
}
function downloadStatus(message, fraction) {
  downloadView = { busy: !!downloadJob, message, fraction }; send('download', downloadView);
}
function cancelDownload() {
  const job = downloadJob; if (!job || job.cancelled) return;
  job.cancelled = true;
  try { process.kill(-job.child.pid, 'SIGTERM'); } catch {}
  job.timer = setTimeout(() => { try { process.kill(-job.child.pid, 'SIGKILL'); } catch {} }, 2000);
  downloadStatus('Cancelling…', downloadView.fraction);
}
function download(input) {
  if (downloadJob) throw new Error('A download is already running.');
  const url = youtubeURL(input.trim()), destination = path.join(dataDir, 'downloads');
  fs.mkdirSync(destination, { recursive: true });
  const args = ['--ignore-config', '--no-plugin-dirs', '--no-playlist', '--no-simulate', '--no-quiet', '--newline', '--no-colors', '--progress', '--progress-delta', '0.5',
    '--socket-timeout', '20', '--retries', '3', '--fragment-retries', '3', '--match-filters', '!is_live', '--break-on-reject', '--windows-filenames', '--no-overwrites', '--no-mtime',
    '-f', 'bestaudio/best', '-x', '--audio-format', 'mp3', '--audio-quality', '0', '--paths', destination, '-o', '%(title).150B [%(id)s].%(ext)s',
    '--progress-template', 'download:LOOP_PROGRESS|%(progress.downloaded_bytes)s|%(progress.total_bytes)s|%(progress.total_bytes_estimate)s|%(progress.speed)s|%(progress.eta)s',
    '--print', 'post_process:LOOP_CONVERT', '--print', 'after_move:LOOP_FILE:%(filepath)s'];
  // Node is optional; yt-dlp can also use an installed Deno runtime.
  for (const dir of (process.env.PATH || '').split(path.delimiter)) {
    const candidate = path.join(dir, 'node'); if (fs.existsSync(candidate)) { args.push('--js-runtimes', 'node:'+candidate); break; }
  }
  args.push('--', url);
  const child = spawn(path.join(dataDir, 'downloader/bin/yt-dlp'), args, { detached: true, stdio: ['ignore', 'pipe', 'pipe'], env: { ...process.env, PYTHONUNBUFFERED: '1' } });
  const job = downloadJob = { child, cancelled: false, file: '', error: '', timer: null };
  downloadStatus('Connecting to YouTube…', null);
  const consume = line => {
    if (line.startsWith('LOOP_FILE:')) { job.file = line.slice(10); return; }
    if (job.cancelled) return;
    if (line.startsWith('LOOP_PROGRESS|')) {
      const [, bytes, total, estimate, speed, eta] = line.split('|');
      const size = Number(total) || Number(estimate); const fraction = size > 0 ? Math.min(1, Number(bytes)/size) : null;
      if (fraction === 1) { downloadStatus('Download complete · Preparing MP3…', 0.9); return; }
      const mb = n => (Number(n)/1048576).toFixed(1)+' MB';
      const parts = ['Downloading', fraction === null ? mb(bytes) : Math.floor(fraction*100)+'%', mb(bytes)];
      if (Number(speed)>0) parts.push(mb(speed)+'/s'); if (Number(eta)>0) parts.push(Math.ceil(Number(eta))+'s left');
      downloadStatus(parts.join(' · '), fraction === null ? null : fraction*.9);
    } else if (line === 'LOOP_CONVERT' || line.startsWith('[ExtractAudio]')) downloadStatus('Converting to MP3…', .95);
    else if (line.startsWith('ERROR:')) job.error = line.slice(6).trim();
  };
  const readers = [child.stdout, child.stderr].map(input => { const r = readline.createInterface({ input }); r.on('line', consume); return r; });
  child.on('error', error => { job.error = error.message; });
  child.on('close', code => {
    clearTimeout(job.timer); readers.forEach(r => r.close()); if (downloadJob !== job) return;
    downloadJob = null;
    if (quitting) return;
    if (job.cancelled) { downloadStatus('Download cancelled. Paste a link to try again.', 0); return; }
    if (code !== 0 || !job.file) { downloadStatus(job.error || 'Download failed. Try again or use another link.', 0); return; }
    enqueue(async () => {
      const added = await addPaths([job.file], false);
      downloadStatus(added.length ? 'Added to your library · '+path.basename(job.file, '.mp3') : 'Downloaded, but could not add the MP3.', added.length ? 1 : 0);
    }).catch(error => downloadStatus(error.message, 0));
  });
}
async function action(type, value) {
  switch (type) {
    case 'toggle':
      if (audioState.playing) await audio.command('pause');
      else if (model.multiple && (!audioState.ready || !model.queue.includes(model.selected))) { await playQueue(Math.max(0, model.queue.indexOf(model.selected)), true); return; }
      else if (model.selected) { if (!audioState.ready) await loadTrack(model.selected, false); await policy(); await audio.command('play'); }
      break;
    case 'select':
      if (!model.paths.includes(value)) throw new Error('Track is not in the library.');
      if (value !== model.selected) {
        const resume = audioState.playing;
        if (audio.child) await audio.command('clear');
        model.selected = value; model.preset = '';
        if (model.multiple && !model.queue.includes(value)) model.queue.push(value);
        if (resume) await loadTrack(value, true);
      }
      break;
    case 'next': if (model.multiple) await next(audioState.playing); break;
    case 'restart': if (audioState.ready) await audio.command('seek', '0'); break;
    case 'seek': if (audioState.ready && Number.isFinite(value)) await audio.command('seek', Math.max(0, Math.min(audioState.duration, value))); break;
    case 'volume': model.volume = clamp(value); model.preset = ''; if (audio.child) await audio.command('volume', model.volume); break;
    case 'mode':
      model.multiple = value === 'multiple'; model.preset = '';
      if (model.multiple && !model.queue.length && model.selected) model.queue.push(model.selected);
      if (model.multiple && audioState.playing && !model.queue.includes(model.selected)) await playQueue(0, true); else await policy(); break;
    case 'check':
      if (!value || !model.paths.includes(value.path)) throw new Error('Unknown track.');
      if (value.checked && !model.queue.includes(value.path)) model.queue.push(value.path);
      if (!value.checked) model.queue = model.queue.filter(p => p !== value.path);
      model.preset = ''; if (model.multiple && !model.queue.length && audio.child) await audio.command('pause'); await policy(); break;
    case 'remove': {
      const removed = model.selected; if (!removed) return;
      if (audio.child) await audio.command('clear');
      model.paths = model.paths.filter(p => p !== removed); model.queue = model.queue.filter(p => p !== removed);
      model.presets.forEach(p => { p.tracks = p.tracks.filter(t => t !== removed); if (p.selected === removed) p.selected = ''; });
      model.selected = model.paths[0] || ''; model.preset = ''; break;
    }
    case 'browse': {
      const result = await dialog.showOpenDialog(win, { title: 'Add MP3 files', properties: ['openFile', 'multiSelections'], filters: [{ name: 'MP3 audio', extensions: ['mp3'] }] });
      if (!result.canceled) await addPaths(result.filePaths); return;
    }
    case 'add-path': if (typeof value !== 'string') throw new Error('Enter a file path.'); await addPaths([value.trim()]); return;
    case 'preset-save': {
      const name = typeof value === 'string' ? value.trim().slice(0,80) : '';
      if (!name) throw new Error('Enter a preset name.');
      if (!model.selected || (model.multiple && !model.queue.length)) throw new Error('Choose tracks before saving a preset.');
      const preset = { name, tracks: [...model.queue], selected: model.selected, volume: model.volume, multiple: model.multiple };
      const existing = model.presets.findIndex(p => p.name === name);
      if (existing < 0) model.presets.push(preset); else model.presets[existing] = preset;
      model.preset = name; notice('Preset saved · '+name); break;
    }
    case 'preset-load': {
      if (!value) { model.preset = ''; break; }
      const preset = model.presets.find(p => p.name === value); if (!preset) throw new Error('Preset not found.');
      const resume = audioState.playing; if (audio.child) await audio.command('clear');
      model.queue = preset.tracks.filter(p => model.paths.includes(p)); model.volume = preset.volume;
      model.multiple = preset.multiple; model.preset = preset.name; model.selected = preset.selected;
      if (model.multiple) await playQueue(0, resume);
      else if (model.paths.includes(model.selected)) { if (resume) await loadTrack(model.selected, true); }
      else { model.selected = ''; notice('The preset track is no longer in your library.'); }
      break;
    }
    case 'preset-delete': model.presets = model.presets.filter(p => p.name !== model.preset); model.preset = ''; notice('Preset removed. Your audio files are unchanged.'); break;
    case 'download': if (typeof value !== 'string') throw new Error('Paste a YouTube link.'); download(value); return;
    case 'download-cancel': cancelDownload(); return;
    default: throw new Error('Unknown action.');
  }
  save(); update();
}
function trusted(event) { return win && event.sender === win.webContents && event.senderFrame === win.webContents.mainFrame && event.senderFrame.url === entryURL; }
function openWindow() {
  win = new BrowserWindow({ width: 740, height: 900, minWidth: 580, minHeight: 600, title: 'loop', backgroundColor: '#ffffff', show: false,
    autoHideMenuBar: true, icon: path.join(__dirname, '../loop.png'),
    webPreferences: { preload: path.join(__dirname, 'preload.cjs'), sandbox: true, contextIsolation: true, nodeIntegration: false,
      spellcheck: false, backgroundThrottling: true, webviewTag: false, devTools: false } });
  win.webContents.setWindowOpenHandler(() => ({ action: 'deny' }));
  win.webContents.on('will-navigate', event => event.preventDefault());
  win.webContents.session.setPermissionRequestHandler((_w, _p, callback) => callback(false));
  win.webContents.session.setPermissionCheckHandler(() => false);
  win.once('ready-to-show', () => win.show());
  win.on('restore', update); win.on('show', () => { if (model) update(); });
  win.on('closed', () => { win = null; app.quit(); });
  win.loadURL(entryURL);
}
function fileArguments(argv) { return argv.filter(p => !p.startsWith('-') && /\.mp3$/i.test(p)); }
if (!app.requestSingleInstanceLock()) app.quit();
else {
  app.on('second-instance', (_event, argv) => {
    if (win) { if (win.isMinimized()) win.restore(); win.show(); win.focus(); }
    const files = fileArguments(argv); if (files.length && model) enqueue(() => addPaths(files));
  });
  app.whenReady().then(async () => {
    nativeTheme.themeSource = 'light'; Menu.setApplicationMenu(null);
    const allowed = new Set(['index.html', 'style.css', 'renderer.js']);
    protocol.handle('loop', request => {
      const url = new URL(request.url), file = url.pathname.slice(1);
      if (url.host !== 'app' || !allowed.has(file)) return new Response('Not found', { status: 404 });
      return net.fetch(pathToFileURL(path.join(__dirname, file)).href);
    });
    model = await loadSettings(); save();
    ipcMain.handle('loop:initial', event => { if (!trusted(event)) throw new Error('Invalid sender.'); return snapshot(); });
    ipcMain.handle('loop:action', (event, type, value) => {
      if (!trusted(event)) throw new Error('Invalid sender.');
      return enqueue(() => action(type, value)).then(() => ({ ok: true }), error => ({ ok: false, error: error.message }));
    });
    openWindow();
    const files = fileArguments(process.argv.slice(1)); if (files.length) enqueue(() => addPaths(files));
  }).catch(error => { dialog.showErrorBox('loop could not start', error.message+'\nYour existing settings have not been deleted.'); app.quit(); });
}
app.on('before-quit', () => { quitting = true; cancelDownload(); audio.stop(); });
app.on('will-quit', () => {
  if (downloadJob) { try { process.kill(-downloadJob.child.pid, 'SIGKILL'); } catch {} }
  if (audio.child) audio.child.kill('SIGTERM');
});
