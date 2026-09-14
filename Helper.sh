#!/usr/bin/env bash
set -euo pipefail

dependencies_available() {
  ldconfig -p 2>/dev/null | grep -q 'libSDL2-2\.0\.so' &&
  ldconfig -p 2>/dev/null | grep -q 'libGL\.so\.1' &&
  { command -v fusermount3 >/dev/null 2>&1 ||
    command -v fusermount >/dev/null 2>&1; } &&
  command -v dolphin-emu >/dev/null 2>&1 &&
  command -v bluetoothctl >/dev/null 2>&1
}

start_bluetooth_service() {
  if command -v systemctl >/dev/null 2>&1; then
    systemctl enable --now bluetooth.service >/dev/null 2>&1 ||
      systemctl enable --now bluetooth >/dev/null 2>&1 || true
  fi
}

install_dir="$(cd -- "${FLOWMULATOR_INSTALL_DIR:-$(dirname -- "${BASH_SOURCE[0]}")}" && pwd -P)"
applications_dir="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
icon_path="${XDG_DATA_HOME:-$HOME/.local/share}/icons/flowmulator.svg"
marker="$HOME/.config/flowmulator/dependencies-installed"
runtime_cache_dir="${XDG_CACHE_HOME:-$HOME/.cache}/flowmulator"

apply_window_rules() {
  local hypr_config="$HOME/.config/hypr/hyprland.lua"
  local marker="-- FLOWMULATOR WINDOW RULES"
  mkdir -p "$(dirname "$hypr_config")"
  if ! grep -Fqx "$marker" "$hypr_config" 2>/dev/null; then
    printf '\n%s\n' "$marker" >> "$hypr_config"
    printf '%s\n' \
      'o.window("Flowmulator", { float = true, center = true })' \
      'o.window("org.azahar_emu.Azahar", { float = true, center = true, fullscreen = false, size = { "(monitor_w*730/1920)", "(monitor_h*876/1080)" }, border_size = 2 })' \
      'o.window("dolphin-emu", { float = true, center = true })' \
      'o.window({ class = "dolphin-emu", title = "Dolphin.*" }, { float = true, center = true, size = { "(monitor_w*1594/1920)", "(monitor_h*872/1080)" }, border_size = 2 })' \
      'o.window({ class = "Flowmulator", title = "Flowmulator - SAKURA" }, { border_color = "rgb(f5a0c0)" })' \
      'o.window({ class = "Flowmulator", title = "Flowmulator - RUNNERS" }, { border_color = "rgb(eaff00)" })' \
      'o.window({ class = "Flowmulator", title = "Flowmulator - BERSERK" }, { border_color = "rgb(ffffff)" })' \
      >> "$hypr_config"
  else
    sed -i \
      -e 's/size = { 730, 877 }/size = { "(monitor_w*730\/1920)", "(monitor_h*876\/1080)" }/g' \
      -e 's/size = { 1596, 873 }/size = { "(monitor_w*1594\/1920)", "(monitor_h*872\/1080)" }/g' \
      -e 's/monitor_h\*877\/1080/monitor_h*876\/1080/g' \
      -e 's/monitor_h\*873\/1080/monitor_h*872\/1080/g' \
      -e 's/monitor_w\*730\/1920-1/monitor_w*730\/1920/g' \
      -e 's/monitor_h\*876\/1080-1/monitor_h*876\/1080/g' \
      -e 's/monitor_w\*1596\/1920-1/monitor_w*1594\/1920/g' \
      -e 's/monitor_h\*872\/1080-1/monitor_h*872\/1080/g' \
      "$hypr_config"
  fi
  if command -v hyprctl >/dev/null 2>&1 &&
     [[ -n "${HYPRLAND_INSTANCE_SIGNATURE:-}" ]]; then
    hyprctl reload >/dev/null
    if hyprctl configerrors 2>/dev/null | grep -q .; then
      printf '%s\n' "Hyprland rejected the Flowmulator window rules." >&2
      return 1
    fi
  fi
}

remove_window_rules() {
  local hypr_config="$HOME/.config/hypr/hyprland.lua"
  [[ -f "$hypr_config" ]] || return 0
  local temporary_config
  temporary_config="$(mktemp "${hypr_config}.XXXXXX")"
  awk '
    /-- FLOWMULATOR WINDOW RULES/ {
      next
    }
    skip {
      if ($0 ~ /\}\)/) skip = 0
      next
    }
    /o\.window\("Flowmulator"/ ||
    /o\.window\("org\.azahar_emu\.Azahar"/ ||
    /o\.window\("dolphin-emu"/ ||
    /o\.window\(\{ class = "dolphin-emu"/ ||
    /o\.window\(\{ class = "Flowmulator"/ {
      if ($0 !~ /\}\)/) skip = 1
      next
    }
    { print }
  ' "$hypr_config" > "$temporary_config"
  chmod --reference="$hypr_config" "$temporary_config"
  mv -- "$temporary_config" "$hypr_config"
  if command -v hyprctl >/dev/null 2>&1 &&
     [[ -n "${HYPRLAND_INSTANCE_SIGNATURE:-}" ]]; then
    hyprctl reload >/dev/null
    if hyprctl configerrors 2>/dev/null | grep -q .; then
      printf '%s\n' "Hyprland reported an error after removing the Flowmulator window rules." >&2
      return 1
    fi
  fi
}

pause_after_action() {
  if [[ -t 0 ]]; then
    printf '%s' "Press Enter to close this window..."
    read -r
  fi
}

remove_flowmulator_application() {
  remove_window_rules
  rm -f "$applications_dir/flowmulator.desktop"
  rm -f "$icon_path"
  rm -f "$applications_dir/dolphin-emu.desktop"
  rm -f "$marker"
  rm -rf "$runtime_cache_dir"
  if [[ -x "$install_dir/Flowmulator" ]]; then
    rm -f "$install_dir/Flowmulator"
  fi
  printf '%s\n' "Uninstall succeeded. Flowmulator files and runtime cache were removed; ROMS/ and SAVES/ were preserved."
}

uninstall_dependencies() {
  local as_root=()
  if [[ "${EUID}" -eq 0 ]]; then
    as_root=()
  elif command -v sudo >/dev/null 2>&1; then
    as_root=(sudo)
  else
    printf '%s\n' "Run as root or install sudo to remove dependencies." >&2
    return 1
  fi

  if command -v pacman >/dev/null 2>&1; then
    if !     "${as_root[@]}" pacman -Rns --noconfirm sdl2 libgl pkgconf libretro-mgba \
      dolphin-emu fuse2; then
      printf '%s\n' "Some dependencies could not be removed; continuing with application removal." >&2
    fi
  elif command -v apt-get >/dev/null 2>&1; then
    if ! "${as_root[@]}" apt-get remove -y libsdl2-2.0-0 libgl1 libglx0 \
      dolphin-emu fuse2; then
      printf '%s\n' "Some dependencies could not be removed; continuing with application removal." >&2
    fi
    if ! "${as_root[@]}" apt-get autoremove -y; then
      printf '%s\n' "Automatic cleanup of unused dependencies failed." >&2
    fi
  elif command -v dnf >/dev/null 2>&1; then
    if !     "${as_root[@]}" dnf remove -y SDL2 mesa-libGL pkgconf-pkg-config libretro-mgba \
      dolphin-emu fuse-libs; then
      printf '%s\n' "Some dependencies could not be removed; continuing with application removal." >&2
    fi
  else
    printf '%s\n' "Unsupported package manager; dependencies were not removed." >&2
  fi
}

install_flowmulator() {
  apply_window_rules
  hide_dolphin_launcher
  install_flowmulator_launcher

  if dependencies_available; then
    start_bluetooth_service
    printf '%s\n' "Flowmulator is repaired and all dependencies are installed."
    return 0
  fi

  local as_root=()
  if [[ "${EUID}" -eq 0 ]]; then
    as_root=()
  elif command -v sudo >/dev/null 2>&1; then
    as_root=(sudo)
  else
    printf '%s\n' "Run as root or install sudo before installing dependencies." >&2
    return 1
  fi

  if command -v pacman >/dev/null 2>&1; then
    "${as_root[@]}" pacman -S --needed sdl2 libgl dolphin-emu fuse2 bluez bluez-utils
  elif command -v apt-get >/dev/null 2>&1; then
    "${as_root[@]}" apt-get update
    "${as_root[@]}" apt-get install -y libsdl2-2.0-0 libgl1 libglx0 \
      dolphin-emu bluez
    if ! "${as_root[@]}" apt-get install -y libfuse2; then
      "${as_root[@]}" apt-get install -y libfuse2t64
    fi
  elif command -v dnf >/dev/null 2>&1; then
    "${as_root[@]}" dnf install -y SDL2 mesa-libGL dolphin-emu fuse-libs bluez
  else
    printf '%s\n' "Unsupported package manager. Install the dependencies manually." >&2
    return 1
  fi

  dependencies_available || {
    printf '%s\n' "Repair finished, but a required dependency is still missing." >&2
    return 1
  }
  start_bluetooth_service
  printf '%s\n' "Flowmulator installation completed."
}

repair_install() {
  install_flowmulator
}

update_flowmulator() {
  local repository="${FLOWMULATOR_REPOSITORY:-ferotrysagain2-source/flowmulator}"
  local asset_name="${FLOWMULATOR_RELEASE_ASSET:-Flowmulator.v0.8.zip}"
  local download_url="https://github.com/${repository}/releases/latest/download/${asset_name}"
  local temporary_dir archive extract_dir release_dir candidate file
  local user_config_dir="$HOME/.config/flowmulator"
  local dolphin_config_dir="$HOME/.config/dolphin-emu"
  local azahar_config_dir="$HOME/.config/azahar-emu"
  for command in curl unzip mktemp; do
    if ! command -v "$command" >/dev/null 2>&1; then
      printf 'Updater requires %s.\n' "$command" >&2
      return 1
    fi
  done
  temporary_dir="$(mktemp -d)"
  cleanup_update() {
    rm -rf "$temporary_dir"
  }
  trap cleanup_update RETURN
  for config_dir in "$user_config_dir" "$dolphin_config_dir" \
                    "$azahar_config_dir"; do
    if [[ -d "$config_dir" ]]; then
      mkdir -p "$temporary_dir/config-backup/$(basename "$config_dir")"
      cp -a "$config_dir/." \
        "$temporary_dir/config-backup/$(basename "$config_dir")/"
    fi
  done
  archive="$temporary_dir/$asset_name"
  printf 'Downloading the latest Flowmulator release...\n'
  curl --fail --location --silent --show-error --retry 3 \
    --output "$archive" "${download_url}?update=$(date +%s)"
  extract_dir="$temporary_dir/extracted"
  mkdir "$extract_dir"
  unzip -q "$archive" -d "$extract_dir"
  release_dir="$extract_dir"
  if [[ ! -x "$release_dir/Flowmulator" ]]; then
    candidate="$(find "$extract_dir" -mindepth 1 -maxdepth 1 -type d -print -quit)"
    if [[ -z "$candidate" || ! -x "$candidate/Flowmulator" ]]; then
      printf '%s\n' "Downloaded release has no Flowmulator executable." >&2
      return 1
    fi
    release_dir="$candidate"
  fi
  if ! [[ -s "$release_dir/Flowmulator" ]]; then
    printf '%s\n' "Downloaded release contains an empty Flowmulator executable." >&2
    return 1
  fi
  for file in Flowmulator Helper.sh README.md; do
    if [[ -e "$release_dir/$file" ]]; then
      local staged_file="$install_dir/.${file}.update"
      cp -f "$release_dir/$file" "$staged_file"
      mv -f "$staged_file" "$install_dir/$file"
    fi
  done
  chmod +x "$install_dir/Flowmulator" "$install_dir/Helper.sh"
  FLOWMULATOR_AUTO_INSTALL=1 FLOWMULATOR_INSTALL_DIR="$install_dir" \
    bash "$install_dir/Helper.sh"
  for config_dir in "$user_config_dir" "$dolphin_config_dir" \
                    "$azahar_config_dir"; do
    local backup_dir="$temporary_dir/config-backup/$(basename "$config_dir")"
    if [[ -d "$backup_dir" ]]; then
      mkdir -p "$config_dir"
      cp -a "$backup_dir/." "$config_dir/"
    fi
  done
  printf '%s\n' "Flowmulator was updated. ROMS/ and SAVES/ were left unchanged."
}

hide_dolphin_launcher() {
  mkdir -p "$applications_dir"
  cat > "$applications_dir/dolphin-emu.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Dolphin Emulator
NoDisplay=true
EOF
}

install_flowmulator_launcher() {
  mkdir -p "$applications_dir"
  if [[ ! -x "$install_dir/Flowmulator" ]]; then
    printf '%s\n' "Could not find Flowmulator in $install_dir." >&2
    return 1
  fi
  if ! "$install_dir/Flowmulator" --install-icon; then
    printf '%s\n' "Could not install the Flowmulator icon." >&2
    return 1
  fi
  cat > "$applications_dir/flowmulator.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Flowmulator
Comment=Retro gaming station for GBA, NDS, and Wii
Exec="$install_dir/Flowmulator"
Path=$install_dir
Icon=$icon_path
Terminal=false
Categories=Game;Emulator;
StartupWMClass=Flowmulator
EOF
}

if [[ "$(uname -s)" != "Linux" ]]; then
  printf '%s\n' "Flowmulator requires Linux." >&2
  exit 1
fi

if [[ "${FLOWMULATOR_AUTO_INSTALL:-0}" != "1" && -t 0 ]]; then
  printf '%s\n' "Flowmulator installer"
  printf '%s\n' "1) Install Flowmulator"
  printf '%s\n' "2) Update Flowmulator"
  printf '%s\n' "3) Repair installation"
  printf '%s\n' "4) Uninstall Flowmulator and its dependencies"
  printf '%s' "Choose an option [1-4]: "
  read -r choice
  case "$choice" in
    4)
      printf '%s' "This removes installed dependencies and the application launcher. Continue? [y/N] "
      read -r confirmation
      if [[ "$confirmation" =~ ^[Yy]$ ]]; then
        uninstall_dependencies
        remove_flowmulator_application
        printf '%s\n' "Uninstall completed successfully."
        pause_after_action
      else
        printf '%s\n' "Uninstall cancelled."
      fi
      exit 0
      ;;
    1|"")
      install_flowmulator
      printf '%s\n' "Install completed successfully."
      pause_after_action
      exit 0
      ;;
    2)
      update_flowmulator
      printf '%s\n' "Update completed successfully."
      pause_after_action
      exit 0
      ;;
    3)
      repair_install
      printf '%s\n' "Repair completed successfully."
      pause_after_action
      exit 0
      ;;
    *)
      printf '%s\n' "Invalid choice." >&2
      exit 2
      ;;
  esac
fi

repair_install
