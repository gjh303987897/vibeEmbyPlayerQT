#!/usr/bin/env bash
# CI-only keyrings hold synthetic test credentials and are removed on exit.
set -euo pipefail

case "$(uname -s)" in
  Linux)
    if [[ "${1:-}" != --secret-service-session ]]; then
      exec dbus-run-session -- bash "$0" --secret-service-session "$@"
    fi
    shift
    keyring_dir="$(mktemp -d "${RUNNER_TEMP:-/tmp}/vibeplayer-keyring.XXXXXX")"
    trap 'rm -rf -- "$keyring_dir"' EXIT
    export XDG_DATA_HOME="$keyring_dir/data"
    export XDG_RUNTIME_DIR="$keyring_dir/run"
    mkdir -p "$XDG_DATA_HOME" "$XDG_RUNTIME_DIR"
    chmod 700 "$XDG_RUNTIME_DIR"
    eval "$(printf '%s' 'vibeplayer-ci-test-keyring' | gnome-keyring-daemon --unlock --components=secrets)"
    export GNOME_KEYRING_CONTROL
    eval "$(gnome-keyring-daemon --start --components=secrets)"
    ctest "$@"
    ;;
  Darwin)
    keyring_dir="$(mktemp -d "${RUNNER_TEMP:-/tmp}/vibeplayer-keychain.XXXXXX")"
    keychain="$keyring_dir/vibeplayer-ci.keychain-db"
    original_default="$(security default-keychain -d user)"
    original_default="${original_default#*\"}"
    original_default="${original_default%\"*}"
    original_keychains=()
    while IFS= read -r keychain_path; do
      keychain_path="${keychain_path#*\"}"
      original_keychains+=("${keychain_path%\"*}")
    done < <(security list-keychains -d user)
    cleanup() {
      security default-keychain -d user -s "$original_default" || true
      security list-keychains -d user -s "${original_keychains[@]}" || true
      security delete-keychain "$keychain" || true
      rm -rf -- "$keyring_dir"
    }
    trap cleanup EXIT
    security create-keychain -p 'vibeplayer-ci-test-keyring' "$keychain"
    security list-keychains -d user -s "$keychain" "${original_keychains[@]}"
    security default-keychain -d user -s "$keychain"
    security unlock-keychain -p 'vibeplayer-ci-test-keyring' "$keychain"
    security set-keychain-settings -lut 21600 "$keychain"
    ctest "$@"
    ;;
  *)
    echo 'This helper requires Linux or macOS.' >&2
    exit 2
    ;;
esac
