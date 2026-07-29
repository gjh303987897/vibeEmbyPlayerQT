#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 3 || $# -gt 4 ]]; then
  echo "Usage: $0 <installed-package-dir> <output.flatpak> <x86_64|aarch64> [app-id]" >&2
  exit 2
fi

package_dir="$(realpath "$1")"
output_path="$(realpath -m "$2")"
flatpak_arch="$3"
app_id="${4:-io.github.gjh303987897.vibeEmbyPlayerQT}"
runtime_version="${FLATPAK_RUNTIME_VERSION:-25.08}"

case "$flatpak_arch" in
  x86_64|aarch64)
    ;;
  *)
    echo "Unsupported Flatpak architecture: $flatpak_arch" >&2
    exit 2
    ;;
esac

if [[ ! "$app_id" =~ ^[A-Za-z_][A-Za-z0-9_]*(\.[A-Za-z_][A-Za-z0-9_]*){2,}$ ]]; then
  echo "Invalid Flatpak application ID: $app_id" >&2
  exit 2
fi

for command_name in convert flatpak identify ostree; do
  if ! command -v "$command_name" >/dev/null 2>&1; then
    echo "Required command is unavailable: $command_name" >&2
    exit 1
  fi
done

work_parent="${RUNNER_TEMP:-/tmp}"
work_dir="$(mktemp -d "$work_parent/vibePlayerQT-flatpak.XXXXXX")"
trap 'rm -rf "$work_dir"' EXIT

build_dir="$work_dir/build"
repo_dir="$work_dir/repo"
verify_repo="$work_dir/verify-repo"
mkdir -p "$(dirname "$output_path")"
flatpak build-init \
  --arch="$flatpak_arch" \
  "$build_dir" \
  "$app_id" \
  org.freedesktop.Sdk \
  org.freedesktop.Platform \
  "$runtime_version"
cp -a "$package_dir/." "$build_dir/files/"
rm -f "$build_dir/files/vibePlayerQT.sh"

desktop_dir="$build_dir/files/share/applications"
source_icon_dir="$build_dir/files/share/icons/hicolor/1024x1024/apps"
flatpak_icon_dir="$build_dir/files/share/icons/hicolor/512x512/apps"
desktop_file="$desktop_dir/$app_id.desktop"
source_icon="$source_icon_dir/vibePlayerQT.png"
icon_file="$flatpak_icon_dir/$app_id.png"

mv "$desktop_dir/vibePlayerQT.desktop" "$desktop_file"
sed -i \
  -e 's/^Exec=.*/Exec=vibePlayerQT/' \
  -e "s/^Icon=.*/Icon=$app_id/" \
  "$desktop_file"
mkdir -p "$flatpak_icon_dir"
convert "$source_icon" -resize 512x512 "$icon_file"
test "$(identify -format '%wx%h' "$icon_file")" = "512x512"
rm -f "$source_icon"

flatpak build-finish \
  --no-inherit-permissions \
  --command=vibePlayerQT \
  --share=network \
  --share=ipc \
  --socket=x11 \
  --socket=wayland \
  --socket=pulseaudio \
  --device=dri \
  --filesystem=host \
  "$build_dir"

flatpak build-export --arch="$flatpak_arch" "$repo_dir" "$build_dir" stable
flatpak build-bundle \
  --arch="$flatpak_arch" \
  --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo \
  "$repo_dir" \
  "$output_path" \
  "$app_id" \
  stable

test -s "$output_path"
ostree init --repo="$verify_repo" --mode=archive-z2
flatpak build-import-bundle "$verify_repo" "$output_path"
ostree --repo="$verify_repo" refs | grep -Fx "app/$app_id/$flatpak_arch/stable"
