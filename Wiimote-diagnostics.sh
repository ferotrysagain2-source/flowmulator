#!/usr/bin/env bash
set -u

if [[ "${FLOWMULATOR_DIAGNOSTICS_TEE:-}" != "1" ]]; then
  report_dir="${XDG_DOWNLOAD_DIR:-$HOME/Downloads}"
  [[ -d "$report_dir" ]] || report_dir="$HOME"
  report_file="$report_dir/Wiimote-diagnostics-$(date +%Y%m%d-%H%M%S).txt"
  export FLOWMULATOR_DIAGNOSTICS_TEE=1
  exec > >(tee "$report_file") 2>&1
  printf 'Report file: %s\n' "$report_file"
fi

print_section() {
  printf '\n===== %s =====\n' "$1"
}

run_command() {
  printf '$ %s\n' "$*"
  "$@" 2>&1 || printf '[command exited with status %s]\n' "$?"
}

run_shell() {
  printf '$ %s\n' "$1"
  bash -c "$1" 2>&1 || printf '[command exited with status %s]\n' "$?"
}

printf '%s\n' \
  'Flowmulator Wii Remote diagnostics' \
  'This script only reads diagnostics and performs a temporary Bluetooth scan.' \
  'It does not install packages, change Dolphin settings, or pair/unpair devices.'

print_section "System"
run_shell 'printf "Date: "; date --iso-8601=seconds'
run_shell 'printf "Kernel: "; uname -srmo'
run_shell 'printf "Desktop: "; printf "%s\n" "${XDG_CURRENT_DESKTOP:-unknown}"'
run_shell 'printf "Session: "; printf "%s\n" "${XDG_SESSION_TYPE:-unknown}"'
run_shell 'printf "Flowmulator directory: "; printf "%s\n" "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"'

print_section "Required commands"
for command_name in bluetoothctl systemctl rfkill dolphin-emu hyprctl; do
  if command -v "$command_name" >/dev/null 2>&1; then
    printf '%-15s %s\n' "$command_name" "$(command -v "$command_name")"
  else
    printf '%-15s MISSING\n' "$command_name"
  fi
done

print_section "Bluetooth adapter and service"
if command -v systemctl >/dev/null 2>&1; then
  run_command systemctl is-enabled bluetooth.service
  run_command systemctl is-active bluetooth.service
  run_command systemctl --no-pager --full status bluetooth.service
fi
if command -v rfkill >/dev/null 2>&1; then
  run_command rfkill list bluetooth
fi
if command -v bluetoothctl >/dev/null 2>&1; then
  run_shell 'bluetoothctl show'
  run_shell 'bluetoothctl list'
  run_shell 'bluetoothctl devices'
  run_shell 'bluetoothctl paired-devices'
fi

print_section "Bluetooth scan"
printf '%s\n' \
  'Put the Wii Remote into pairing mode now by holding 1 + 2.' \
  'The scan will run for 15 seconds. Keep holding or briefly press 1 + 2 during it.'
if command -v bluetoothctl >/dev/null 2>&1; then
  printf '$ bluetoothctl scan on; sleep 15; bluetoothctl scan off; bluetoothctl devices\n'
  {
    printf '%s\n' 'power on' 'agent on' 'default-agent' 'scan on'
    sleep 15
    printf '%s\n' 'scan off' 'devices' 'paired-devices' 'quit'
  } | bluetoothctl 2>&1 || printf '[Bluetooth scan command failed]\n'
else
  printf '%s\n' 'bluetoothctl is missing; no scan was performed.'
fi

print_section "Dolphin"
if command -v dolphin-emu >/dev/null 2>&1; then
  run_command dolphin-emu --version
fi
dolphin_config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/dolphin-emu"
printf 'Dolphin config directory: %s\n' "$dolphin_config_dir"
if [[ -d "$dolphin_config_dir" ]]; then
  run_shell "grep -HnE '^(Wiimote1|WiimoteContinuousScanning|Source|BackgroundInput|EnableSpeaker)' '$dolphin_config_dir/Dolphin.ini' '$dolphin_config_dir/WiimoteNew.ini' 2>/dev/null"
  run_shell "find '$dolphin_config_dir' -maxdepth 2 -type f \\( -iname '*log*' -o -iname '*.txt' \\) -printf '%p\\n' 2>/dev/null | sort"
else
  printf '%s\n' 'Dolphin config directory does not exist.'
fi

print_section "Flowmulator Wii Remote settings"
flow_config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/flowmulator"
printf 'Flowmulator config directory: %s\n' "$flow_config_dir"
if [[ -d "$flow_config_dir" ]]; then
  run_shell "grep -HnEi 'wiimote|wii|input' '$flow_config_dir'/* 2>/dev/null"
else
  printf '%s\n' 'Flowmulator config directory does not exist.'
fi

print_section "Recent Bluetooth and Dolphin logs"
if command -v journalctl >/dev/null 2>&1; then
  run_command journalctl --no-pager -b --since "30 minutes ago" -u bluetooth.service
  run_shell 'journalctl --no-pager -b --since "30 minutes ago" 2>/dev/null | grep -iE "dolphin|wiimote|bluetooth|bluez|hid|l2cap" | tail -n 200'
fi

print_section "Hyprland and running processes"
if command -v hyprctl >/dev/null 2>&1; then
  run_command hyprctl version
  run_command hyprctl clients
fi
run_shell 'ps -ef | grep -iE "[d]olphin-emu|[b]luetoothd|[f]lowmulator"'

print_section "End of report"
printf '%s\n' \
  'Copy everything from "Flowmulator Wii Remote diagnostics" through this line and send it back.' \
  'The scan result and the Bluetooth/Dolphin log sections are the most important parts.'
