# loop

An Electron desktop MP3 player with a small C++ audio engine. White surfaces, black controls, rounded cards, and a searchable library. Built for Linux.

## Features

- Repeat one track indefinitely, or check multiple tracks and choose **Loop checked tracks**. Tracks play in checked order, then the sequence starts again. Uncheck and recheck a track to move it to the end.
- Search filenames and paths without changing playback. **Ctrl+F** focuses search.
- Save named **presets** containing track order, loop mode, current track, and volume. Load, replace, or delete them from the preset controls.
- Add MP3s with the file picker (**Ctrl+O**) or paste local paths, `~/Music/...` paths, or `file://` URLs.
- Download a single YouTube video, Short, or recorded live video as MP3. View transfer percentage, speed, ETA, conversion status, and cancel an active download. Finished files appear in the library without interrupting playback.
- Play/pause, seek, restart, advance to the next checked track, and adjust volume. Space toggles playback when you are not using an input or button.
- Minimize to keep listening. Closing the app stops playback and cancels any download.
- Removing a track or preset never deletes an audio file. Sequence playback skips unavailable files and stops with a message if none can play. Transitions may have a short pause; this is not a gapless mixer.

## Memory and CPU

Electron uses more baseline memory than the previous GTK version. This version limits avoidable overhead rather than promising GTK-level RAM usage:

- One window with plain HTML, CSS, and JavaScript. No React, web server, webviews, remote fonts, artwork fetching, or animation loops.
- A separate native `loop-audio` helper streams and decodes only the current track using miniaudio. MP3 files are never loaded into renderer memory, and the whole queue is not predecoded.
- The helper starts on demand. Its input loop blocks while paused; the audio device also stops. During playback, it checks completion five times per second and reports progress once per second.
- The renderer receives no playback progress updates while minimized. Electron background throttling stays enabled. Search/list rendering only runs on relevant user or library changes.
- Hardware acceleration is disabled for the static interface. Renderer V8 heap growth and disk/media caches are capped; those caps are not limits on total process RAM.
- yt-dlp and FFmpeg run only during downloading/conversion. Those operations temporarily use additional resources.

Runtime RAM/CPU use has not been benchmarked. The Electron migration has been compiled and packaged; playback and UI behavior have not yet been exercised.

## Install

Requires Linux, a C/C++ compiler, CMake, pkg-config, GLib development headers, Node.js/npm, Python 3 with venv support, and FFmpeg. GTK development headers are only needed for the optional legacy frontend. Node.js also supports YouTube JavaScript challenges.

On Ubuntu/Debian, the development packages are typically `build-essential cmake pkg-config libglib2.0-dev nodejs npm python3-venv ffmpeg`.

```sh
git clone https://github.com/voduff/loop.git
cd loop
bash install.sh
```

The installer builds the native helper, installs the locked Electron dependency, copies the Electron runtime and local app assets, prepares yt-dlp's isolated environment, and registers **loop** in your application launcher. Network access is required to obtain dependencies.

The app keeps Electron's process sandbox enabled. On Linux systems that restrict unprivileged user namespaces, the launcher can use an existing root-owned setuid Chromium sandbox helper. It checks common Chromium/Chrome locations and the system helper distributed with Claude Desktop. The installer does not disable the sandbox or change system-wide security settings. Systems without usable user namespaces or a system sandbox helper need an administrator to configure Chromium sandbox support.

For development:

```sh
npm ci
npm run build
npm start
```

The installed launcher includes any detected system sandbox helper configuration; `npm start` uses your development environment's sandbox configuration.

## Existing library migration

On first Electron launch, the native helper reads the original GLib `settings.ini` format. The app preserves paths (including Unicode and escaped characters), checked order, selected track, volume, loop mode, and saved presets in `settings.json`. **The original INI file is left untouched.** Later launches use JSON. Startup remains paused.

Audio files and personal settings are not included in this repository. If the original downloaded “what it feels like to be a memory (playlist)” MP3 exists next to the project folder, the installer can copy it as an initial option. Otherwise add your own files or YouTube links.

Installed locations (respecting `XDG_DATA_HOME` / `XDG_CONFIG_HOME`):

- Launcher: `~/.local/bin/loop`
- Desktop entry: `~/.local/share/applications/io.local.loop.desktop`
- App and native helper: `~/.local/share/loop/electron-app/`
- Electron runtime: `~/.local/share/loop/electron-<version>/`
- Downloaded MP3s: `~/.local/share/loop/downloads/`
- Downloader environment: `~/.local/share/loop/downloader/`
- Settings: `~/.config/loop/settings.json`
- Original settings backup: `~/.config/loop/settings.ini`

Open files using `loop /path/to/file.mp3`. Subsequent Electron invocations add files to the existing window.

## Source layout

- `electron/`: main process, sandboxed preload bridge, local HTML/CSS/JS interface.
- `src/backend.cpp`: audio command transport and original settings import.
- `src/player.hpp`, `src/audio.c`: streamed miniaudio playback.
- `vendor/miniaudio.h`: miniaudio 0.11.23, with its upstream license included.
- `src/main.cpp`, `src/downloader.hpp`: retained GTK frontend. Build it with `cmake -S . -B build -DLOOP_BUILD_LEGACY=ON` and `cmake --build build -j2`; the optional binary is `build/loop-gtk`.

The renderer has no Node access, context isolation and sandboxing stay enabled, IPC is checked against the app's main frame, and only packaged assets load through the local `loop://` protocol. Downloads run outside the renderer, using argument arrays rather than shell commands.

If YouTube changes and downloads fail, update the helper independently:

```sh
"${XDG_DATA_HOME:-$HOME/.local/share}/loop/downloader/bin/python" -m pip install --upgrade 'yt-dlp[default]'
```

Videos requiring sign-in or otherwise unavailable may fail. Download errors are displayed in the app.
