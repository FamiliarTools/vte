#!/usr/bin/env bash
# Ask the terminal this is running in whether it decodes SIXEL, and record who
# launched it. Everything here is read back over the pty or out of /proc;
# nothing is inferred from a unit file or a store path.
#
# usage: probe.sh <report-file> <sixel-file>
set -u

OUT=${1:?report file}
SIX=${2:?sixel file}

saved=$(stty -g)
stty raw -echo

# Read a reply up to its terminating character, or give up after a second.
reply() {
        local term=$1 ch acc=''
        while IFS= read -r -N1 -t 1 ch; do
                acc+=$ch
                [ "$ch" = "$term" ] && break
        done
        printf '%s' "$acc"
}

# DA1. VTE lists attribute 4 only while image parsing is on, so the answer
# says what this terminal will admit to a program that asks.
printf '\033[c'
da1=$(reply c)

# The image itself. Home the cursor, feed the file, ask where the cursor ended
# up: a decoded image occupies rows and moves it, an ignored DCS does not.
printf '\033[2J\033[H'
cat "$SIX"
printf '\033[6n'
cpr=$(reply R)

stty "$saved"

# Who this ran under. The scope says systemd owns it; walking up to the
# nearest kgx names the exact process, so the report carries its own evidence
# of the launch path instead of leaving the caller to assume it.
scope=$(sed -n 's|.*/\(vte-spawn-[^/]*\.scope\).*|\1|p' /proc/self/cgroup | head -n1)

# systemd's own description of the scope names the terminal that asked for it,
# and the scope only exists while this probe is alive, so it has to be read
# from here rather than after the fact.
terminal=$(systemctl --user show "$scope" -p Description --value 2>/dev/null |
        sed -n 's/.*launched by kgx process \([0-9]*\).*/\1/p')

{
        printf 'da1=%s\n' "$(printf '%s' "$da1" | sed 's/\x1b/ESC/g')"
        printf 'cpr=%s\n' "$(printf '%s' "$cpr" | sed 's/\x1b/ESC/g')"
        printf 'scope=%s\n' "${scope:-none}"
        printf 'terminal=%s\n' "${terminal:-none}"
} >"$OUT"
