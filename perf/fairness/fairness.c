/* Two VteTerminals in ONE process, so they share the main loop and whatever
 * fairness accounting process_incoming does.
 *
 * Terminal A either floods sixels or sits idle. Terminal B runs a fixed amount
 * of plain output and touches a file when it finishes. The wall time B takes,
 * with A flooding versus with A idle, is the starvation measure.
 */
#include <vte/vte.h>
#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>

static const char *mode;
static const char *donefile;

static void on_spawn(VteTerminal *t, GPid pid, GError *err, gpointer u) {
        if (err) g_printerr("spawn failed: %s\n", err->message);
}

static void activate(GtkApplication *app, gpointer u) {
        GtkWidget *win = gtk_application_window_new(app);
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

        VteTerminal *a = VTE_TERMINAL(vte_terminal_new());
        VteTerminal *b = VTE_TERMINAL(vte_terminal_new());
        vte_terminal_set_enable_sixel(a, TRUE);
        vte_terminal_set_enable_sixel(b, TRUE);

        gtk_box_append(GTK_BOX(box), GTK_WIDGET(a));
        gtk_box_append(GTK_BOX(box), GTK_WIDGET(b));
        gtk_window_set_child(GTK_WINDOW(win), box);

        /* A: the aggressor, or idle in the control run. */
        char *acmd;
        if (g_strcmp0(mode, "flood") == 0)
                acmd = g_strdup("while :; do printf '\\033Pq#1~~~~~~~~~~~~~~~~\\033\\\\'; done");
        else if (g_strcmp0(mode, "textflood") == 0)
                /* Control: the same shape of work with no sixel in it, so
                 * ordinary contention from a busy sibling is separated from
                 * anything sixel decode does specifically.
                 */
                acmd = g_strdup("while :; do printf 'xxxxxxxxxxxxxxxxxxxxxxxx\\n'; done");
        else
                acmd = g_strdup("sleep 600");
        char *aargv[] = { (char*)"/bin/sh", (char*)"-c", acmd, NULL };
        vte_terminal_spawn_async(a, VTE_PTY_DEFAULT, NULL, aargv, NULL,
                                 (GSpawnFlags)0, NULL, NULL, NULL, -1, NULL,
                                 on_spawn, NULL);

        /* B: the victim. Fixed work, then a marker. */
        char *bcmd = g_strdup_printf(
                "i=0; while [ $i -lt 20000 ]; do i=$((i+1)); "
                "echo \"the quick brown fox jumps over the lazy dog $i\"; done; "
                "date +%%s.%%N > %s", donefile);
        char *bargv[] = { (char*)"/bin/sh", (char*)"-c", bcmd, NULL };
        vte_terminal_spawn_async(b, VTE_PTY_DEFAULT, NULL, bargv, NULL,
                                 (GSpawnFlags)0, NULL, NULL, NULL, -1, NULL,
                                 on_spawn, NULL);

        gtk_window_set_default_size(GTK_WINDOW(win), 1000, 600);
        gtk_window_present(GTK_WINDOW(win));
}

int main(int argc, char **argv) {
        mode = argc > 1 ? argv[1] : "idle";
        donefile = argc > 2 ? argv[2] : "/tmp/fair_done";
        GtkApplication *app = gtk_application_new("org.vte.FairTest",
                                                  G_APPLICATION_NON_UNIQUE);
        g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
        return g_application_run(G_APPLICATION(app), 1, argv);
}
