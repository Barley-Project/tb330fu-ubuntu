#!/bin/bash
# TB330FU remote debug loop, PC side (Linux, macOS or Git-Bash on Windows).  Overview: docs/11-dev-loop.md.
# Settings (environment): TB_SSH_KEY (default ~/.ssh/tb330fu_dev), TB_USER (ubuntu), TB_PASSWORD (for "tb.sh sudo" / serial login), FASTBOOT (fastboot binary),
# TB_FASTBOOT_SERIAL (only needed if several devices are attached), TB_DEV_OUT (screenshots and logs), TB_ADDR (tablet address if not the default).
#
#   tb.sh addr                 find the tablet on the USB-NCM link (prints the address)
#   tb.sh wait [seconds]       wait until ssh answers (default 240 s)
#   tb.sh ssh [cmd...]         run a command as $TB_USER (no argument: interactive shell)
#   tb.sh sudo cmd...          run a command as root (sudo with $TB_PASSWORD fed on stdin; your stdin follows to the command)
#   tb.sh sh                   read a shell script from stdin and run it as root on the tablet
#   tb.sh put src... dst       scp to the tablet            tb.sh get src dst   scp from the tablet
#   tb.sh status               tb330fu-status (kernel, usb role, charger, units, drm, modules, dmesg tail)
#   tb.sh dmesg|journal [n]    kernel log / journal of the current boot, saved under $TB_DEV_OUT
#   tb.sh shot [name]          screenshot of the scanned framebuffer -> $TB_DEV_OUT/<name>.png (landscape)
#   tb.sh time                 set the tablet clock from the PC (no RTC battery, TLS / apt need a sane date)
#   tb.sh fastboot             ssh -> reboot-mode -> bootloader, wait until `fastboot devices` lists the tablet
#   tb.sh flash <part> <img>   fastboot flash (whitelist: vendor_boot_a, vendor_boot_b, userdata, boot_a, boot_b); needs the tablet in fastboot
#   tb.sh reboot               fastboot reboot
#   tb.sh cycle <part> <img>   fastboot -> flash -> reboot -> wait for ssh   (the whole "try a new image" loop)
#   tb.sh serial run "<cmd>"   same as `tb.sh sudo` but over the CDC-ACM serial console (works without the IP link)
#   tb.sh nic                  show the PC-side network adapter of the gadget (MAC 02:00:00:00:00:02)
set -u
L="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KEY="${TB_SSH_KEY:-$HOME/.ssh/tb330fu_dev}"
TB_USER="${TB_USER:-ubuntu}"
FB="${FASTBOOT:-fastboot}"
TB_SERIAL="${TB_FASTBOOT_SERIAL:-}"
OUT="${TB_DEV_OUT:-$L/.tb-out}"
mkdir -p "$OUT"
SSHO=(-i "$KEY" -o IdentitiesOnly=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o BatchMode=yes
      -o ConnectTimeout=5 -o ServerAliveInterval=5 -o ServerAliveCountMax=4)
PW="${TB_PASSWORD:-}"

nic() {
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$L/devtools/ncm-adapter.ps1")" 2>/dev/null | tr -d '\r'
}

try_ssh() { ssh -n "${SSHO[@]}" -o ConnectTimeout=4 "$TB_USER@$1" true >/dev/null 2>&1 </dev/null; }

addr() {
    [ -n "${TB_ADDR:-}" ] && { echo "$TB_ADDR"; return 0; }
    if try_ssh 192.168.7.2; then echo 192.168.7.2; return 0; fi
    local idx
    idx="$(nic | awk -F'\t' 'NR==1{print $1}')"
    if [ -n "$idx" ] && try_ssh "fe80::ff:fe00:1%$idx"; then echo "fe80::ff:fe00:1%$idx"; return 0; fi
    return 1
}

A=""
need_addr() { [ -n "$A" ] && return 0; A="$(addr)" || { echo "tb.sh: the tablet does not answer ssh (try: tb.sh nic; tb.sh serial run 'uname -a'; fastboot devices)" >&2; return 1; }; }

t_ssh()  { need_addr || return 1; ssh "${SSHO[@]}" "$TB_USER@$A" "$@"; }
t_sudo() { need_addr || return 1; { echo "$PW"; cat; } | ssh "${SSHO[@]}" "$TB_USER@$A" "sudo -S -p '' $*"; }

fb_listed() { "$FB" devices 2>/dev/null | grep -q "^$TB_SERIAL[[:alnum:]]*[[:space:]]"; }

cmd="${1:-}"; [ $# -gt 0 ] && shift
case "$cmd" in
    addr)  addr || { echo "not reachable"; exit 1; } ;;
    nic)   nic ;;
    wait)
        secs="${1:-240}"; t0=$(date +%s)
        while :; do
            if A="$(addr)"; then echo "up: $A after $(( $(date +%s) - t0 )) s"; exit 0; fi
            [ $(( $(date +%s) - t0 )) -ge "$secs" ] && { echo "timeout after $secs s"; exit 1; }
            sleep 3
        done ;;
    ssh)
        need_addr || exit 1
        if [ $# -eq 0 ]; then exec ssh "${SSHO[@]}" -o BatchMode=no "$TB_USER@$A"; else t_ssh "$@"; fi ;;
    sudo)  t_sudo "$@" </dev/null ;;
    sh)    t_sudo sh -s ;;
    put)
        need_addr || exit 1
        n=$#; dst="${!n}"; srcs=("${@:1:$((n-1))}")
        case "$A" in *:*) h="[$A]" ;; *) h="$A" ;; esac
        scp "${SSHO[@]}" -r "${srcs[@]}" "$TB_USER@$h:$dst" ;;
    get)
        need_addr || exit 1
        case "$A" in *:*) h="[$A]" ;; *) h="$A" ;; esac
        scp "${SSHO[@]}" -r "$TB_USER@$h:$1" "$2" ;;
    status) t_sudo tb330fu-status </dev/null ;;
    dmesg)  f="$OUT/dmesg_$(date +%m%d_%H%M%S).txt"; t_sudo dmesg </dev/null > "$f"; echo "$f ($(wc -l < "$f") lines)"; tail -n "${1:-40}" "$f" ;;
    journal) f="$OUT/journal_$(date +%m%d_%H%M%S).txt"; t_sudo journalctl -b --no-pager </dev/null > "$f"; echo "$f ($(wc -l < "$f") lines)"; tail -n "${1:-40}" "$f" ;;
    shot)
        name="${1:-shot_$(date +%m%d_%H%M%S)}"
        t_sudo tb330fu-shot </dev/null > "$OUT/$name.raw" || exit 1
        python -I "$L/devtools/tbshot.py" "$OUT/$name.raw" "$OUT/$name.png" && rm -f "$OUT/$name.raw"
        cp "$OUT/$name.png" "$OUT/latest.png" ;;
    time)   t_sudo date -u -s "'$(date -u '+%Y-%m-%d %H:%M:%S')'" </dev/null ;;
    fastboot)
        fb_listed && { echo "already in fastboot"; exit 0; }
        t_sudo tb330fu-fastboot </dev/null >/dev/null 2>&1 &
        wait $! 2>/dev/null
        t0=$(date +%s)
        while ! fb_listed; do
            [ $(( $(date +%s) - t0 )) -ge 120 ] && { echo "tb.sh: no fastboot device after 120 s (the bootloader did not honour the reboot argument?)"; exit 1; }
            sleep 2
        done
        echo "fastboot: $("$FB" devices | head -1)" ;;
    flash)
        part="$1"; img="$2"
        case "$part" in vendor_boot_a|vendor_boot_b|userdata|boot_a|boot_b) ;; *) echo "tb.sh flash: partition '$part' is not on the whitelist"; exit 3 ;; esac
        [ -s "$img" ] || { echo "tb.sh flash: no such image $img"; exit 3; }
        fb_listed || { echo "tb.sh flash: the tablet ($TB_SERIAL) is not in fastboot"; exit 3; }
        "$FB" -s "$TB_SERIAL" flash "$part" "$img" ;;
    reboot) "$FB" -s "$TB_SERIAL" reboot ;;
    cycle)
        "$0" fastboot || exit 1
        "$0" flash "$1" "$2" || exit 1
        "$0" reboot || exit 1
        sleep 20
        "$0" wait 300 ;;
    serial) python -I "$L/devtools/tbserial.py" "$@" ;;
    *) sed -n '2,24p' "$0"; exit 1 ;;
esac
