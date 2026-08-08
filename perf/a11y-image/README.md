# An image in the accessibility tree, but not in the clipboard

Asserts that the two text paths disagree in exactly the intended way:

    CLIPBOARD_HAS_UFFFC=no      copying must not interleave image junk
    A11Y_HAS_UFFFC=yes          a reader must be able to tell an image
                                from blank space

Build against the tree's libvte:

    g++ -O2 -o a11y-image a11y-image.c -I ../../src -I ../../_b/src \
        $(pkg-config --cflags gtk4) -L ../../_b/src -lvte-2.91-gtk4 \
        $(pkg-config --libs gtk4)

## Two traps, both of which produced a false FAIL first

1. `vte_terminal_set_enable_a11y(term, TRUE)` is required. The snapshot in
   `vte_accessible_text_contents_changed()` returns early when a11y is off,
   so the contents never refresh.

2. The contents are DOUBLE BUFFERED and `contents_flip` is deliberately not
   flipped on the same change ("so that we can allow the AT context the
   ability to access the current contents on DELETE operations"). Reading
   immediately after the image is emitted returns the PREVIOUS snapshot. The
   harness drives two further content changes before asserting.

Without either of those the test reports a stale, empty snapshot and looks
like a code failure.
