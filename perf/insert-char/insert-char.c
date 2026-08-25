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

/* One VteTerminal, one large fed buffer, and a CPU-time number for the parse.
 *
 * The quantity under measurement is the per-character cost of the image choke
 * point in Terminal::insert_char(). The bulk text path,
 * insert_single_width_chars(), calls the choke point once per RUN, so only the
 * characters that fall back to insert_char() - wide ones, and combining marks -
 * can pay per character at all. This feeds each of those populations on its
 * own so the arms can be compared with the call compiled out.
 *
 * process_incoming() drains the whole incoming queue in a single call, so the
 * entire payload is parsed inside one frame-clock tick and the process burns
 * no CPU waiting. That makes CLOCK_PROCESS_CPUTIME_ID across the flood a
 * measure of the parse and nothing else, and it is reported separately from
 * the process total, which also carries GTK startup.
 */

#include <vte/vte.h>
#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Feeding this OSC is the last thing the payload does, and nothing else in it
 * sets a title, so the signal fires exactly once, when the parse has reached
 * the end of the buffer.
 */
#define MARKER "\033]0;done\a"

static const char *mode;
static long n_chars;
static GString *payload;
static double flood_cpu_start;
static gboolean flooding;
static FILE *perf_ctl;
static FILE *perf_ack;

static double cpu_seconds(void) {
        struct timespec ts;
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
        return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Counting the whole process would charge the parse for GTK startup and for
 * building the payload, both of which grow with the character count and so do
 * not cancel out of a per-character figure. perf(1) can be told to count only
 * a window: run it with --control and gate the counters on the flood.
 */
static void perf_control(const char *cmd) {
        char ack[16];

        if (perf_ctl == NULL)
                return;

        fprintf(perf_ctl, "%s\n", cmd);
        fflush(perf_ctl);
        if (perf_ack != NULL && fgets(ack, sizeof ack, perf_ack) == NULL)
                g_printerr("perf did not acknowledge \"%s\"\n", cmd);
}

static void perf_control_open(void) {
        const char *ctl = g_getenv("VTE_PERF_CTL_FIFO");
        const char *ack = g_getenv("VTE_PERF_ACK_FIFO");

        if (ctl == NULL)
                return;

        perf_ctl = fopen(ctl, "w");
        if (perf_ctl == NULL) {
                g_printerr("cannot open %s\n", ctl);
                return;
        }
        if (ack != NULL)
                perf_ack = fopen(ack, "r");
}

/* The three populations. "ascii" is the control: it is the one that goes
 * through insert_single_width_chars(), so a difference between the arms there
 * would mean the measurement is picking up something other than the call.
 */
static void append_char(GString *s, long i) {
        if (g_strcmp0(mode, "ascii") == 0) {
                g_string_append_c(s, (char)('a' + (i % 26)));
        } else if (g_strcmp0(mode, "combining") == 0) {
                /* A base letter plus COMBINING ACUTE ACCENT, which is the
                 * branch of insert_char() that walks back over the base cell.
                 */
                g_string_append_c(s, (char)('a' + (i % 26)));
                g_string_append_unichar(s, 0x0301);
        } else {
                /* CJK unified ideographs, all double width. */
                g_string_append_unichar(s, 0x4e00 + (gunichar)(i % 4096));
        }
}

/* Columns one payload character occupies, which is also how many of them fit
 * on a line. The fixture check below proves this against the widget rather
 * than trusting it.
 */
static int char_columns(void) {
        return g_strcmp0(mode, "cjk") == 0 ? 2 : 1;
}

static GString *build_payload(long count, int columns_per_line) {
        GString *s = g_string_sized_new((gsize)count * 4);
        int per_line = columns_per_line / char_columns();

        for (long i = 0; i < count; i++) {
                append_char(s, i);
                if ((i + 1) % per_line == 0)
                        g_string_append(s, "\r\n");
        }
        return s;
}

/* The fixture check. A payload that is not actually double width would be fed
 * through insert_single_width_chars() and the run would silently measure the
 * wrong path, so ask the widget where the cursor ended up after a known number
 * of characters and refuse to report a number if it disagrees.
 */
static gboolean fixture_tick(gpointer u) {
        return G_SOURCE_CONTINUE;
}

static gboolean fixture_ok(VteTerminal *t) {
        const int probe = 10;
        GString *s = g_string_new(NULL);
        glong col = 0, row = 0;
        gint64 deadline;
        guint tick;

        vte_terminal_feed(t, "\033[H\033[2J", -1);
        for (long i = 0; i < probe; i++)
                append_char(s, i);
        vte_terminal_feed(t, s->str, (gssize)s->len);
        g_string_free(s, TRUE);

        /* The parse runs off the frame clock, so this has to wait for a frame
         * rather than merely drain what is already pending. The tick source is
         * what makes the deadline reachable: without a source that keeps
         * firing, the iteration below blocks for good once the cursor has
         * stopped moving, and a failing fixture would hang instead of saying so.
         */
        tick = g_timeout_add(10, fixture_tick, NULL);
        deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
        do {
                vte_terminal_get_cursor_position(t, &col, &row);
                if (col == probe * char_columns())
                        break;
                g_main_context_iteration(NULL, TRUE);
        } while (g_get_monotonic_time() < deadline);
        g_source_remove(tick);

        if (col != probe * char_columns()) {
                g_printerr("FIXTURE FAILED: %s payload put the cursor at column %ld "
                           "after %d characters, expected %d\n",
                           mode, col, probe, probe * char_columns());
                return FALSE;
        }
        vte_terminal_feed(t, "\033[H\033[2J", -1);
        return TRUE;
}

static void on_title_changed(VteTerminal *t, gpointer u) {
        double flood, total;

        if (!flooding)
                return;
        flooding = FALSE;
        perf_control("disable");

        flood = cpu_seconds() - flood_cpu_start;
        total = cpu_seconds();

        g_print("%s\t%ld chars\tflood %.4f s cpu\ttotal %.4f s cpu\t%.2f Mchar/s\n",
                mode, n_chars, flood, total, (double)n_chars / flood / 1e6);
        gtk_main_quit();
}

static gboolean start_flood(gpointer data) {
        VteTerminal *t = VTE_TERMINAL(data);

        if (!fixture_ok(t)) {
                gtk_main_quit();
                exit(1);
        }

        flooding = TRUE;
        perf_control("enable");
        flood_cpu_start = cpu_seconds();
        vte_terminal_feed(t, payload->str, (gssize)payload->len);
        vte_terminal_feed(t, MARKER, -1);
        return G_SOURCE_REMOVE;
}

int main(int argc, char **argv) {
        GtkWidget *win;
        VteTerminal *t;

        mode = argc > 1 ? argv[1] : "cjk";
        n_chars = argc > 2 ? atol(argv[2]) : 4000000;

        if (g_strcmp0(mode, "cjk") != 0 &&
            g_strcmp0(mode, "ascii") != 0 &&
            g_strcmp0(mode, "combining") != 0) {
                g_printerr("usage: %s <cjk|ascii|combining> [chars]\n", argv[0]);
                return 1;
        }

        perf_control_open();
        gtk_init(&argc, &argv);

        win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        t = VTE_TERMINAL(vte_terminal_new());
        gtk_container_add(GTK_CONTAINER(win), GTK_WIDGET(t));
        gtk_window_set_default_size(GTK_WINDOW(win), 800, 400);
        g_signal_connect(t, "window-title-changed",
                         G_CALLBACK(on_title_changed), NULL);
        gtk_widget_show_all(win);

        payload = build_payload(n_chars, vte_terminal_get_column_count(t));

        /* The scheduler runs off the widget's frame clock, so the flood cannot
         * start before the window is on screen.
         */
        g_timeout_add(500, start_flood, t);
        gtk_main();

        g_string_free(payload, TRUE);
        return 0;
}
