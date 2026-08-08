# Cross-terminal processing fairness under a SIXEL flood

Two `VteTerminal`s in ONE process, so they share the main loop and whatever
fairness accounting `process_incoming()` does. Terminal A floods, terminal B
does a fixed amount of plain output and records when it finishes. B's wall
time is the starvation measure.

Three modes, and the third is the one that makes the result mean anything:

    idle       A does nothing
    textflood  A floods TEXT at a comparable rate
    flood      A floods tiny SIXEL images

Without the `textflood` control the numbers say only "a busy sibling costs
throughput", which is unsurprising and not a bug.

## Measured on this tree (0.84 + the sixel work, GSK cairo renderer, Xvfb)

    idle       0.29s  0.29s  0.31s  0.28s
    textflood  0.30s  0.33s
    flood      0.90s  0.72s  0.86s  0.81s

A text-flooding sibling costs about 5%. A sixel-flooding sibling costs 150%
to 210%. The gap is specific to image decoding, not general contention.

## Why

The incoming budget is denominated in BYTES READ FROM THE PTY
(`m_input_bytes`, `m_max_input_bytes`, `VTE_MAX_PROCESS_TIME`). A sixel byte
costs orders of magnitude more work than a text byte, so a byte-based budget
under-charges images.

## What DOES fix it

Give a round that decoded an image a shorter slice
(`VTE_MAX_PROCESS_TIME_IMAGE`, 20ms, against `VTE_MAX_PROCESS_TIME`'s 100ms).
The adaptive step then shrinks this terminal's read budget, so it returns the
main loop to its siblings sooner.

    idle       0.22s  0.23s
    textflood  0.24s  0.23s
    flood      0.31s  0.35s

Starvation drops from about 2.7x to about 1.45x. The cost is borne by the
terminal doing the flooding, measured separately: 3000 images take 0.19s
instead of 0.13s, about 46% slower. That trade is the point - a terminal
decoding images gives up throughput so its siblings keep theirs.

## What does NOT fix it

Adding the decoded pixel count to `m_input_bytes` - the obvious first idea -
makes it WORSE, measured at 1.19s and 1.53s. The adaptive step is

    target = VTE_MAX_PROCESS_TIME / elapsed * m_input_bytes

so inflating `m_input_bytes` RAISES the read budget and the terminal pulls
more sixel data per round. Any fix has to reduce what this terminal reads,
not increase what it claims to have read.
