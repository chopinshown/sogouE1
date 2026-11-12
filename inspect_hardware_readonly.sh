#!/bin/sh
# Run inside an explicitly selected recorder ADB shell. Reads only.
section() { printf '\n--- %s ---\n' "$1"; }
read_file() { if [ -r "$1" ]; then printf '\n%s\n' "$1"; cat "$1"; fi; }

section 'Identity and operating system'
id
uname -a
read_file /etc/os-release
read_file /etc/version
read_file /proc/version
for path in /sys/firmware/devicetree/base/model /sys/firmware/devicetree/base/compatible; do
  if [ -r "$path" ]; then printf '\n%s\n' "$path"; tr '\000' '\n' < "$path"; fi
done

section 'CPU and memory'
read_file /proc/cpuinfo
read_file /proc/meminfo
for path in /sys/devices/system/cpu/online /sys/devices/system/cpu/possible \
  /sys/devices/system/cpu/cpu*/cpufreq/cpuinfo_max_freq \
  /sys/devices/system/cpu/cpu*/cpufreq/scaling_available_frequencies; do
  read_file "$path"
done

section 'Storage hardware and partition sizes'
read_file /proc/partitions
for path in /sys/class/block/mmcblk*/device/name \
  /sys/class/block/mmcblk*/device/type /sys/class/block/mmcblk*/device/cid \
  /sys/class/block/mmcblk*/device/manfid /sys/class/block/mmcblk*/device/oemid \
  /sys/class/block/mmcblk*/device/date /sys/class/block/mmcblk*/device/fwrev \
  /sys/class/block/mmcblk*/size; do
  read_file "$path"
done
df -k

section 'Audio devices'
read_file /proc/asound/cards
read_file /proc/asound/pcm

section 'Framebuffer and display'
for path in /sys/class/graphics/fb*/name /sys/class/graphics/fb*/virtual_size \
  /sys/class/graphics/fb*/bits_per_pixel /sys/class/graphics/fb*/modes; do
  read_file "$path"
done

section 'Battery identification and status'
for path in /sys/class/power_supply/*/type /sys/class/power_supply/*/manufacturer \
  /sys/class/power_supply/*/model_name /sys/class/power_supply/*/technology \
  /sys/class/power_supply/*/capacity /sys/class/power_supply/*/voltage_now \
  /sys/class/power_supply/*/charge_full_design; do
  read_file "$path"
done

# Legacy embedded adbd may keep its PTY open after stdin reaches EOF.
exit
