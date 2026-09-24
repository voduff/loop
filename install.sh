#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
data_dir="${XDG_DATA_HOME:-$HOME/.local/share}"
bin_dir="$HOME/.local/bin"
cmake -S "$project_dir" -B "$project_dir/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$project_dir/build" -j2
command -v ffmpeg >/dev/null || { printf 'FFmpeg is required for YouTube downloads.\n' >&2; exit 1; }
mkdir -p "$data_dir/loop" "$data_dir/applications" "$bin_dir"
if [[ ! -x "$data_dir/loop/downloader/bin/python" ]]; then
    python3 -m venv "$data_dir/loop/downloader"
fi
"$data_dir/loop/downloader/bin/python" -m pip --disable-pip-version-check install --quiet 'yt-dlp[default]==2026.08.19'
# Replace atomically, including when the previous version is still running.
new_binary="$(mktemp "$bin_dir/.loop-install.XXXXXX")"
trap 'rm -f -- "$new_binary"' EXIT
install -m755 "$project_dir/build/loop" "$new_binary"
mv -f -- "$new_binary" "$bin_dir/loop"
install -Dm644 "$project_dir/loop.svg" "$data_dir/icons/hicolor/scalable/apps/io.local.loop.svg"
default_audio="$project_dir/../what it feels like to be a memory (playlist).mp3"
if [[ -f "$default_audio" && ! -e "$data_dir/loop/what it feels like to be a memory (playlist).mp3" ]]; then
    cp -- "$default_audio" "$data_dir/loop/what it feels like to be a memory (playlist).mp3"
fi
cat > "$data_dir/applications/io.local.loop.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=loop
Comment=Your sound. On repeat. A lightweight MP3 loop player.
Exec="$bin_dir/loop" %F
Icon=io.local.loop
Terminal=false
Categories=AudioVideo;Audio;Player;
MimeType=audio/mpeg;
StartupNotify=true
StartupWMClass=loop
Keywords=mp3;music;repeat;loop;audio;
EOF
if command -v update-desktop-database >/dev/null; then update-desktop-database "$data_dir/applications"; fi
if command -v gtk-update-icon-cache >/dev/null; then gtk-update-icon-cache -f -t "$data_dir/icons/hicolor" >/dev/null 2>&1 || true; fi
printf 'Installed loop to %s\n' "$bin_dir/loop"
