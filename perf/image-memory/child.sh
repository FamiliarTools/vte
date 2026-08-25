#!/bin/sh
# The fixture the memory harness floods. Driven entirely through the
# environment run.sh exports; it takes no arguments.
#
# Every checkpoint is a handshake in the filesystem rather than a sleep on the
# harness side, so a slow machine samples LATE rather than sampling the wrong
# thing. A sampling harness that guesses when the terminal has caught up is
# measuring its own queue depth.
#
#   VTE_MEM_WORK      where the marks and the progress counter go
#   VTE_MEM_CORPUS    directory of frame-1 .. frame-N
#   VTE_MEM_PREVIEWS  how many frames to send
#   VTE_MEM_MID       the frame to stop at for the middle sample
#   VTE_MEM_SETTLE    seconds to let the widget drain before a sample
set -u

W=$VTE_MEM_WORK

: >"$W/boot"
while [ ! -e "$W/go" ]; do sleep 0.2; done

i=1
while [ "$i" -le "$VTE_MEM_PREVIEWS" ]; do
        cat "$VTE_MEM_CORPUS/frame-$i"
        printf '%s' "$i" >"$W/progress"
        if [ "$i" = "$VTE_MEM_MID" ]; then
                sleep "$VTE_MEM_SETTLE"
                : >"$W/mark-mid"
                while [ ! -e "$W/go-mid" ]; do sleep 0.2; done
        fi
        i=$((i + 1))
done

sleep "$VTE_MEM_SETTLE"
: >"$W/mark-full"
while [ ! -e "$W/go-full" ]; do sleep 0.2; done

# RIS, then flood again.
#
# Pss alone cannot tell "the terminal released the pixels" from "the terminal
# kept them", because a freed chunk the allocator is sitting on looks exactly
# like a chunk still in use - and the post-RIS number is therefore allocator
# behaviour, not terminal behaviour. Reusing the space can tell them apart: if
# the first flood's memory really was released, the second flood ends where the
# first one did, and if it was not, the second ends a budget higher.
printf '\033c'
sleep "$VTE_MEM_SETTLE"
: >"$W/mark-reset"
while [ ! -e "$W/go-reset" ]; do sleep 0.2; done

i=1
while [ "$i" -le "$VTE_MEM_MID" ]; do
        cat "$VTE_MEM_CORPUS/frame-$i"
        printf '%s' "$i" >"$W/progress"
        i=$((i + 1))
done
sleep "$VTE_MEM_SETTLE"
: >"$W/mark-reuse"

# Park. The harness kills the app; the child must not be what ends the run.
sleep 86400
