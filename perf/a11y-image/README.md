# An image marked in the accessibility tree and in the clipboard

Asserts that both text paths mark an image's position with U+FFFC:

    CLIPBOARD_HAS_UFFFC=yes     a copy must carry the image's position
                                (vte#309)
    A11Y_HAS_UFFFC=yes          a reader must be able to tell an image
                                from blank space

Build against the tree's libvte:

    g++ -O2 -o a11y-image a11y-image.c \
        -I ../../src -I ../../src/vte -I ../../_build/src -I ../../_build/src/vte \
        $(pkg-config --cflags gtk+-3.0) -L ../../_build/src -lvte-2.91 \
        $(pkg-config --libs gtk+-3.0)

Run it against a SIXEL file, on a display:

    LD_LIBRARY_PATH=../../_build/src ./a11y-image /tmp/p.six

## Two traps, both of which produced a false FAIL first

1. `vte_terminal_set_enable_a11y(term, TRUE)` is required. The snapshot
   returns early when a11y is off, so the contents never refresh.

2. On the GTK4 backend the contents are DOUBLE BUFFERED and `contents_flip`
   is deliberately not flipped on the same change ("so that we can allow the
   AT context the ability to access the current contents on DELETE
   operations"). Reading immediately after the image is emitted returns the
   PREVIOUS snapshot. The harness drives two further content changes before
   asserting, which is harmless on GTK3 too.

Without either of those the test reports a stale, empty snapshot and looks
like a code failure.
