#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
data_dir="${XDG_DATA_HOME:-$HOME/.local/share}"
bin_dir="$HOME/.local/bin"
app_dir="$data_dir/loop/electron-app"
cmake -S "$project_dir" -B "$project_dir/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$project_dir/build" -j2
(cd "$project_dir" && npm ci --no-audit --no-fund)
command -v ffmpeg >/dev/null || { printf 'FFmpeg is required for YouTube downloads.\n' >&2; exit 1; }
mkdir -p "$app_dir/electron" "$data_dir/applications" "$bin_dir"
if [[ ! -x "$data_dir/loop/downloader/bin/python" ]]; then
    python3 -m venv "$data_dir/loop/downloader"
fi
"$data_dir/loop/downloader/bin/python" -m pip --disable-pip-version-check install --quiet 'yt-dlp[default]>=2026.08.19'
electron_version="$(node -p "require(process.argv[1]).version" "$project_dir/node_modules/electron/package.json")"
runtime_dir="$data_dir/loop/electron-$electron_version"
if [[ ! -x "$runtime_dir/electron" ]]; then
    runtime_temp="$(mktemp -d "$data_dir/loop/.electron-install.XXXXXX")"
    cp -a "$project_dir/node_modules/electron/dist/." "$runtime_temp/"
    mv -- "$runtime_temp" "$runtime_dir"
fi
install -m644 "$project_dir/package.json" "$app_dir/package.json"
install -m644 "$project_dir/electron/"* "$app_dir/electron/"
new_audio="$(mktemp "$app_dir/.audio-install.XXXXXX")"
install -m755 "$project_dir/build/loop-audio" "$new_audio"
mv -f -- "$new_audio" "$app_dir/loop-audio"
install -m644 "$project_dir/loop.png" "$app_dir/loop.png"
install -Dm644 "$project_dir/loop.svg" "$data_dir/icons/hicolor/scalable/apps/io.local.loop.svg"
install -Dm644 "$project_dir/loop.png" "$data_dir/icons/hicolor/128x128/apps/io.local.loop.png"
# Use a root-owned system sandbox helper when user namespaces are restricted.
# Never disable Chromium's sandbox or change global security settings.
sandbox_helper=""
for candidate in /usr/local/lib/loop/chrome-sandbox /usr/lib/chromium/chrome-sandbox /opt/google/chrome/chrome-sandbox /usr/lib/claude-desktop/chrome-sandbox; do
    if [[ -u "$candidate" && "$(stat -c %u "$candidate")" == 0 ]]; then sandbox_helper="$candidate"; break; fi
done
launcher="$(mktemp "$bin_dir/.loop-install.XXXXXX")"
trap 'rm -f -- "$launcher"' EXIT
{
    printf '#!/usr/bin/env bash\nset -e\n'
    if [[ -n "$sandbox_helper" ]]; then printf 'export CHROME_DEVEL_SANDBOX=%q\n' "$sandbox_helper"; fi
    printf 'exec %q %q "$@"\n' "$runtime_dir/electron" "$app_dir"
} > "$launcher"
chmod 755 "$launcher"
mv -f -- "$launcher" "$bin_dir/loop"
default_audio="$project_dir/../what it feels like to be a memory (playlist).mp3"
if [[ -f "$default_audio" && ! -e "$data_dir/loop/what it feels like to be a memory (playlist).mp3" ]]; then
    cp -- "$default_audio" "$data_dir/loop/what it feels like to be a memory (playlist).mp3"
fi
cat > "$data_dir/applications/io.local.loop.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=loop
Comment=Your sound. On repeat. Electron UI with streamed native audio.
Exec="$bin_dir/loop" %F
Icon=io.local.loop
Terminal=false
Categories=AudioVideo;Audio;Player;
MimeType=audio/mpeg;
StartupNotify=true
StartupWMClass=loop-desktop
Keywords=mp3;music;repeat;loop;audio;
DESKTOP
if command -v update-desktop-database >/dev/null; then update-desktop-database "$data_dir/applications"; fi
if command -v gtk-update-icon-cache >/dev/null; then gtk-update-icon-cache -f -t "$data_dir/icons/hicolor" >/dev/null 2>&1 || true; fi
printf 'Installed loop (Electron %s) to %s\n' "$electron_version" "$bin_dir/loop"
