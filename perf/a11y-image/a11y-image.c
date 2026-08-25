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

/* Assert that both text paths mark an image's position with U+FFFC: the
 * accessibility text so a reader can tell an image from blank space, and the
 * selection text so a copy carries the image's position too (vte#309).
 */
#include <vte/vte.h>
#include <gtk/gtk.h>
#include <string.h>

static VteTerminal *term;
static const char *sixfile;

static char *
a11y_text(void)
{
        AtkObject *acc = gtk_widget_get_accessible(GTK_WIDGET(term));
        if (acc == NULL || !ATK_IS_TEXT(acc))
                return NULL;

        return atk_text_get_text(ATK_TEXT(acc), 0, -1);
}

static gboolean check(gpointer u) {
        /* Select everything, which is what a copy would do. */
        vte_terminal_select_all(term);

        char *sel = vte_terminal_get_text_selected(term, VTE_FORMAT_TEXT);
        const char *ofc = "\xef\xbf\xbc";   /* U+FFFC in UTF-8 */

        gboolean sel_has = sel && strstr(sel, ofc) != NULL;

        char *a11y = a11y_text();
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
                (sel_has && a11y_has) ? "PASS" : "FAIL");

        g_free(sel); g_free(a11y);
        gtk_main_quit();
        return G_SOURCE_REMOVE;
}
static void on_spawn(VteTerminal *t, GPid p, GError *e, gpointer u) {}
static gboolean feed(gpointer d) {
        vte_terminal_feed_child(term, (const char*)d, -1);
        return G_SOURCE_REMOVE;
}
int main(int argc, char **argv) {
        sixfile = argc > 1 ? argv[1] : "/tmp/p.six";

        gtk_init(&argc, &argv);

        GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        term = VTE_TERMINAL(vte_terminal_new());
        vte_terminal_set_enable_sixel(term, TRUE);
        /* The a11y snapshot only refreshes when a11y is enabled. */
        vte_terminal_set_enable_a11y(term, TRUE);
        gtk_container_add(GTK_CONTAINER(win), GTK_WIDGET(term));
        char *argvv[] = { (char*)"/bin/sh", NULL };
        vte_terminal_spawn_async(term, VTE_PTY_DEFAULT, NULL, argvv, NULL,
                                 (GSpawnFlags)0, NULL, NULL, NULL, -1, NULL,
                                 on_spawn, NULL);
        char *cmd = g_strdup_printf("stty -echo; clear; printf 'ABOVE\\n'; cat %s; printf '\\nBELOW\\n'\n", sixfile);
        g_timeout_add(1500, feed, cmd);
        g_timeout_add(3000, feed, (char*)"printf 'TICK\\n'\n");
        g_timeout_add(4000, feed, (char*)"printf 'TICK2\\n'\n");
        g_timeout_add(6500, check, NULL);
        gtk_window_set_default_size(GTK_WINDOW(win), 900, 600);
        gtk_widget_show_all(win);

        gtk_main();
        return 0;
}
