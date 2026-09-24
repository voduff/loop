# loop

A small native C++ MP3 player for Linux. White surfaces, black controls, thin borders, and rounded corners. Uses GTK 3 and miniaudio 0.11.23; no browser runtime.

Open **loop** from the desktop application launcher, then press **Play**. Repeat one track, or check multiple tracks and choose **Loop checked tracks** to repeat the sequence. Minimize the window to continue listening.

- **Add MP3** opens a file picker with multiple selection.
- **Paste path** accepts a local file path, `~/Music/...`, or a `file://` URL.
- **Add from YouTube** accepts a video, short, or recorded live video link. Paste it and press **Download** (or Enter). Transfer progress shows percentage, size, speed, and ETA when available, followed by an MP3 conversion status. The finished MP3 is automatically saved in the library without interrupting your current track.
- **Cancel** stops both the download and any conversion. Closing the app also cancels active downloads. Partial transfers can resume when you retry the same link. Downloads use one video per link; ongoing live streams are excluded.
- Select a saved row to switch tracks. Switching while playing continues playback with the new track.
- **Search** filters titles and file paths as you type, including Unicode text. Filtering never removes tracks from your loop or interrupts playback. **Ctrl+F** focuses search.
- **Loop checked tracks** plays checked tracks in the order you checked them, shown as `#1`, `#2`, etc. After the last track, playback returns to the first. Uncheck and recheck a track to move it to the end. Clicking an unchecked track in this mode adds it to the sequence and selects it. **Next** advances through the sequence; **Loop one track** repeats only the current track.
- **Save preset…** saves the checked track order, loop mode, current track, and volume under a name such as “Deep focus.” Select a preset from the dropdown to restore it. Sequence presets start at their first playable track; loading while paused stays paused, while loading during playback continues with the preset. Save using an existing name to update it. The trash button removes only the preset.
- Automatic sequence playback skips unavailable files and pauses with a message if none can play. Track changes may have a brief pause; this is not gapless mixing.
- The minus button removes a saved option; it never deletes the audio file.
- Use the position slider to seek, the back button to restart, and the volume slider to adjust loudness.
- **Space** toggles playback. **Ctrl+O** opens the file picker.
- Paths, selection, volume, loop mode, checked order, and presets are saved. Startup is paused. Your existing library is preserved when upgrading.
- **Add from YouTube** can be expanded when needed to leave more room for your library.

Audio files and personal settings are not included in this repository. The original installation has “what it feels like to be a memory (playlist)” as its initial option. The installer copies that MP3 only if it exists next to the project folder. On a fresh installation, add your own MP3 or YouTube link; you can remove the original placeholder from the list if the file is absent.

## Resource use

Audio is streamed using bounded decode buffers instead of loading the whole track into RAM. Only one track is decoded at a time, even in a multi-track loop. The audio device stops while paused. Track completion is checked four times per second during playback, while the progress display updates once per second. No playback timer runs while paused. YouTube downloading runs yt-dlp and FFmpeg in separate processes only while needed; it uses additional CPU and RAM during downloading and conversion. The GUI reads progress asynchronously. Ordinary local playback makes no network requests. GTK and the system audio backend determine the remaining baseline memory use.

## Build and install

Requires CMake, a C/C++ compiler, pkg-config, GTK 3 development headers, Python 3 with venv support, and FFmpeg. Node.js is used for YouTube's JavaScript challenges when available. miniaudio is vendored, including its license at the end of `vendor/miniaudio.h`. The installer downloads yt-dlp 2026.08.19 and its default dependencies into an isolated app environment. Network access is needed for that installation and for YouTube downloads.

```sh
git clone https://github.com/voduff/loop.git
cd loop
bash install.sh
```

The installer builds Release mode and installs for the current user:

- Executable: `~/.local/bin/loop`
- Launcher: `${XDG_DATA_HOME:-~/.local/share}/applications/io.local.loop.desktop`
- Default MP3: `${XDG_DATA_HOME:-~/.local/share}/loop/`
- YouTube MP3s: `${XDG_DATA_HOME:-~/.local/share}/loop/downloads/`
- YouTube helper: `${XDG_DATA_HOME:-~/.local/share}/loop/downloader/`
- Settings: `${XDG_CONFIG_HOME:-~/.config}/loop/settings.ini`

You can also open files with `loop /path/to/file.mp3`. A second invocation adds the file to the existing app window.

If YouTube changes and downloads begin failing, the helper can be updated independently:

```sh
"${XDG_DATA_HOME:-$HOME/.local/share}/loop/downloader/bin/python" -m pip install --upgrade 'yt-dlp[default]'
```

The app displays download errors inline; hover over the message for full details. Videos requiring sign-in or otherwise unavailable may fail to download.
