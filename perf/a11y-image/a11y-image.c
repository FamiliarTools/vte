/*
 * Copyright © 2026 Guilherme Fontes
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/* Assert the two text paths disagree in exactly the intended way:
 * the accessibility text marks an image with U+FFFC; the clipboard text
 * does not contain it at all.
 */
#include <vte/vte.h>
#include <gtk/gtk.h>
#include <string.h>

static VteTerminal *term;
static const char *sixfile;

static gboolean check(gpointer u) {
        /* Select everything, which is what a copy would do. */
        vte_terminal_select_all(term);

        char *sel = vte_terminal_get_text_selected(term, VTE_FORMAT_TEXT);
        const char *ofc = "\xef\xbf\xbc";   /* U+FFFC in UTF-8 */

        gboolean sel_has = sel && strstr(sel, ofc) != NULL;

        /* The accessibility text comes through GtkAccessibleText. */
        char *a11y = NULL;
        if (GTK_IS_ACCESSIBLE_TEXT(term)) {
                /* get_contents is an interface vfunc, not a public call. */
                GtkAccessibleTextInterface *iface =
                        GTK_ACCESSIBLE_TEXT_GET_IFACE(GTK_ACCESSIBLE_TEXT(term));
                GBytes *b = iface->get_contents
                        ? iface->get_contents(GTK_ACCESSIBLE_TEXT(term), 0, G_MAXUINT)
                        : NULL;
                if (b) {
                        gsize n = 0;
                        const char *d = (const char*)g_bytes_get_data(b, &n);
                        a11y = g_strndup(d, n);
                        g_bytes_unref(b);
                }
        }
        gboolean a11y_has = a11y && strstr(a11y, ofc) != NULL;

        g_print("SEL_LEN=%d SEL_HAS_ABOVE=%s\n",
                sel ? (int)strlen(sel) : -1,
                (sel && strstr(sel, "ABOVE")) ? "yes" : "no");
        g_print("A11Y_LEN=%d A11Y_HAS_ABOVE=%s\n",
                a11y ? (int)strlen(a11y) : -1,
                (a11y && strstr(a11y, "ABOVE")) ? "yes" : "no");
        g_print("CLIPBOARD_HAS_UFFFC=%s\n", sel_has ? "yes" : "no");
        g_print("A11Y_HAS_UFFFC=%s\n", a11y_has ? "yes" : "no");
        g_print("VERDICT=%s\n",
                (!sel_has && a11y_has) ? "PASS" : "FAIL");

        g_free(sel); g_free(a11y);
        g_application_quit(g_application_get_default());
        return G_SOURCE_REMOVE;
}
static void on_spawn(VteTerminal *t, GPid p, GError *e, gpointer u) {}
static void activate(GtkApplication *app, gpointer u) {
        GtkWidget *win = gtk_application_window_new(app);
        term = VTE_TERMINAL(vte_terminal_new());
        vte_terminal_set_enable_sixel(term, TRUE);
        /* The a11y snapshot only refreshes when a11y is enabled. */
        vte_terminal_set_enable_a11y(term, TRUE);
        gtk_window_set_child(GTK_WINDOW(win), GTK_WIDGET(term));
        char *argv[] = { (char*)"/bin/sh", NULL };
        vte_terminal_spawn_async(term, VTE_PTY_DEFAULT, NULL, argv, NULL,
                                 (GSpawnFlags)0, NULL, NULL, NULL, -1, NULL,
                                 on_spawn, NULL);
        char *cmd = g_strdup_printf("stty -echo; clear; printf 'ABOVE\\n'; cat %s; printf '\\nBELOW\\n'\n", sixfile);
        g_timeout_add(1500, (GSourceFunc)[](gpointer d) -> gboolean {
                vte_terminal_feed_child(term, (const char*)d, -1);
                return G_SOURCE_REMOVE; }, cmd);
        g_timeout_add(3000, (GSourceFunc)[](gpointer) -> gboolean {
                vte_terminal_feed_child(term, "printf 'TICK\\n'\n", -1);
                return G_SOURCE_REMOVE; }, NULL);
        g_timeout_add(4000, (GSourceFunc)[](gpointer) -> gboolean {
                vte_terminal_feed_child(term, "printf 'TICK2\\n'\n", -1);
                return G_SOURCE_REMOVE; }, NULL);
        g_timeout_add(6500, check, NULL);
        gtk_window_set_default_size(GTK_WINDOW(win), 900, 600);
        gtk_window_present(GTK_WINDOW(win));
}
int main(int argc, char **argv) {
        sixfile = argc > 1 ? argv[1] : "/tmp/p.six";
        GtkApplication *app = gtk_application_new("org.vte.A11yTest",
                                                  G_APPLICATION_NON_UNIQUE);
        g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
        return g_application_run(G_APPLICATION(app), 1, argv);
}
