#!/usr/bin/env bash
# Prove the SIXEL kill switch reaches GNOME Console in the shape it is
# actually launched in: a D-Bus activated, systemd-managed user service, not a
# terminal started by hand from a shell.
#
# usage: run.sh <out-dir>
#
# The switch is org.gnome.Console's `sixel-enabled` key, bound to
# VteTerminal:enable-sixel. It is a GSetting and not an environment variable,
# which matters here: an environment variable is fixed when the service is
# activated and could only be changed by killing it, whereas dconf reaches the
# already-running process over the session bus. So the run asserts that the
# same service PID served both arms - if it restarted between them, the run
# proved nothing about propagation.
#
# Each arm asks the terminal two questions over the pty and compares answers:
#
#   DA1  attribute 4 present  the terminal advertises image support
#   CPR  cursor row after a   the image was decoded and took up rows
#        real .six
#
# Both must flip together. DA1 alone would only show what the terminal claims;
# the cursor row shows what it did.
set -u

OUT=${1:?output directory}

HERE=$(cd "$(dirname "$0")" && pwd)
SIX=$HERE/../../src/tests/sixel/bands.six
SCHEMA=org.gnome.Console
KEY=sixel-enabled

for tool in gsettings kgx systemctl; do
        command -v "$tool" >/dev/null || { echo "SKIP: $tool not found"; exit 77; }
done

gsettings list-keys "$SCHEMA" 2>/dev/null | grep -qx "$KEY" || {
        echo "SKIP: $SCHEMA has no $KEY key; this Console was built without the switch"
        exit 77
}

mkdir -p "$OUT"

# The unit that owns the running Console. Its name is assigned by D-Bus
# activation, so match on what it runs rather than on a fixed unit name.
unit_of_console() {
        systemctl --user list-units --all --plain --no-legend \
                  'dbus-*org.gnome.Console*.service' 2>/dev/null |
                awk '{print $1; exit}'
}

pid_of_unit() {
        [ -n "$1" ] || return 0
        systemctl --user show "$1" -p MainPID --value 2>/dev/null
}

unit=$(unit_of_console)
[ -n "$unit" ] || {
        echo "SKIP: no D-Bus activated GNOME Console is running; open one first"
        exit 77
}
pid_before=$(pid_of_unit "$unit")

saved=$(gsettings get "$SCHEMA" "$KEY")
restore() { gsettings set "$SCHEMA" "$KEY" "$saved"; }
trap restore EXIT

arm() { # <name> <value>
        local name=$1 value=$2
        local rep=$OUT/$name.txt
        rm -f "$rep"
        gsettings set "$SCHEMA" "$KEY" "$value"
        kgx -e "$HERE/probe.sh $rep $SIX" >/dev/null 2>&1
        local i
        for i in $(seq 150); do
                [ -s "$rep" ] && return 0
                sleep 0.1
        done
        echo "FAIL: $name produced no report; the probe never ran"
        return 1
}

arm on true || exit 1
arm off false || exit 1

pid_after=$(pid_of_unit "$unit")
restore
trap - EXIT

read_field() { sed -n "s/^$2=//p" "$OUT/$1.txt"; }

fail=0
say() { printf '%s\n' "$1" | tee -a "$OUT/report.txt"; }
: >"$OUT/report.txt"

say "unit                 $unit"
say "service pid          $pid_before -> $pid_after"
[ -n "$pid_before" ] && [ "$pid_before" = "$pid_after" ] || {
        say "FAIL: the service did not survive the switch, so nothing propagated into it"
        fail=1
}

for name in on off; do
        say "$KEY=$name"
        say "  da1                $(read_field "$name" da1)"
        say "  cursor after image $(read_field "$name" cpr)"
        say "  scope              $(read_field "$name" scope)"
        say "  spawned by kgx     $(read_field "$name" terminal)"
        case $(read_field "$name" scope) in
        vte-spawn-*) ;;
        *)
                say "  FAIL: the probe did not run in a systemd scope"
                fail=1
                ;;
        esac
        [ "$(read_field "$name" terminal)" = "$pid_before" ] || {
                say "  FAIL: a Console other than the service answered this arm"
                fail=1
        }
done

# The image is 4 rows of the emulated cell, so `on` must leave the cursor
# below row 1 and `off` must leave it exactly where the cursor was homed.
case $(read_field on da1) in *';4;'*) ;; *) say "FAIL: no image attribute with the switch on"; fail=1;; esac
case $(read_field off da1) in *';4;'*) say "FAIL: image attribute survived the switch"; fail=1;; esac
case $(read_field on cpr) in 'ESC[1;1R') say "FAIL: the image took up no rows with the switch on"; fail=1;; esac
case $(read_field off cpr) in 'ESC[1;1R') ;; *) say "FAIL: something moved the cursor with the switch off"; fail=1;; esac

[ "$fail" = 0 ] && say "PASS: the kill switch reaches the systemd-launched Console"
exit "$fail"
