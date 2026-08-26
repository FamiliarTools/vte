/*
 * Copyright © 2026 Guilherme Fontes
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 */

/* Hostile-input coverage for the SIXEL decoder.
 *
 * sixel-test.cc checks that well-formed sequences decode to the right pixels.
 * This file checks the opposite property, which nothing checked before: that
 * NO byte string at all, however malformed or hostile, can make the decoder
 * crash, read out of bounds, hang, or hand back an image that violates the
 * bounds the rest of the terminal relies on. The decoder is the widest
 * attacker-reachable surface in the tree, because every byte of every SIXEL a
 * terminal ever sees comes from whatever program is on the other end of the
 * pty - timg, chafa, a file manager's thumbnailer, or a `cat` of a binary
 * file that happened to contain a DCS introducer.
 *
 * There is no libFuzzer here on purpose: the toolchain this is built with has
 * no clang and gcc rejects -fsanitize=fuzzer, so a coverage-guided target
 * could not be built, let alone run in CI. What replaces it is a deterministic
 * replay harness, which is the better trade for this repository anyway: it
 * always terminates, it needs no corpus directory to be carried around, and a
 * failure reproduces from the printed seed rather than from a crash file. Run
 * the binary itself under ASan/UBSan to get the memory-safety half; the
 * assertions here are what catches the rest, and they hold either way.
 *
 * Four arms, deliberately different in kind:
 *
 *   corpus      - a checked-in table of inputs chosen to hit the shapes a
 *                 grammar-random generator reaches only by luck: a raster
 *                 attribute declaring a full-size canvas with no data at all,
 *                 repeat counts near INT_MAX, colour registers far out of
 *                 range, parameter lists past the eight-argument maximum.
 *   truncation  - every proper prefix of every corpus entry. A decoder is a
 *                 state machine fed by a pty, so it is cut at an arbitrary
 *                 offset constantly and every one of its states must survive
 *                 being the last one. Enumerating prefixes is exhaustive over
 *                 that, where picking a few cut points is not.
 *   bytes       - every one of the 256 byte values, in each position the
 *                 grammar distinguishes. This is written as a sweep rather
 *                 than as a list of the interesting ones so that it cannot
 *                 rot: a command byte added to the Command enum tomorrow is
 *                 already covered today, and no comment has to be kept in
 *                 sync with the enum.
 *   generative  - random sequences from a weighted grammar, then byte-level
 *                 mutation on top. Seeded from the GLib test RNG, so it is an
 *                 ordinary unit test that always terminates and replays
 *                 exactly with --seed=.
 *
 * Every arm ends in the same checker, check_decoded(), because the invariants
 * do not depend on how the bytes were produced.
 */

#include "config.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <glib.h>

#include "sixel-parser.hh"
#include "sixel-context.hh"

using namespace std::literals;

using Context = vte::sixel::Context;
using Status = vte::sixel::Parser::ParseStatus;
using Mode = vte::sixel::Parser::Mode;

namespace {

/* The decoder's own bounds, asked of the decoder rather than repeated here, so
 * that lowering VTE_SIXEL_MAX_WIDTH - which the SIXEL overlay does, to
 * 1024x1026, precisely to cap the DECGRA allocation - moves the assertions with
 * it instead of leaving them pinned to the upstream numbers. The constants
 * themselves are private, so this goes through the public accessors, which
 * needs an instance to ask.
 */
Context const&
decoder() noexcept
{
        static auto const c = Context{};
        return c;
}

auto const k_max_width = decoder().max_width();
auto const k_max_height = decoder().max_height();

/* One past the highest index param_to_color_register() can produce: the two
 * reserved registers plus the addressable ones. Any pen at or above this in
 * decoded scanline data would be an out-of-bounds read of Context::m_colors on
 * the very next line of image_data(), which is why the checker sweeps for it.
 */
auto const k_pen_limit = 2u + unsigned(decoder().num_colors());

/* An input held in an exactly-sized heap block.
 *
 * This is not the same thing as a std::string, and the difference is the whole
 * point: a std::string has spare capacity and a NUL terminator after it, so a
 * decoder that runs one byte past the end reads a byte that is really there
 * and nothing happens. A block from new[] has an ASan redzone immediately
 * after the last byte instead, so the same overrun is a fault with a stack
 * trace. Every arm funnels its bytes through here for that reason.
 */
class Input {
public:
        Input(void const* data, size_t size)
                : m_size{size},
                  m_data{std::make_unique<uint8_t[]>(size ? size : 1)}
        {
                if (size)
                        memcpy(m_data.get(), data, size);
        }

        explicit Input(std::string_view const& sv) : Input(sv.data(), sv.size()) { }

        auto begin() const noexcept { return m_data.get(); }
        auto end() const noexcept { return m_data.get() + m_size; }
        auto size() const noexcept { return m_size; }

private:
        size_t m_size;
        std::unique_ptr<uint8_t[]> m_data;
};

/* Render an input as a C-style escaped string, so that a failure names the
 * exact bytes that produced it and they can be pasted straight back into the
 * corpus table below.
 */
std::string
escape(Input const& input)
{
        auto out = std::string{};
        for (auto p = input.begin(); p != input.end(); ++p) {
                auto const c = *p;
                if (c == '\\' || c == '"') {
                        out += '\\';
                        out += char(c);
                } else if (c >= 0x20 && c < 0x7f) {
                        out += char(c);
                } else {
                        char buf[8];
                        g_snprintf(buf, sizeof(buf), "\\x%02x", c);
                        out += buf;
                }
        }
        return out;
}

/* The invariants. Every arm calls this and nothing else asserts.
 *
 * @context has already been prepared and parsed into; @status is what parse()
 * returned and @input is what produced it, carried only so that a failure can
 * print it.
 */
void
check_decoded(Context& context,
              Status status,
              Input const& input,
              char const* arm)
{
        /* 1. Parsing with eos set must always reach a terminal state. A
         * CONTINUE here would mean the decoder is waiting for more bytes that
         * are never coming, which upstack is a DCS that never closes and a
         * terminal that has stopped drawing.
         */
        if (status == Status::CONTINUE)
                g_error("%s: parse did not terminate on eos: \"%s\"",
                        arm, escape(input).c_str());

        /* 2. The reported image must fit the bounds every consumer of it
         * assumes. image-ref.hh sizes its cell-reference bitfields from these
         * two constants with a static_assert, and ring.cc rejects records
         * outside them, so a decoder that reported more would be handing the
         * ring a record it is required to drop, or worse.
         */
        auto const width = context.image_width();
        auto const height = context.image_height();
        if (width > k_max_width)
                g_error("%s: image_width %u > %u: \"%s\"",
                        arm, width, unsigned(k_max_width), escape(input).c_str());
        if (height > k_max_height)
                g_error("%s: image_height %u > %u: \"%s\"",
                        arm, height, unsigned(k_max_height), escape(input).c_str());

        /* 3. Materialise the image. Up to here the decode has only filled
         * scanlines; this is the step that allocates height * stride and walks
         * every scanline, so a bad offset or a short buffer shows up here and
         * not before. It is also the step whose cost is driven by the DECLARED
         * raster size rather than by how many bytes arrived, which is the
         * amplification the timing test below pins down.
         */
        auto size = size_t{0};
        auto* pixels = context.image_data_indexed(&size);
        if (!pixels) {
                /* A null return is a legitimate outcome for an empty or
                 * unallocatable image, but then it must be an empty one.
                 */
                if (width != 0 && height != 0 && size != 0)
                        g_error("%s: null pixels for %ux%u image: \"%s\"",
                                arm, width, height, escape(input).c_str());
                return;
        }

        /* 4. Every pen in the decoded image must be an addressable colour
         * register. Context::image_data() indexes m_colors[pen] with no check
         * at all, so a pen past the end of that array is a straight
         * out-of-bounds read in the drawing path - which is reached for every
         * image actually displayed, but never by the pixel-comparison tests,
         * because they all use in-range colours.
         */
        auto const count = size / sizeof(Context::color_index_t);
        for (auto i = size_t{0}; i < count; ++i) {
                if (pixels[i] >= k_pen_limit) {
                        auto const bad = unsigned(pixels[i]);
                        g_free(pixels);
                        g_error("%s: pen %u at %zu >= %u: \"%s\"",
                                arm, bad, i, k_pen_limit, escape(input).c_str());
                }
        }

        /* 5. The buffer really is as big as the size handed back. Reading the
         * last byte is a no-op with the sanitizer off and a bounds check with
         * it on, which is the cheapest way to catch a size that overstates the
         * allocation.
         */
        if (count > 0) {
                auto volatile last = pixels[count - 1];
                (void)last;
        }

        g_free(pixels);
}

/* Feed one input through a freshly prepared Context and check the result.
 *
 * @mode is carried through because the parser's handling of the high bit and
 * of C1 controls differs between the three, and a byte that is ignored in one
 * aborts the parse in another.
 */
void
run_one(Input const& input,
        char const* arm,
        Mode mode = Mode::UTF8,
        bool private_color_registers = true)
{
        auto context = Context{};
        context.set_mode(mode);
        context.reset();
        context.prepare(-1,             /* no ID */
                        0x50,           /* C0 DCS */
                        0xff, 0xff, 0xff,
                        false,          /* bg transparent */
                        private_color_registers);

        auto const [status, ip] = context.parse(input.begin(), input.end(), true);

        /* The returned pointer must lie inside the buffer it was given. A
         * decoder that hands back a position past the end would, upstack, make
         * vte resume the outer parser at an offset outside its own buffer.
         */
        if (ip < input.begin() || ip > input.end())
                g_error("%s: parse returned out-of-range position: \"%s\"",
                        arm, escape(input).c_str());

        check_decoded(context, status, input, arm);
}

void
run_one(std::string_view const& sv,
        char const* arm,
        Mode mode = Mode::UTF8)
{
        run_one(Input{sv}, arm, mode);
}

/* --- The checked-in corpus --- */

/* Inputs are the SIXEL body only: what a terminal hands the decoder after it
 * has recognised the DCS introducer. Terminators are part of the body and are
 * therefore written out, including the ones that are not ST.
 */
struct CorpusEntry {
        char const* name;
        std::string_view body;
};

CorpusEntry const k_corpus[] = {
        /* Degenerate and empty */
        {"empty",                   ""sv},
        {"st-only",                 "\x1b\\"sv},
        {"nul-only",                "\x00"sv},
        {"one-sixel",               "@\x1b\\"sv},

        /* DECGRA declaring a canvas with no data behind it. This is the CPU
         * and memory amplification the overlay lowers the maxima for: the
         * raster attributes alone force a buffer proportional to the DECLARED
         * size, and these inputs declare the largest one accepted while
         * carrying zero pixels.
         */
        {"raster-max-empty",        "\"1;1;99999;99999\x1b\\"sv},
        {"raster-max-one-sixel",    "\"1;1;99999;99999@\x1b\\"sv},
        {"raster-negative",         "\"-1;-1;-1;-1@\x1b\\"sv},
        {"raster-repeated",         "\"1;1;9999;9999\"1;1;9999;9999\"1;1;9999;9999@\x1b\\"sv},
        {"raster-after-data",       "@\"1;1;9999;9999@\x1b\\"sv},
        {"raster-zero",             "\"0;0;0;0@\x1b\\"sv},

        /* DECGRI, the other amplifier: a repeat count is a multiplier on a
         * single byte of input.
         */
        {"repeat-huge",             "!999999999@\x1b\\"sv},
        {"repeat-intmax",           "!2147483647@\x1b\\"sv},
        {"repeat-overflowing",      "!99999999999999999999@\x1b\\"sv},
        {"repeat-zero",             "!0@\x1b\\"sv},
        {"repeat-negative",         "!-5@\x1b\\"sv},
        {"repeat-no-data",          "!9999\x1b\\"sv},
        {"repeat-then-command",     "!9999$!9999-!9999#1\x1b\\"sv},
        {"repeat-chain",            "!100!100!100!100@\x1b\\"sv},
        {"repeat-across-lines",     "!9999@-!9999@-!9999@\x1b\\"sv},

        /* DECGCI, colour registers. param_to_color_register() masks rather
         * than clamps, so these check the mask holds for values it was never
         * meant to see.
         */
        {"color-out-of-range",      "#99999;2;100;100;100@\x1b\\"sv},
        {"color-negative",          "#-1;2;100;100;100@\x1b\\"sv},
        {"color-intmax",            "#2147483647;2;100;100;100@\x1b\\"sv},
        {"color-select-only",       "#5@\x1b\\"sv},
        {"color-bad-space",         "#1;9;100;100;100@\x1b\\"sv},
        {"color-hls",               "#1;1;360;100;100@\x1b\\"sv},
        {"color-hls-out-of-range",  "#1;1;99999;99999;99999@\x1b\\"sv},
        {"color-rgb-out-of-range",  "#1;2;9999;9999;9999@\x1b\\"sv},
        {"color-negative-channels", "#1;2;-1;-1;-1@\x1b\\"sv},
        {"color-no-params",         "#@\x1b\\"sv},
        {"color-trailing-sep",      "#1;@\x1b\\"sv},

        /* Parameter machinery: the eight-argument maximum, the reserved
         * subparameter separator, and separators with nothing around them.
         */
        {"params-overflow",         "#1;2;3;4;5;6;7;8;9;10@\x1b\\"sv},
        {"params-way-overflow",     "\"1;2;3;4;5;6;7;8;9;10;11;12;13;14;15;16@\x1b\\"sv},
        {"params-subparam",         "#1:2;3@\x1b\\"sv},
        {"params-only-seps",        "#;;;;;;;;;;@\x1b\\"sv},
        {"params-huge-digits",      "#111111111111111111111111111111@\x1b\\"sv},
        {"params-leading-zeros",    "#0000000000000000001@\x1b\\"sv},

        /* Geometry churn: DECGCR and DECGNL are what move the write cursor,
         * and a storm of either is how a stream reaches the height and width
         * maxima without carrying much data.
         */
        {"nl-storm",                "@---------------------------------------@\x1b\\"sv},
        {"cr-storm",                "@$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$@\x1b\\"sv},
        {"nl-past-max-height",      "!400-!400-!400-!400-!400-!400-!400-@\x1b\\"sv},
        {"cr-nl-interleaved",       "@$-@$-@$-@$-@$-@\x1b\\"sv},
        {"home",                    "@+@+@\x1b\\"sv},

        /* Terminations, including the ones that are not ST. ESC anywhere in
         * the body is a cancel unless the next byte is a backslash, and CAN
         * and SUB abort outright.
         */
        {"esc-mid-sequence",        "\"1;1;100;100\x1b@\x1b\\"sv},
        {"esc-in-params",          "#1;2;\x1b\x1b\\"sv},
        {"esc-at-end",              "@\x1b"sv},
        {"esc-esc",                 "@\x1b\x1b\\"sv},
        {"can",                     "@\x18@\x1b\\"sv},
        {"sub",                     "@\x1a@\x1b\\"sv},
        {"can-at-end",              "@\x18"sv},
        {"no-terminator",           "\"1;1;100;100#1!100@"sv},
        {"c1-st",                   "@\xc2\x9c"sv},
        {"c1-other",                "@\xc2\x90@"sv},
        {"lone-c2",                 "@\xc2"sv},
        {"c2-then-ascii",           "@\xc2@\x1b\\"sv},

        /* Control characters interleaved through the data, which is what a
         * `cat` of a binary file produces once a DCS introducer has been seen.
         */
        {"c0-interleaved",          "@\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d@\x1b\\"sv},
        {"nul-interleaved",         "@\x00@\x00@\x1b\\"sv},
        {"del-interleaved",         "@\x7f@\x1b\\"sv},
        {"high-bit-bytes",          "@\x80\x81\xfe\xff@\x1b\\"sv},
        {"all-high-bits",           "\xff\xfe\xfd\xfc\xfb\xfa\xf9\xf8\x1b\\"sv},

        /* Reserved commands, which the parser must ignore rather than
         * dispatch. Written as the whole reserved span so that the entry does
         * not have to be revisited if one of them is ever assigned.
         */
        {"reserved-commands",       "%&'()*,./<=>@\x1b\\"sv},
        {"reserved-with-params",    "%1;2;3&4;5;6@\x1b\\"sv},

        /* Shapes that mix the amplifiers with the terminators, which is where
         * a decoder that allocates before it validates tends to fail.
         */
        {"raster-then-cancel",      "\"1;1;99999;99999\x18"sv},
        {"raster-then-eof",         "\"1;1;99999;99999"sv},
        {"repeat-then-cancel",      "!999999999\x18"sv},
        {"everything",              "\"1;1;9999;9999#1;2;100;0;0!500@$-#2;2;0;100;0!500A-\x1b\\"sv},
};

/* --- Arms --- */

void
test_corpus(void)
{
        for (auto const& entry : k_corpus) {
                for (auto mode : {Mode::UTF8, Mode::EIGHTBIT, Mode::SEVENBIT})
                        run_one(entry.body, entry.name, mode);
        }

        /* The corpus is the deterministic half of this file and its value is
         * proportional to how many shapes it holds, so guard against it being
         * quietly emptied.
         */
        g_assert_cmpuint(G_N_ELEMENTS(k_corpus), >=, 60);
}

void
test_truncation(void)
{
        /* Every proper prefix of every corpus entry. A decoder fed by a pty is
         * cut at an arbitrary offset on every read, so each of its states has
         * to survive being the last one; enumerating the prefixes is
         * exhaustive over the cut points, where choosing a few is not.
         */
        auto n = guint64{0};
        for (auto const& entry : k_corpus) {
                for (auto len = size_t{0}; len < entry.body.size(); ++len) {
                        run_one(Input{entry.body.data(), len}, entry.name);
                        ++n;
                }
        }

        g_test_message("truncation: %" G_GUINT64_FORMAT " prefixes", n);
        g_assert_cmpuint(n, >, 900);
}

void
test_every_byte(void)
{
        /* Every byte value in every position the grammar distinguishes.
         *
         * This is a sweep and not a list of the interesting values on purpose:
         * a list would be a closed enumeration of a set defined elsewhere (the
         * Command enum in sixel-parser.hh, the C0 and C1 ranges), and it would
         * go stale silently the first time that set changed. 256 values times
         * a handful of positions is cheap enough that there is no reason to
         * choose.
         */
        auto n = guint64{0};
        for (auto b = 0u; b < 256u; ++b) {
                auto const c = char(b);

                /* Alone; in command position; between parameters; among data;
                 * doubled; and after a raster attribute that has already
                 * forced an allocation.
                 */
                std::string const positions[] = {
                        std::string{c},
                        std::string{c} + "1;2;3@\x1b\\",
                        std::string{"#1;"} + c + ";2@\x1b\\",
                        std::string{"@"} + c + "@\x1b\\",
                        std::string{c} + c + "\x1b\\",
                        std::string{"\"1;1;64;64"} + c + "@\x1b\\",
                        std::string{"!8"} + c + "\x1b\\",
                };

                for (auto const& s : positions) {
                        for (auto mode : {Mode::UTF8, Mode::EIGHTBIT, Mode::SEVENBIT}) {
                                run_one(Input{s.data(), s.size()}, "every-byte", mode);
                                ++n;
                        }
                }
        }

        g_test_message("every-byte: %" G_GUINT64_FORMAT " inputs", n);
        g_assert_cmpuint(n, ==, 256 * 7 * 3);
}

/* --- Generative arm --- */

/* The default is sized to keep the whole binary well under a second, because a
 * test nobody waits for is a test that gets skipped. The exhaustive run is the
 * same code with the count turned up, behind an environment variable rather
 * than behind a second binary, so that what CI runs and what a long hunt runs
 * cannot drift apart.
 */
int const k_gen_sequences_default = 256;
int const k_gen_max_ops = 64;

int
gen_sequences(void)
{
        static auto const n = [] {
                auto const* env = g_getenv("VTE_SIXEL_CORPUS_SEQUENCES");
                if (!env)
                        return k_gen_sequences_default;
                auto const v = g_ascii_strtoll(env, nullptr, 10);
                return (v > 0 && v < (1 << 24)) ? int(v) : k_gen_sequences_default;
        }();
        return n;
}

enum GenOp {
        GEN_SIXEL_RUN,
        GEN_REPEAT,
        GEN_COLOR_SELECT,
        GEN_COLOR_DEFINE,
        GEN_RASTER,
        GEN_CR,
        GEN_NL,
        GEN_HOME,
        GEN_RESERVED,
        GEN_CONTROL,
        GEN_JUNK_PARAMS,
        GEN_RAW_BYTES,
        GEN_OP_COUNT
};

char const* const k_gen_op_names[GEN_OP_COUNT] = {
        "sixel-run", "repeat", "color-select", "color-define", "raster",
        "cr", "nl", "home", "reserved", "control", "junk-params", "raw-bytes",
};

/* Weighted so that the stream still looks mostly like a picture. An unweighted
 * draw makes almost every sequence abort on a control character within a few
 * bytes, which reaches the abort path over and over and the decode path
 * essentially never - every assertion would pass while checking nothing.
 */
int const k_gen_op_weights[GEN_OP_COUNT] = {
        30, 14, 10, 10, 6, 8, 8, 2, 3, 4, 3, 2,
};

int
draw_op(void)
{
        auto total = 0;
        for (auto w : k_gen_op_weights)
                total += w;

        auto r = g_test_rand_int_range(0, total);
        for (auto i = 0; i < GEN_OP_COUNT; ++i) {
                r -= k_gen_op_weights[i];
                if (r < 0)
                        return i;
        }
        return GEN_SIXEL_RUN;
}

/* A parameter value, drawn from a distribution that spends most of its mass on
 * the edges rather than uniformly over the range. Uniform draws over int would
 * essentially never produce 0, 1, or a value one either side of a maximum, and
 * those are where off-by-one bounds bugs live.
 */
int
draw_param(void)
{
        switch (g_test_rand_int_range(0, 10)) {
        case 0: return 0;
        case 1: return 1;
        case 2: return -1;
        case 3: return g_test_rand_int_range(0, 8);
        case 4: return int(k_max_width) + g_test_rand_int_range(-2, 3);
        case 5: return int(k_max_height) + g_test_rand_int_range(-2, 3);
        case 6: return int(decoder().num_colors()) + g_test_rand_int_range(-2, 3);
        case 7: return G_MAXINT;
        case 8: return G_MAXINT - g_test_rand_int_range(0, 4);
        default: return g_test_rand_int_range(0, 100000);
        }
}

void
append_params(std::string& out)
{
        auto const n = g_test_rand_int_range(0, 11);
        for (auto i = 0; i < n; ++i) {
                if (i)
                        out += (g_test_rand_int_range(0, 8) == 0) ? ':' : ';';
                if (g_test_rand_int_range(0, 6) != 0) {
                        char buf[24];
                        g_snprintf(buf, sizeof(buf), "%d", draw_param());
                        out += buf;
                }
        }
}

std::string
generate(void)
{
        auto out = std::string{};
        auto const ops = g_test_rand_int_range(1, k_gen_max_ops + 1);

        for (auto i = 0; i < ops; ++i) {
                switch (draw_op()) {
                case GEN_SIXEL_RUN: {
                        auto const n = g_test_rand_int_range(1, 24);
                        for (auto j = 0; j < n; ++j)
                                out += char(0x3f + g_test_rand_int_range(0, 64));
                        break;
                }
                case GEN_REPEAT:
                        out += '!';
                        append_params(out);
                        if (g_test_rand_int_range(0, 4) != 0)
                                out += char(0x3f + g_test_rand_int_range(0, 64));
                        break;
                case GEN_COLOR_SELECT:
                        out += '#';
                        append_params(out);
                        break;
                case GEN_COLOR_DEFINE: {
                        char buf[64];
                        g_snprintf(buf, sizeof(buf), "#%d;%d;%d;%d;%d",
                                   draw_param(), g_test_rand_int_range(0, 4),
                                   draw_param(), draw_param(), draw_param());
                        out += buf;
                        break;
                }
                case GEN_RASTER:
                        out += '"';
                        append_params(out);
                        break;
                case GEN_CR:    out += '$'; break;
                case GEN_NL:    out += '-'; break;
                case GEN_HOME:  out += '+'; break;
                case GEN_RESERVED:
                        /* The reserved command span, swept rather than listed
                         * so that a newly assigned command is covered without
                         * this having to be edited.
                         */
                        out += char(0x25 + g_test_rand_int_range(0, 11));
                        append_params(out);
                        break;
                case GEN_CONTROL:
                        out += char(g_test_rand_int_range(0, 0x20));
                        break;
                case GEN_JUNK_PARAMS:
                        append_params(out);
                        break;
                case GEN_RAW_BYTES: {
                        auto const n = g_test_rand_int_range(1, 8);
                        for (auto j = 0; j < n; ++j)
                                out += char(g_test_rand_int_range(0, 256));
                        break;
                }
                }
        }

        /* Most sequences get a terminator, some deliberately do not: an
         * unterminated body is exactly what arrives when the writer is killed
         * mid-image, and it has to reach a terminal state on eos anyway.
         */
        switch (g_test_rand_int_range(0, 8)) {
        case 0: break;
        case 1: out += '\x18'; break;
        case 2: out += '\x1a'; break;
        case 3: out += "\xc2\x9c"; break;
        default: out += "\x1b\\"; break;
        }

        return out;
}

/* Byte-level mutation on top of the grammar. The grammar can only produce
 * things it knows how to say; mutation is what reaches the bytes it would
 * never emit, and it is where truncation mid-multibyte and flipped high bits
 * come from.
 */
void
mutate(std::string& s)
{
        auto const n = g_test_rand_int_range(0, 5);
        for (auto i = 0; i < n && !s.empty(); ++i) {
                switch (g_test_rand_int_range(0, 5)) {
                case 0: /* bit flip */
                        s[g_test_rand_int_range(0, int(s.size()))] ^=
                                char(1u << g_test_rand_int_range(0, 8));
                        break;
                case 1: /* byte replace */
                        s[g_test_rand_int_range(0, int(s.size()))] =
                                char(g_test_rand_int_range(0, 256));
                        break;
                case 2: /* truncate */
                        s.resize(size_t(g_test_rand_int_range(0, int(s.size()))));
                        break;
                case 3: /* splice in an ESC, which is a cancel */
                        s.insert(size_t(g_test_rand_int_range(0, int(s.size()))), 1, '\x1b');
                        break;
                case 4: { /* duplicate a span, which is how repeats stack up */
                        auto const pos = size_t(g_test_rand_int_range(0, int(s.size())));
                        auto const len = std::min(s.size() - pos, size_t(g_test_rand_int_range(1, 17)));
                        s.insert(pos, s.substr(pos, len));
                        break;
                }
                }
        }
}

void
test_generative(void)
{
        auto total_bytes = guint64{0};
        auto decoded = guint64{0};

        auto const n_sequences = gen_sequences();
        for (auto seq = 0; seq < n_sequences; ++seq) {
                auto s = generate();
                mutate(s);
                total_bytes += s.size();

                auto const mode = (g_test_rand_int_range(0, 8) == 0)
                        ? Mode::EIGHTBIT
                        : ((g_test_rand_int_range(0, 8) == 0) ? Mode::SEVENBIT : Mode::UTF8);

                auto input = Input{s.data(), s.size()};

                /* Run it, and separately count whether it produced a real
                 * image, so that the coverage claim below is measured rather
                 * than assumed.
                 */
                auto context = Context{};
                context.set_mode(mode);
                context.reset();
                context.prepare(-1, 0x50, 0xff, 0xff, 0xff, false, true);
                auto const [status, ip] = context.parse(input.begin(), input.end(), true);
                if (ip < input.begin() || ip > input.end())
                        g_error("generative: parse returned out-of-range position: \"%s\"",
                                escape(input).c_str());
                if (context.image_width() > 0 && context.image_height() > 0)
                        ++decoded;
                check_decoded(context, status, input, "generative");

                /* Re-run the same bytes through a Context that has already
                 * decoded something, without a reset in between, since a
                 * terminal reuses one Context for every image on the screen
                 * and shared colour registers make the second image depend on
                 * the first.
                 */
                run_one(input, "generative-reuse", mode, false /* shared registers */);
        }

        g_test_message("generative: %d sequences, %" G_GUINT64_FORMAT " bytes, "
                       "%" G_GUINT64_FORMAT " produced a non-empty image",
                       n_sequences, total_bytes, decoded);

        /* The arm is worthless if the inputs all abort before painting
         * anything: every assertion would pass while the decode path was never
         * entered. Assert the coverage outright rather than trusting the
         * weights to have kept working.
         */
        g_assert_cmpuint(decoded, >, guint64(n_sequences / 4));
}

/* --- Amplification --- */

void
test_no_amplification(void)
{
        /* DECGRA forces an allocation and a walk proportional to the DECLARED
         * raster size, not to how many bytes actually arrived. That is a real
         * amplification factor - a few dozen bytes of input against a canvas
         * of millions of pixels - and it is why the SIXEL overlay lowers the
         * maxima. What has to hold is that the factor is BOUNDED: capped by
         * the maxima, so that the worst input costs a fixed amount rather than
         * an amount the sender chooses.
         *
         * This is a bound, not a benchmark. It is set well above the measured
         * cost so that it does not flake on a loaded machine, and it is here
         * to catch a change that removes the cap entirely - which would not be
         * slower by a factor of two, it would not finish.
         */
        static char const* const inputs[] = {
                "\"1;1;99999;99999\x1b\\",
                "\"1;1;99999;99999@\x1b\\",
                "!999999999@\x1b\\",
                "!2147483647@\x1b\\",
                "\"1;1;99999;99999!999999999@\x1b\\",
        };

        for (auto const* s : inputs) {
                auto timer = g_timer_new();
                run_one(std::string_view{s}, "amplification");
                auto const elapsed = g_timer_elapsed(timer, nullptr);
                g_timer_destroy(timer);

                g_test_message("amplification: %.4fs for \"%s\"",
                               elapsed, escape(Input{std::string_view{s}}).c_str());
                g_assert_cmpfloat(elapsed, <, 5.0);
        }
}

} // anon namespace

int
main(int argc,
     char* argv[])
{
        g_test_init(&argc, &argv, nullptr);

        g_test_add_func("/vte/sixel/corpus/table", test_corpus);
        g_test_add_func("/vte/sixel/corpus/truncation", test_truncation);
        g_test_add_func("/vte/sixel/corpus/every-byte", test_every_byte);
        g_test_add_func("/vte/sixel/corpus/generative", test_generative);
        g_test_add_func("/vte/sixel/corpus/amplification", test_no_amplification);

        return g_test_run();
}
