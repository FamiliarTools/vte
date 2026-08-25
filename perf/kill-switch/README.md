# Does the kill switch reach the terminal a desktop actually launches?

A switch that only works when you start the terminal by hand is not a switch.
GNOME Console is not started by hand: the desktop activates it over D-Bus and
systemd owns the resulting service, so the question is whether turning SIXEL
off reaches *that* process.

## What the switch is

Not an environment variable. There is no `VTE_SIXEL` in this tree, and there
never was one that shipped - the toggle is
`VteTerminal:enable-sixel`, off by default in the library, and GNOME Console's
`org.gnome.Console sixel-enabled` key bound to it.

That distinction is the whole point of this measurement. An environment
variable is fixed when the service is activated: to change it you would have to
find the unit, edit its environment and kill the running Console. A GSetting
travels over the session bus into a process that is already running. So the
run below asserts that the service PID is *unchanged* across both arms - if it
had restarted, the result would say nothing about propagation.

## What is asked, and why two questions

Each arm runs `probe.sh` inside a Console window and reads two answers back
over the pty:

- **DA1** (`CSI c`). VTE lists attribute `4` only while image parsing is on.
  This is what the terminal *claims* to a program that asks - which is what a
  sixel-capable tool keys off before sending anything.
- **Cursor position** (`CSI 6 n`) after homing the cursor and feeding
  `src/tests/sixel/bands.six`. A decoded image occupies rows, so the cursor
  moves; an ignored DCS leaves it exactly where it was. This is what the
  terminal *did*.

DA1 alone would be an advertisement. The cursor row is the render.

The report also carries its own evidence of the launch path, because the
alternative is assuming it: the probe records the systemd scope it is running
in, and the scope's own description, which names the kgx process that asked for
it.

## Running it

```sh
perf/kill-switch/run.sh /tmp/kill-switch
```

It needs a D-Bus activated Console already running, and a Console built with
the `sixel-enabled` key; it exits 77 (skip) otherwise. It flips the key, so it
saves the previous value and restores it on the way out, including on failure.
Two windows open and close while it runs.

## The result

```
unit                 dbus-:1.1-org.gnome.Console@1.service
service pid          35863 -> 35863
sixel-enabled=on
  da1                ESC[?61;1;4;21;22;28c
  cursor after image ESC[5;1R
  scope              vte-spawn-1c8be743-f64b-4b62-ac12-41dadf6cef8b.scope
  spawned by kgx     35863
sixel-enabled=off
  da1                ESC[?61;1;21;22;28c
  cursor after image ESC[1;1R
  scope              vte-spawn-f5c0d0b2-d0b8-434f-8392-50130c09d4bc.scope
  spawned by kgx     35863
PASS: the kill switch reaches the systemd-launched Console
```

Read across: the `4` leaves the attribute list, and the image stops taking up
its four rows, in a Console that systemd started before the key was ever
touched and that did not restart in between. With the switch off nothing leaked
to the screen as text either - the cursor is still at column 1, so the DCS was
parsed and discarded rather than printed.

## What this does not prove

That the *deployed* Console carries the current branch. It carries some build
of it; which commit is a packaging question, and a Console packaged against a
library without the key exits this script at 77 rather than passing it. The
switch mechanism has not changed across the branch, so an older build is still
evidence about the mechanism.
