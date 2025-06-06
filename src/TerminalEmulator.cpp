// The MIT License (MIT)

// Copyright (c) 2020 Fredrik A. Kristiansen
// Copyright (c) 2014 - 2020 Hiltjo Posthuma<hiltjo at codemadness dot org>
// Copyright (c) 2018 Devin J.Pohly<djpohly at gmail dot com>
// Copyright (c) 2014 - 2017 Quentin Rameau<quinq at fifth dot space>
// Copyright (c) 2009 - 2012 Aurélien APTEL<aurelien dot aptel at gmail dot com>
// Copyright (c) 2008 - 2017 Anselm R Garbe<garbeam at gmail dot com>
// Copyright (c) 2012 - 2017 Roberto E.Vargas Caballero<k0ga at shike2 dot com>
// Copyright (c) 2012 - 2016 Christoph Lohmann<20h at r - 36 dot net>
// Copyright (c) 2013 Eon S.Jeon<esjeon at hyunmu dot am>
// Copyright (c) 2013 Alexander Sedov<alex0player at gmail dot com>
// Copyright (c) 2013 Mark Edgar<medgar123 at gmail dot com>
// Copyright (c) 2013 - 2014 Eric Pruitt<eric.pruitt at gmail dot com>
// Copyright (c) 2013 Michael Forney<mforney at mforney dot org>
// Copyright (c) 2013 - 2014 Markus Teich<markus dot teich at stusta dot mhn dot de>
// Copyright (c) 2014 - 2015 Laslo Hunhold<dev at frign dot de>

//  Permission is hereby granted, free of charge, to any person obtaining a
//  copy of this software and associated documentation files (the "Software"),
//  to deal in the Software without restriction, including without limitation
//  the rights to use, copy, modify, merge, publish, distribute, sublicense,
//  and/or sell copies of the Software, and to permit persons to whom the
//  Software is furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in
//  all copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
//  OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
//  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
//  DEALINGS IN THE SOFTWARE.
#include "Hexe/Terminal/TerminalEmulator.h"
#include "boxdraw_data.h"
#include "emoji_blocks.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include "config.def.h"
#include <cmath>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) < (b) ? (b) : (a))
#define LEN(a) (sizeof(a) / sizeof(a)[0])
#define BETWEEN(x, a, b) ((a) <= (x) && (x) <= (b))
#define DIVCEIL(n, d) (((n) + ((d)-1)) / (d))
#define DEFAULT(a, b) (a) = (a) ? (a) : (b)
#define LIMIT(x, a, b) (x) = (x) < (a) ? (a) : (x) > (b) ? (b) : (x)
#define ATTRCMP(a, b) ((a).mode != (b).mode || (a).fg != (b).fg || \
                       (a).bg != (b).bg)
#define TIMEDIFF(t1, t2) ((t1.tv_sec - t2.tv_sec) * 1000 + \
                          (t1.tv_nsec - t2.tv_nsec) / 1E6)
#define MODBIT(x, set, bit) ((set) ? ((x) |= (bit)) : ((x) &= ~(bit)))

#define TRUECOLOR(r, g, b) (1 << 24 | (r) << 16 | (g) << 8 | (b))
#define IS_TRUECOL(x) (1 << 24 & (x))

/* Arbitrary sizes */
#define UTF_INVALID 0xFFFD
#define UTF_SIZ 4

/* macros */
#define IS_SET(flag) ((term.mode & (flag)) != 0)
#define ISCONTROLC0(c) (BETWEEN(c, 0, 0x1f) || (c) == 0x7f)
#define ISCONTROLC1(c) (BETWEEN(c, 0x80, 0x9f))
#define ISCONTROL(c) (ISCONTROLC0(c) || ISCONTROLC1(c))
#define ISDELIM(u) (u && Hexe_wcschr(worddelimiters, u))

using namespace Hexe::Terminal;

static inline bool is_emoji_presentation(int32_t code)
{
    const emoji_range_t **curr = emoji_ranges;
    while (*curr != 0)
    {
        if (code >= (*curr)->min_code && code <= (*curr)->max_code)
        {
            int ofs = code - (*curr)->min_code;

            int byteOffset = ofs / 8;
            int bit = 1 << (ofs % 8);
            auto byte = (*curr)->bitmap[byteOffset];

            return ((*curr)->bitmap[byteOffset] & bit) == bit;
        }
        ++curr;
    }
    return false;
}

static inline bool
is_emoji(int32_t code)
{
    if (is_emoji_presentation(code))
        return true;
    return false;
}

static size_t Hexe_wcslen(const Rune *s)
{
    const Rune *a;
    for (a = s; *s; s++)
        ;
    return s - a;
}

static Rune *
Hexe_wcschr(const Rune *s, Rune c)
{
    if (!c)
        return (Rune *)s + Hexe_wcslen(s);
    for (; *s && *s != c; s++)
        ;
    return *s ? (Rune *)s : 0;
}

static const uchar utfbyte[UTF_SIZ + 1] = {0x80, 0, 0xC0, 0xE0, 0xF0};
static const uchar utfmask[UTF_SIZ + 1] = {0xC0, 0x80, 0xE0, 0xF0, 0xF8};
static const Rune utfmin[UTF_SIZ + 1] = {0, 0, 0x80, 0x800, 0x10000};
static const Rune utfmax[UTF_SIZ + 1] = {0x10FFFF, 0x7F, 0x7FF, 0xFFFF, 0x10FFFF};

int Hexe::Terminal::isboxdraw(Rune u)
{
    Rune block = u & ~0xff;
    return (block == 0x2500 && boxdata[u & 0xFF]) ||
           (block == 0x2800);
}

/* the "index" is actually the entire shape data encoded as ushort */
ushort
Hexe::Terminal::boxdrawindex(const Glyph *g)
{
    if ((g->u & ~0xff) == 0x2800)
        return BRL | (g->u & 0xFF);
    if ((g->mode & ATTR_BOLD))
        return BDB | boxdata[g->u & 0xFF];
    return boxdata[g->u & 0xFF];
}

static void *
xmalloc(size_t len)
{
    void *p;

    p = malloc(len);
    assert(p != NULL);

    return p;
}

static void *
xrealloc(void *p, size_t len)
{
    p = realloc(p, len);
    assert(p != NULL);

    return p;
}

static char *
xstrdup(char *s)
{
#ifdef WIN32
    s = _strdup(s);
#else
    s = strdup(s);
#endif
    assert(s != NULL);
    return s;
}

static size_t utf8decode(const char *, Rune *, size_t);
static Rune utf8decodebyte(char, size_t *);
static char utf8encodebyte(Rune, size_t);
static size_t utf8validate(Rune *, size_t);
static size_t utf8encode(Rune, char *);

static char *base64dec(const char *);
static char base64dec_getc(const char **);

static intmax_t xwrite(int, const char *, size_t);

size_t
utf8decode(const char *c, Rune *u, size_t clen)
{
    size_t i, j, len, type;
    Rune udecoded;

    *u = UTF_INVALID;
    if (!clen)
        return 0;
    udecoded = utf8decodebyte(c[0], &len);
    if (!BETWEEN(len, 1, UTF_SIZ))
        return 1;
    for (i = 1, j = 1; i < clen && j < len; ++i, ++j)
    {
        udecoded = (udecoded << 6) | utf8decodebyte(c[i], &type);
        if (type != 0)
            return j;
    }
    if (j < len)
        return 0;
    *u = udecoded;
    utf8validate(u, len);

    return len;
}

Rune utf8decodebyte(char c, size_t *i)
{
    for (*i = 0; *i < LEN(utfmask); ++(*i))
        if (((uchar)c & utfmask[*i]) == utfbyte[*i])
            return (uchar)c & ~utfmask[*i];

    return 0;
}

size_t
utf8encode(Rune u, char *c)
{
    size_t len, i;

    len = utf8validate(&u, 0);
    if (len > UTF_SIZ)
        return 0;

    for (i = len - 1; i != 0; --i)
    {
        c[i] = utf8encodebyte(u, 0);
        u >>= 6;
    }
    c[0] = utf8encodebyte(u, len);

    return len;
}

char utf8encodebyte(Rune u, size_t i)
{
    return utfbyte[i] | (u & ~utfmask[i]);
}

size_t
utf8validate(Rune *u, size_t i)
{
    if (!BETWEEN(*u, utfmin[i], utfmax[i]) || BETWEEN(*u, 0xD800, 0xDFFF))
        *u = UTF_INVALID;
    for (i = 1; *u > utfmax[i]; ++i)
        ;

    return i;
}

static const char base64_digits[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 62, 0, 0, 0,
    63, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 0, 0, 0, 0 /*-1*/, 0, 0, 0, 0, 1,
    2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
    22, 23, 24, 25, 0, 0, 0, 0, 0, 0, 26, 27, 28, 29, 30, 31, 32, 33, 34,
    35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

char base64dec_getc(const char **src)
{
    while (**src && !isprint(**src))
        (*src)++;
    return **src ? *((*src)++) : '='; /* emulate padding if string ends */
}

char *
base64dec(const char *src)
{
    size_t in_len = strlen(src);
    char *result, *dst;

    if (in_len % 4)
        in_len += 4 - (in_len % 4);
    result = dst = (char *)xmalloc(in_len / 4 * 3 + 1);
    while (*src)
    {
        int a = base64_digits[(unsigned char)base64dec_getc(&src)];
        int b = base64_digits[(unsigned char)base64dec_getc(&src)];
        int c = base64_digits[(unsigned char)base64dec_getc(&src)];
        int d = base64_digits[(unsigned char)base64dec_getc(&src)];

        /* invalid input. 'a' can be -1, e.g. if src is "\n" (c-str) */
        if (a == -1 || b == -1)
            break;

        *dst++ = (a << 2) | ((b & 0x30) >> 4);
        if (c == -1)
            break;
        *dst++ = ((b & 0x0f) << 4) | ((c & 0x3c) >> 2);
        if (d == -1)
            break;
        *dst++ = ((c & 0x03) << 6) | d;
    }
    *dst = '\0';
    return result;
}

void TerminalEmulator::selinit(void)
{
    sel.mode = SEL_IDLE;
    sel.snap = 0;
    sel.ob.x = -1;
}

int TerminalEmulator::tlinelen(int y)
{
    int i = term.col;

    if (term.line[y][i - 1].mode & ATTR_WRAP)
        return i;

    while (i > 0 && term.line[y][i - 1].u == ' ')
        --i;

    return i;
}

void TerminalEmulator::selstart(int col, int row, int snap)
{
    selclear();
    sel.mode = SEL_EMPTY;
    sel.type = SEL_REGULAR;
    sel.alt = IS_SET(MODE_ALTSCREEN);
    sel.snap = snap;
    sel.oe.x = sel.ob.x = col;
    sel.oe.y = sel.ob.y = row;
    selnormalize();

    if (sel.snap != 0)
        sel.mode = SEL_READY;
    tsetdirt(sel.nb.y, sel.ne.y);
}

void TerminalEmulator::selextend(int col, int row, int type, int done)
{
    int oldey, oldex, oldsby, oldsey, oldtype;

    if (sel.mode == SEL_IDLE)
        return;
    if (done && sel.mode == SEL_EMPTY)
    {
        selclear();
        return;
    }

    oldey = sel.oe.y;
    oldex = sel.oe.x;
    oldsby = sel.nb.y;
    oldsey = sel.ne.y;
    oldtype = sel.type;

    sel.oe.x = col;
    sel.oe.y = row;
    selnormalize();
    sel.type = type;

    if (oldey != sel.oe.y || oldex != sel.oe.x || oldtype != sel.type || sel.mode == SEL_EMPTY)
        tsetdirt(MIN(sel.nb.y, oldsby), MAX(sel.ne.y, oldsey));

    sel.mode = done ? SEL_IDLE : SEL_READY;
}

void TerminalEmulator::selnormalize(void)
{
    int i;

    if (sel.type == SEL_REGULAR && sel.ob.y != sel.oe.y)
    {
        sel.nb.x = sel.ob.y < sel.oe.y ? sel.ob.x : sel.oe.x;
        sel.ne.x = sel.ob.y < sel.oe.y ? sel.oe.x : sel.ob.x;
    }
    else
    {
        sel.nb.x = MIN(sel.ob.x, sel.oe.x);
        sel.ne.x = MAX(sel.ob.x, sel.oe.x);
    }
    sel.nb.y = MIN(sel.ob.y, sel.oe.y);
    sel.ne.y = MAX(sel.ob.y, sel.oe.y);

    selsnap(&sel.nb.x, &sel.nb.y, -1);
    selsnap(&sel.ne.x, &sel.ne.y, +1);

    /* expand selection over line breaks */
    if (sel.type == SEL_RECTANGULAR)
        return;
    i = tlinelen(sel.nb.y);
    if (i < sel.nb.x)
        sel.nb.x = i;
    if (tlinelen(sel.ne.y) <= sel.ne.x)
        sel.ne.x = term.col - 1;
}

int TerminalEmulator::selected(int x, int y)
{
    if (sel.mode == SEL_EMPTY || sel.ob.x == -1 ||
        sel.alt != IS_SET(MODE_ALTSCREEN))
        return 0;

    if (sel.type == SEL_RECTANGULAR)
        return BETWEEN(y, sel.nb.y, sel.ne.y) && BETWEEN(x, sel.nb.x, sel.ne.x);

    return BETWEEN(y, sel.nb.y, sel.ne.y) && (y != sel.nb.y || x >= sel.nb.x) && (y != sel.ne.y || x <= sel.ne.x);
}

void TerminalEmulator::selsnap(int *x, int *y, int direction)
{
    int newx, newy, xt, yt;
    int delim, prevdelim;
    Glyph *gp, *prevgp;

    switch (sel.snap)
    {
    case SNAP_WORD:
        /*
		 * Snap around if the word wraps around at the end or
		 * beginning of a line.
		 */
        prevgp = &term.line[*y][*x];
        prevdelim = ISDELIM(prevgp->u);
        for (;;)
        {
            newx = *x + direction;
            newy = *y;
            if (!BETWEEN(newx, 0, term.col - 1))
            {
                newy += direction;
                newx = (newx + term.col) % term.col;
                if (!BETWEEN(newy, 0, term.row - 1))
                    break;

                if (direction > 0)
                    yt = *y, xt = *x;
                else
                    yt = newy, xt = newx;
                if (!(term.line[yt][xt].mode & ATTR_WRAP))
                    break;
            }

            if (newx >= tlinelen(newy))
                break;

            gp = &term.line[newy][newx];
            delim = ISDELIM(gp->u);
            if (!(gp->mode & ATTR_WDUMMY) && (delim != prevdelim || (delim && gp->u != prevgp->u)))
                break;

            *x = newx;
            *y = newy;
            prevgp = gp;
            prevdelim = delim;
        }
        break;
    case SNAP_LINE:
        /*
		 * Snap around if the the previous line or the current one
		 * has set ATTR_WRAP at its end. Then the whole next or
		 * previous line will be selected.
		 */
        *x = (direction < 0) ? 0 : term.col - 1;
        if (direction < 0)
        {
            for (; *y > 0; *y += direction)
            {
                if (!(term.line[*y - 1][term.col - 1].mode & ATTR_WRAP))
                {
                    break;
                }
            }
        }
        else if (direction > 0)
        {
            for (; *y < term.row - 1; *y += direction)
            {
                if (!(term.line[*y][term.col - 1].mode & ATTR_WRAP))
                {
                    break;
                }
            }
        }
        break;
    }
}

char *
TerminalEmulator::getsel(void)
{
    char *str, *ptr;
    int y, bufsize, lastx, linelen;
    Glyph *gp, *last;

    if (sel.ob.x == -1)
        return NULL;

    bufsize = (term.col + 1) * (sel.ne.y - sel.nb.y + 1) * UTF_SIZ;
    ptr = str = (char *)xmalloc(bufsize);

    /* append every set & selected glyph to the selection */
    for (y = sel.nb.y; y <= sel.ne.y; y++)
    {
        if ((linelen = tlinelen(y)) == 0)
        {
            *ptr++ = '\n';
            continue;
        }

        if (sel.type == SEL_RECTANGULAR)
        {
            gp = &term.line[y][sel.nb.x];
            lastx = sel.ne.x;
        }
        else
        {
            gp = &term.line[y][sel.nb.y == y ? sel.nb.x : 0];
            lastx = (sel.ne.y == y) ? sel.ne.x : term.col - 1;
        }
        last = &term.line[y][MIN(lastx, linelen - 1)];
        while (last >= gp && last->u == ' ')
            --last;

        for (; gp <= last; ++gp)
        {
            if (gp->mode & ATTR_WDUMMY)
                continue;

            ptr += utf8encode(gp->u, ptr);
        }

        /*
		 * Copy and pasting of line endings is inconsistent
		 * in the inconsistent terminal and GUI world.
		 * The best solution seems like to produce '\n' when
		 * something is copied from st and convert '\n' to
		 * '\r', when something to be pasted is received by
		 * st.
		 * FIXME: Fix the computer world.
		 */
        if ((y < sel.ne.y || lastx >= linelen) &&
            (!(last->mode & ATTR_WRAP) || sel.type == SEL_RECTANGULAR))
            *ptr++ = '\n';
    }
    *ptr = 0;
    return str;
}

void TerminalEmulator::selclear(void)
{
    if (sel.ob.x == -1)
        return;
    sel.mode = SEL_IDLE;
    sel.ob.x = -1;
    tsetdirt(sel.nb.y, sel.ne.y);
}

void TerminalEmulator::_die(const char *errstr, ...)
{
    char buf[256];

    memset(buf, 0, 256);
    va_list ap;
    va_start(ap, errstr);
    vsnprintf(buf, 256, errstr, ap);
    va_end(ap);
    LogError(buf);
    Terminate();
}

size_t
TerminalEmulator::ttyread(void)
{
    int ret, written;

    /* append read bytes to unprocessed bytes */
    ret = m_pty->Read(m_buf + m_buflen, LEN(m_buf) - m_buflen);

    switch (ret)
    {
    case 0:
        return 0;
    case -1:
        _die("couldn't read from shell: %s\n", strerror(errno));
        return 0;
    default:
        m_buflen += ret;
        written = twrite(m_buf, m_buflen, 0);
        m_buflen -= written;
        /* keep any incomplete UTF-8 byte sequence for the next call */
        if (m_buflen > 0)
            memmove(m_buf, m_buf + written, m_buflen);
        return ret;
    }

    return 0;
}

void TerminalEmulator::ttywrite(const char *s, size_t n, int may_echo)
{
    const char *next;

    if (may_echo && IS_SET(MODE_ECHO))
        twrite(s, (int)n, 1);

    if (!IS_SET(MODE_CRLF))
    {
        ttywriteraw(s, n);
        return;
    }

    /* This is similar to how the kernel handles ONLCR for ttys */
    while (n > 0)
    {
        if (*s == '\r')
        {
            next = s + 1;
            ttywriteraw("\r\n", 2);
        }
        else
        {
            next = (const char *)memchr(s, '\r', n);
            DEFAULT(next, s + n);
            ttywriteraw(s, next - s);
        }
        n -= next - s;
        s = next;
    }
}

void TerminalEmulator::ttywriteraw(const char *s, size_t n)
{
    if (m_pty->Write(s, n) < n)
    {
        _die("Failed to write to TTY");
    }
}

void TerminalEmulator::ttyhangup()
{
    if (m_process)
        m_process->Terminate();
}

int TerminalEmulator::tattrset(int attr)
{
    int i, j;

    for (i = 0; i < term.row - 1; i++)
    {
        for (j = 0; j < term.col - 1; j++)
        {
            if (term.line[i][j].mode & attr)
                return 1;
        }
    }

    return 0;
}

void TerminalEmulator::tsetdirt(int top, int bot)
{
    int i;

    LIMIT(top, 0, term.row - 1);
    LIMIT(bot, 0, term.row - 1);

    for (i = top; i <= bot; i++)
        term.dirty[i] = 1;
}

void TerminalEmulator::tsetdirtattr(int attr)
{
    int i, j;

    for (i = 0; i < term.row - 1; i++)
    {
        for (j = 0; j < term.col - 1; j++)
        {
            if (term.line[i][j].mode & attr)
            {
                tsetdirt(i, i);
                break;
            }
        }
    }
}

void TerminalEmulator::tfulldirt(void)
{
    tsetdirt(0, term.row - 1);
}

void TerminalEmulator::tcursor(int mode)
{
    static TCursor c[2];
    int alt = IS_SET(MODE_ALTSCREEN);

    if (mode == CURSOR_SAVE)
    {
        c[alt] = term.c;
    }
    else if (mode == CURSOR_LOAD)
    {
        term.c = c[alt];
        tmoveto(c[alt].x, c[alt].y);
    }
}

void TerminalEmulator::treset(void)
{
    uint i;

    term.c = TCursor{};
    term.c.attr = Glyph{};
    term.c.attr.mode = ATTR_NULL;
    term.c.attr.fg = defaultfg;
    term.c.attr.bg = defaultbg;
    term.c.x = 0;
    term.c.y = 0;
    term.c.state = CURSOR_DEFAULT;

    memset(term.tabs, 0, term.col * sizeof(*term.tabs));
    for (i = tabspaces; i < (uint)term.col; i += tabspaces)
        term.tabs[i] = 1;
    term.top = 0;
    term.bot = term.row - 1;
    term.mode = MODE_WRAP | MODE_UTF8;
    memset(term.trantbl, CS_USA, sizeof(term.trantbl));
    term.charset = 0;

    for (i = 0; i < 2; i++)
    {
        tmoveto(0, 0);
        tcursor(CURSOR_SAVE);
        tclearregion(0, 0, term.col - 1, term.row - 1);
        tswapscreen();
    }
}

void TerminalEmulator::tnew(int col, int row)
{
    term = Term{};
    term.c = TCursor{};
    term.c.attr = Glyph{};
    term.c.attr.fg = defaultfg;
    term.c.attr.bg = defaultbg;

    tresize(col, row);
    treset();
}

void TerminalEmulator::tswapscreen(void)
{
    Line *tmp = term.line;

    term.line = term.alt;
    term.alt = tmp;
    term.mode ^= MODE_ALTSCREEN;
    tfulldirt();
}

void TerminalEmulator::tscrolldown(int orig, int n)
{
    int i;
    Line temp;

    LIMIT(n, 0, term.bot - orig + 1);

    tsetdirt(orig, term.bot - n);
    tclearregion(0, term.bot - n + 1, term.col - 1, term.bot);

    for (i = term.bot; i >= orig + n; i--)
    {
        temp = term.line[i];
        term.line[i] = term.line[i - n];
        term.line[i - n] = temp;
    }

    selscroll(orig, n);
}

void TerminalEmulator::tscrollup(int orig, int n)
{
    int i;
    Line temp;

    LIMIT(n, 0, term.bot - orig + 1);

    tclearregion(0, orig, term.col - 1, orig + n - 1);
    tsetdirt(orig + n, term.bot);

    for (i = orig; i <= term.bot - n; i++)
    {
        temp = term.line[i];
        term.line[i] = term.line[i + n];
        term.line[i + n] = temp;
    }

    selscroll(orig, -n);
}

void TerminalEmulator::selscroll(int orig, int n)
{
    if (sel.ob.x == -1)
        return;

    if (BETWEEN(sel.nb.y, orig, term.bot) != BETWEEN(sel.ne.y, orig, term.bot))
    {
        selclear();
    }
    else if (BETWEEN(sel.nb.y, orig, term.bot))
    {
        sel.ob.y += n;
        sel.oe.y += n;
        if (sel.ob.y < term.top || sel.ob.y > term.bot ||
            sel.oe.y < term.top || sel.oe.y > term.bot)
        {
            selclear();
        }
        else
        {
            selnormalize();
        }
    }
}

void TerminalEmulator::tnewline(int first_col)
{
    int y = term.c.y;

    if (y == term.bot)
    {
        tscrollup(term.top, 1);
    }
    else
    {
        y++;
    }
    tmoveto(first_col ? 0 : term.c.x, y);
}

void TerminalEmulator::csiparse(void)
{
    char *p = csiescseq.buf, *np;
    long int v;

    csiescseq.narg = 0;
    if (*p == '?')
    {
        csiescseq.priv = 1;
        p++;
    }

    csiescseq.buf[csiescseq.len] = '\0';
    while (p < csiescseq.buf + csiescseq.len)
    {
        np = NULL;
        v = strtol(p, &np, 10);
        if (np == p)
            v = 0;
        if (v == LONG_MAX || v == LONG_MIN)
            v = -1;
        csiescseq.arg[csiescseq.narg++] = v;
        p = np;
        if (*p != ';' || csiescseq.narg == ESC_ARG_SIZ)
            break;
        p++;
    }
    csiescseq.mode[0] = *p++;
    csiescseq.mode[1] = (p < csiescseq.buf + csiescseq.len) ? *p : '\0';
}

/* for absolute user moves, when decom is set */
void TerminalEmulator::tmoveato(int x, int y)
{
    tmoveto(x, y + ((term.c.state & CURSOR_ORIGIN) ? term.top : 0));
}

void TerminalEmulator::tmoveto(int x, int y)
{
    int miny, maxy;

    if (term.c.state & CURSOR_ORIGIN)
    {
        miny = term.top;
        maxy = term.bot;
    }
    else
    {
        miny = 0;
        maxy = term.row - 1;
    }
    term.c.state &= ~CURSOR_WRAPNEXT;
    term.c.x = LIMIT(x, 0, term.col - 1);
    term.c.y = LIMIT(y, miny, maxy);
}

void TerminalEmulator::tsetchar(Rune u, Glyph *attr, int x, int y)
{
    static const char *vt100_0[62] = {
        /* 0x41 - 0x7e */
        "↑", "↓", "→", "←", "█", "▚", "☃",      /* A - G */
        0, 0, 0, 0, 0, 0, 0, 0,                 /* H - O */
        0, 0, 0, 0, 0, 0, 0, 0,                 /* P - W */
        0, 0, 0, 0, 0, 0, 0, " ",               /* X - _ */
        "◆", "▒", "␉", "␌", "␍", "␊", "°", "±", /* ` - g */
        "␤", "␋", "┘", "┐", "┌", "└", "┼", "⎺", /* h - o */
        "⎻", "─", "⎼", "⎽", "├", "┤", "┴", "┬", /* p - w */
        "│", "≤", "≥", "π", "≠", "£", "·",      /* x - ~ */
    };

    /*
	 * The table is proudly stolen from rxvt.
	 */
    if (term.trantbl[term.charset] == CS_GRAPHIC0 &&
        BETWEEN(u, 0x41, 0x7e) && vt100_0[u - 0x41])
        utf8decode(vt100_0[u - 0x41], &u, UTF_SIZ);

    if (term.line[y][x].mode & ATTR_WIDE)
    {
        if (x + 1 < term.col)
        {
            term.line[y][x + 1].u = ' ';
            term.line[y][x + 1].mode &= ~ATTR_WDUMMY;
        }
    }
    else if (term.line[y][x].mode & ATTR_WDUMMY)
    {
        term.line[y][x - 1].u = ' ';
        term.line[y][x - 1].mode &= ~ATTR_WIDE;
    }

    term.dirty[y] = 1;
    term.line[y][x] = *attr;
    term.line[y][x].u = u;

    if (isboxdraw(u))
        term.line[y][x].mode |= ATTR_BOXDRAW;
}

void TerminalEmulator::tclearregion(int x1, int y1, int x2, int y2)
{
    int x, y, temp;
    Glyph *gp;

    if (x1 > x2)
        temp = x1, x1 = x2, x2 = temp;
    if (y1 > y2)
        temp = y1, y1 = y2, y2 = temp;

    LIMIT(x1, 0, term.col - 1);
    LIMIT(x2, 0, term.col - 1);
    LIMIT(y1, 0, term.row - 1);
    LIMIT(y2, 0, term.row - 1);

    for (y = y1; y <= y2; y++)
    {
        term.dirty[y] = 1;
        for (x = x1; x <= x2; x++)
        {
            gp = &term.line[y][x];
            if (selected(x, y))
                selclear();
            gp->fg = term.c.attr.fg;
            gp->bg = term.c.attr.bg;
            gp->mode = 0;
            gp->u = ' ';
        }
    }
}

void TerminalEmulator::tdeletechar(int n)
{
    int dst, src, size;
    Glyph *line;

    LIMIT(n, 0, term.col - term.c.x);

    dst = term.c.x;
    src = term.c.x + n;
    size = term.col - src;
    line = term.line[term.c.y];

    memmove(&line[dst], &line[src], size * sizeof(Glyph));
    tclearregion(term.col - n, term.c.y, term.col - 1, term.c.y);
}

void TerminalEmulator::tinsertblank(int n)
{
    int dst, src, size;
    Glyph *line;

    LIMIT(n, 0, term.col - term.c.x);

    dst = term.c.x + n;
    src = term.c.x;
    size = term.col - dst;
    line = term.line[term.c.y];

    memmove(&line[dst], &line[src], size * sizeof(Glyph));
    tclearregion(src, term.c.y, dst - 1, term.c.y);
}

void TerminalEmulator::tinsertblankline(int n)
{
    if (BETWEEN(term.c.y, term.top, term.bot))
        tscrolldown(term.c.y, n);
}

void TerminalEmulator::tdeleteline(int n)
{
    if (BETWEEN(term.c.y, term.top, term.bot))
        tscrollup(term.c.y, n);
}

int32_t
TerminalEmulator::tdefcolor(int *attr, int *npar, int l)
{
    int32_t idx = -1;
    uint r, g, b;

    switch (attr[*npar + 1])
    {
    case 2: /* direct color in RGB space */
        if (*npar + 4 >= l)
        {
            fprintf(stderr,
                    "erresc(38): Incorrect number of parameters (%d)\n",
                    *npar);
            break;
        }
        r = attr[*npar + 2];
        g = attr[*npar + 3];
        b = attr[*npar + 4];
        *npar += 4;
        if (!BETWEEN(r, 0, 255) || !BETWEEN(g, 0, 255) || !BETWEEN(b, 0, 255))
            fprintf(stderr, "erresc: bad rgb color (%u,%u,%u)\n",
                    r, g, b);
        else
            idx = TRUECOLOR(r, g, b);
        break;
    case 5: /* indexed color */
        if (*npar + 2 >= l)
        {
            fprintf(stderr,
                    "erresc(38): Incorrect number of parameters (%d)\n",
                    *npar);
            break;
        }
        *npar += 2;
        if (!BETWEEN(attr[*npar], 0, 255))
            fprintf(stderr, "erresc: bad fgcolor %d\n", attr[*npar]);
        else
            idx = attr[*npar];
        break;
    case 0: /* implemented defined (only foreground) */
    case 1: /* transparent */
    case 3: /* direct color in CMY space */
    case 4: /* direct color in CMYK space */
    default:
        fprintf(stderr,
                "erresc(38): gfx attr %d unknown\n", attr[*npar]);
        break;
    }

    return idx;
}

void TerminalEmulator::tsetattr(int *attr, int l)
{
    int i;
    int32_t idx;

    for (i = 0; i < l; i++)
    {
        switch (attr[i])
        {
        case 0:
            term.c.attr.mode &= ~(
                ATTR_BOLD |
                ATTR_FAINT |
                ATTR_ITALIC |
                ATTR_UNDERLINE |
                ATTR_BLINK |
                ATTR_REVERSE |
                ATTR_INVISIBLE |
                ATTR_STRUCK);
            term.c.attr.fg = defaultfg;
            term.c.attr.bg = defaultbg;
            break;
        case 1:
            term.c.attr.mode |= ATTR_BOLD;
            break;
        case 2:
            term.c.attr.mode |= ATTR_FAINT;
            break;
        case 3:
            term.c.attr.mode |= ATTR_ITALIC;
            break;
        case 4:
            term.c.attr.mode |= ATTR_UNDERLINE;
            break;
        case 5: /* slow blink */
                /* FALLTHROUGH */
        case 6: /* rapid blink */
            term.c.attr.mode |= ATTR_BLINK;
            break;
        case 7:
            term.c.attr.mode |= ATTR_REVERSE;
            break;
        case 8:
            term.c.attr.mode |= ATTR_INVISIBLE;
            break;
        case 9:
            term.c.attr.mode |= ATTR_STRUCK;
            break;
        case 22:
            term.c.attr.mode &= ~(ATTR_BOLD | ATTR_FAINT);
            break;
        case 23:
            term.c.attr.mode &= ~ATTR_ITALIC;
            break;
        case 24:
            term.c.attr.mode &= ~ATTR_UNDERLINE;
            break;
        case 25:
            term.c.attr.mode &= ~ATTR_BLINK;
            break;
        case 27:
            term.c.attr.mode &= ~ATTR_REVERSE;
            break;
        case 28:
            term.c.attr.mode &= ~ATTR_INVISIBLE;
            break;
        case 29:
            term.c.attr.mode &= ~ATTR_STRUCK;
            break;
        case 38:
            if ((idx = tdefcolor(attr, &i, l)) >= 0)
                term.c.attr.fg = idx;
            break;
        case 39:
            term.c.attr.fg = defaultfg;
            break;
        case 48:
            if ((idx = tdefcolor(attr, &i, l)) >= 0)
                term.c.attr.bg = idx;
            break;
        case 49:
            term.c.attr.bg = defaultbg;
            break;
        default:
            if (BETWEEN(attr[i], 30, 37))
            {
                term.c.attr.fg = attr[i] - 30;
            }
            else if (BETWEEN(attr[i], 40, 47))
            {
                term.c.attr.bg = attr[i] - 40;
            }
            else if (BETWEEN(attr[i], 90, 97))
            {
                term.c.attr.fg = attr[i] - 90 + 8;
            }
            else if (BETWEEN(attr[i], 100, 107))
            {
                term.c.attr.bg = attr[i] - 100 + 8;
            }
            else
            {
                fprintf(stderr,
                        "erresc(default): gfx attr %d unknown\n",
                        attr[i]);
                csidump();
            }
            break;
        }
    }
}

void TerminalEmulator::tsetscroll(int t, int b)
{
    int temp;

    LIMIT(t, 0, term.row - 1);
    LIMIT(b, 0, term.row - 1);
    if (t > b)
    {
        temp = t;
        t = b;
        b = temp;
    }
    term.top = t;
    term.bot = b;
}

void TerminalEmulator::tsetmode(int priv, int set, int *args, int narg)
{
    int alt, *lim;

    for (lim = args + narg; args < lim; ++args)
    {
        if (priv)
        {
            switch (*args)
            {
            case 1: /* DECCKM -- Cursor key */
                xsetmode(set, MODE_APPCURSOR);
                break;
            case 5: /* DECSCNM -- Reverse video */
                xsetmode(set, MODE_REVERSE);
                break;
            case 6: /* DECOM -- Origin */
                MODBIT(term.c.state, set, CURSOR_ORIGIN);
                tmoveato(0, 0);
                break;
            case 7: /* DECAWM -- Auto wrap */
                MODBIT(term.mode, set, MODE_WRAP);
                break;
            case 0:  /* Error (IGNORED) */
            case 2:  /* DECANM -- ANSI/VT52 (IGNORED) */
            case 3:  /* DECCOLM -- Column  (IGNORED) */
            case 4:  /* DECSCLM -- Scroll (IGNORED) */
            case 8:  /* DECARM -- Auto repeat (IGNORED) */
            case 18: /* DECPFF -- Printer feed (IGNORED) */
            case 19: /* DECPEX -- Printer extent (IGNORED) */
            case 42: /* DECNRCM -- National characters (IGNORED) */
            case 12: /* att610 -- Start blinking cursor (IGNORED) */
                break;
            case 25: /* DECTCEM -- Text Cursor Enable Mode */
                xsetmode(!set, MODE_HIDE);
                break;
            case 9: /* X10 mouse compatibility mode */
                xsetpointermotion(0);
                xsetmode(0, MODE_MOUSE);
                xsetmode(set, MODE_MOUSEX10);
                break;
            case 1000: /* 1000: report button press */
                xsetpointermotion(0);
                xsetmode(0, MODE_MOUSE);
                xsetmode(set, MODE_MOUSEBTN);
                break;
            case 1002: /* 1002: report motion on button press */
                xsetpointermotion(0);
                xsetmode(0, MODE_MOUSE);
                xsetmode(set, MODE_MOUSEMOTION);
                break;
            case 1003: /* 1003: enable all mouse motions */
                xsetpointermotion(set);
                xsetmode(0, MODE_MOUSE);
                xsetmode(set, MODE_MOUSEMANY);
                break;
            case 1004: /* 1004: send focus events to tty */
                xsetmode(set, MODE_FOCUS);
                break;
            case 1006: /* 1006: extended reporting mode */
                xsetmode(set, MODE_MOUSESGR);
                break;
            case 1034:
                xsetmode(set, MODE_8BIT);
                break;
            case 1049: /* swap screen & set/restore cursor as xterm */
                if (!allowaltscreen)
                    break;
                tcursor((set) ? CURSOR_SAVE : CURSOR_LOAD);
                /* FALLTHROUGH */
            case 47: /* swap screen */
            case 1047:
                if (!allowaltscreen)
                    break;
                alt = IS_SET(MODE_ALTSCREEN);
                if (alt)
                {
                    tclearregion(0, 0, term.col - 1,
                                 term.row - 1);
                }
                if (set ^ alt) /* set is always 1 or 0 */
                    tswapscreen();
                if (*args != 1049)
                    break;
                /* FALLTHROUGH */
            case 1048:
                tcursor((set) ? CURSOR_SAVE : CURSOR_LOAD);
                break;
            case 2004: /* 2004: bracketed paste mode */
                xsetmode(set, MODE_BRCKTPASTE);
                break;
            /* Not implemented mouse modes. See comments there. */
            case 1001: /* mouse highlight mode; can hang the
				      terminal by design when implemented. */
            case 1005: /* UTF-8 mouse mode; will confuse
				      applications not supporting UTF-8
				      and luit. */
            case 1015: /* urxvt mangled mouse mode; incompatible
				      and can be mistaken for other control
				      codes. */
                break;
            default:
                fprintf(stderr,
                        "erresc: unknown private set/reset mode %d\n",
                        *args);
                break;
            }
        }
        else
        {
            switch (*args)
            {
            case 0: /* Error (IGNORED) */
                break;
            case 2:
                xsetmode(set, MODE_KBDLOCK);
                break;
            case 4: /* IRM -- Insertion-replacement */
                MODBIT(term.mode, set, MODE_INSERT);
                break;
            case 12: /* SRM -- Send/Receive */
                MODBIT(term.mode, !set, MODE_ECHO);
                break;
            case 20: /* LNM -- Linefeed/new line */
                MODBIT(term.mode, set, MODE_CRLF);
                break;
            default:
                fprintf(stderr,
                        "erresc: unknown set/reset mode %d\n",
                        *args);
                break;
            }
        }
    }
}

void TerminalEmulator::csihandle(void)
{
    char buf[40];
    int len;

    std::shared_ptr<TerminalDisplay> dpy{};

    switch (csiescseq.mode[0])
    {
    default:
    unknown:
        fprintf(stderr, "erresc: unknown csi ");
        csidump();
        /* die(""); */
        break;
    case '@': /* ICH -- Insert <n> blank char */
        DEFAULT(csiescseq.arg[0], 1);
        tinsertblank(csiescseq.arg[0]);
        break;
    case 'A': /* CUU -- Cursor <n> Up */
        DEFAULT(csiescseq.arg[0], 1);
        tmoveto(term.c.x, term.c.y - csiescseq.arg[0]);
        break;
    case 'B': /* CUD -- Cursor <n> Down */
    case 'e': /* VPR --Cursor <n> Down */
        DEFAULT(csiescseq.arg[0], 1);
        tmoveto(term.c.x, term.c.y + csiescseq.arg[0]);
        break;
    case 'i': /* MC -- Media Copy */
        switch (csiescseq.arg[0])
        {
        case 0:
            tdump();
            break;
        case 1:
            tdumpline(term.c.y);
            break;
        case 2:
            tdumpsel();
            break;
        case 4:
            term.mode &= ~MODE_PRINT;
            break;
        case 5:
            term.mode |= MODE_PRINT;
            break;
        }
        break;
    case 'c': /* DA -- Device Attributes */
        if (csiescseq.arg[0] == 0)
            ttywrite(vtiden, strlen(vtiden), 0);
        break;
    case 'b': /* REP -- if last char is printable print it <n> more times */
        DEFAULT(csiescseq.arg[0], 1);
        if (term.lastc)
            while (csiescseq.arg[0]-- > 0)
                tputc(term.lastc);
        break;
    case 'C': /* CUF -- Cursor <n> Forward */
    case 'a': /* HPR -- Cursor <n> Forward */
        DEFAULT(csiescseq.arg[0], 1);
        tmoveto(term.c.x + csiescseq.arg[0], term.c.y);
        break;
    case 'D': /* CUB -- Cursor <n> Backward */
        DEFAULT(csiescseq.arg[0], 1);
        tmoveto(term.c.x - csiescseq.arg[0], term.c.y);
        break;
    case 'E': /* CNL -- Cursor <n> Down and first col */
        DEFAULT(csiescseq.arg[0], 1);
        tmoveto(0, term.c.y + csiescseq.arg[0]);
        break;
    case 'F': /* CPL -- Cursor <n> Up and first col */
        DEFAULT(csiescseq.arg[0], 1);
        tmoveto(0, term.c.y - csiescseq.arg[0]);
        break;
    case 'g': /* TBC -- Tabulation clear */
        switch (csiescseq.arg[0])
        {
        case 0: /* clear current tab stop */
            term.tabs[term.c.x] = 0;
            break;
        case 3: /* clear all the tabs */
            memset(term.tabs, 0, term.col * sizeof(*term.tabs));
            break;
        default:
            goto unknown;
        }
        break;
    case 'G': /* CHA -- Move to <col> */
    case '`': /* HPA */
        DEFAULT(csiescseq.arg[0], 1);
        tmoveto(csiescseq.arg[0] - 1, term.c.y);
        break;
    case 'H': /* CUP -- Move to <row> <col> */
    case 'f': /* HVP */
        DEFAULT(csiescseq.arg[0], 1);
        DEFAULT(csiescseq.arg[1], 1);
        tmoveato(csiescseq.arg[1] - 1, csiescseq.arg[0] - 1);
        break;
    case 'I': /* CHT -- Cursor Forward Tabulation <n> tab stops */
        DEFAULT(csiescseq.arg[0], 1);
        tputtab(csiescseq.arg[0]);
        break;
    case 'J': /* ED -- Clear screen */
        switch (csiescseq.arg[0])
        {
        case 0: /* below */
            tclearregion(term.c.x, term.c.y, term.col - 1, term.c.y);
            if (term.c.y < term.row - 1)
            {
                tclearregion(0, term.c.y + 1, term.col - 1,
                             term.row - 1);
            }
            break;
        case 1: /* above */
            if (term.c.y > 1)
                tclearregion(0, 0, term.col - 1, term.c.y - 1);
            tclearregion(0, term.c.y, term.c.x, term.c.y);
            break;
        case 2: /* all */
            tclearregion(0, 0, term.col - 1, term.row - 1);
            break;
        default:
            goto unknown;
        }
        break;
    case 'K': /* EL -- Clear line */
        switch (csiescseq.arg[0])
        {
        case 0: /* right */
            tclearregion(term.c.x, term.c.y, term.col - 1,
                         term.c.y);
            break;
        case 1: /* left */
            tclearregion(0, term.c.y, term.c.x, term.c.y);
            break;
        case 2: /* all */
            tclearregion(0, term.c.y, term.col - 1, term.c.y);
            break;
        }
        break;
    case 'S': /* SU -- Scroll <n> line up */
        DEFAULT(csiescseq.arg[0], 1);
        tscrollup(term.top, csiescseq.arg[0]);
        break;
    case 'T': /* SD -- Scroll <n> line down */
        DEFAULT(csiescseq.arg[0], 1);
        tscrolldown(term.top, csiescseq.arg[0]);
        break;
    case 'L': /* IL -- Insert <n> blank lines */
        DEFAULT(csiescseq.arg[0], 1);
        tinsertblankline(csiescseq.arg[0]);
        break;
    case 'l': /* RM -- Reset Mode */
        tsetmode(csiescseq.priv, 0, csiescseq.arg, csiescseq.narg);
        break;
    case 'M': /* DL -- Delete <n> lines */
        DEFAULT(csiescseq.arg[0], 1);
        tdeleteline(csiescseq.arg[0]);
        break;
    case 'X': /* ECH -- Erase <n> char */
        DEFAULT(csiescseq.arg[0], 1);
        tclearregion(term.c.x, term.c.y,
                     term.c.x + csiescseq.arg[0] - 1, term.c.y);
        break;
    case 'P': /* DCH -- Delete <n> char */
        DEFAULT(csiescseq.arg[0], 1);
        tdeletechar(csiescseq.arg[0]);
        break;
    case 'Z': /* CBT -- Cursor Backward Tabulation <n> tab stops */
        DEFAULT(csiescseq.arg[0], 1);
        tputtab(-csiescseq.arg[0]);
        break;
    case 'd': /* VPA -- Move to <row> */
        DEFAULT(csiescseq.arg[0], 1);
        tmoveato(term.c.x, csiescseq.arg[0] - 1);
        break;
    case 'h': /* SM -- Set terminal mode */
        tsetmode(csiescseq.priv, 1, csiescseq.arg, csiescseq.narg);
        break;
    case 'm': /* SGR -- Terminal attribute (color) */
        tsetattr(csiescseq.arg, csiescseq.narg);
        break;
    case 'n': /* DSR – Device Status Report (cursor position) */
        if (csiescseq.arg[0] == 6)
        {
            len = snprintf(buf, sizeof(buf), "\033[%i;%iR",
                           term.c.y + 1, term.c.x + 1);
            ttywrite(buf, len, 0);
        }
        break;
    case 'r': /* DECSTBM -- Set Scrolling Region */
        if (csiescseq.priv)
        {
            goto unknown;
        }
        else
        {
            DEFAULT(csiescseq.arg[0], 1);
            DEFAULT(csiescseq.arg[1], term.row);
            tsetscroll(csiescseq.arg[0] - 1, csiescseq.arg[1] - 1);
            tmoveato(0, 0);
        }
        break;
    case 's': /* DECSC -- Save cursor position (ANSI.SYS) */
        tcursor(CURSOR_SAVE);
        break;
    case 'u': /* DECRC -- Restore cursor position (ANSI.SYS) */
        tcursor(CURSOR_LOAD);
        break;
    case ' ':
        switch (csiescseq.mode[1])
        {
        case 'q': /* DECSCUSR -- Set Cursor Style */
            if (csiescseq.arg[0] < 0 || csiescseq.arg[0] < cursor_mode::MAX_CURSOR)
                goto unknown;
            dpy = m_dpy.lock();
            dpy->SetCursorMode((cursor_mode)csiescseq.arg[0]);
            break;
        default:
            goto unknown;
        }
        break;
    }
}

void TerminalEmulator::csidump(void)
{
    size_t i;
    uint c;

    fprintf(stderr, "ESC[");
    for (i = 0; i < csiescseq.len; i++)
    {
        c = csiescseq.buf[i] & 0xff;
        if (isprint(c))
        {
            putc(c, stderr);
        }
        else if (c == '\n')
        {
            fprintf(stderr, "(\\n)");
        }
        else if (c == '\r')
        {
            fprintf(stderr, "(\\r)");
        }
        else if (c == 0x1b)
        {
            fprintf(stderr, "(\\e)");
        }
        else
        {
            fprintf(stderr, "(%02x)", c);
        }
    }
    putc('\n', stderr);
}

void TerminalEmulator::csireset(void)
{
    memset(&csiescseq, 0, sizeof(csiescseq));
}

void TerminalEmulator::strhandle(void)
{
    char *p = NULL, *dec;
    int j, narg, par;

    term.esc &= ~(ESC_STR_END | ESC_STR);
    strparse();
    par = (narg = strescseq.narg) ? atoi(strescseq.args[0]) : 0;

    std::shared_ptr<TerminalDisplay> dpy = m_dpy.lock();

    switch (strescseq.type)
    {
    case ']': /* OSC -- Operating System Command */
        switch (par)
        {
        case 0:
            if (narg > 1)
            {
                dpy->SetTitle(strescseq.args[1]);
                dpy->SetIconTitle(strescseq.args[1]);
            }
            return;
        case 1:
            if (narg > 1)
                dpy->SetIconTitle(strescseq.args[1]);
            return;
        case 2:
            if (narg > 1)
            {
                dpy->SetTitle(strescseq.args[1]);
            }
            return;
        case 52:
            if (narg > 2 && allowwindowops)
            {
                dec = base64dec(strescseq.args[2]);
                if (dec)
                {
                    SetClipboard(dec);
                }
                else
                {
                    fprintf(stderr, "erresc: invalid base64\n");
                }
            }
            return;
        case 4: /* color set */
            if (narg < 3)
                break;
            p = strescseq.args[2];
            /* FALLTHROUGH */
        case 104: /* color reset, here p = NULL */
            j = (narg > 1) ? atoi(strescseq.args[1]) : -1;
            if (ResetColor(j, p))
            {
                if (par == 104 && narg <= 1)
                    return; /* color reset without parameter */
                fprintf(stderr, "erresc: invalid color j=%d, p=%s\n",
                        j, p ? p : "(null)");
            }
            else
            {
                /*
				 * TODO if defaultbg color is changed, borders
				 * are dirty
				 */
                Redraw();
            }
            return;
        }
        break;
    case 'k': /* old title set compatibility */
        dpy->SetTitle(strescseq.args[0]);
        return;
    case 'P': /* DCS -- Device Control String */
    case '_': /* APC -- Application Program Command */
    case '^': /* PM -- Privacy Message */
        return;
    }

    LogError("erresc: unknown str ");
    strdump();
}

void TerminalEmulator::strparse(void)
{
    int c;
    char *p = strescseq.buf;

    strescseq.narg = 0;
    strescseq.buf[strescseq.len] = '\0';

    if (*p == '\0')
        return;

    while (strescseq.narg < STR_ARG_SIZ)
    {
        strescseq.args[strescseq.narg++] = p;
        while ((c = *p) != ';' && c != '\0')
            ++p;
        if (c == '\0')
            return;
        *p++ = '\0';
    }
}

void TerminalEmulator::strdump(void)
{
    size_t i;
    uint c;

    fprintf(stderr, "ESC%c", strescseq.type);
    for (i = 0; i < strescseq.len; i++)
    {
        c = strescseq.buf[i] & 0xff;
        if (c == '\0')
        {
            putc('\n', stderr);
            return;
        }
        else if (isprint(c))
        {
            putc(c, stderr);
        }
        else if (c == '\n')
        {
            fprintf(stderr, "(\\n)");
        }
        else if (c == '\r')
        {
            fprintf(stderr, "(\\r)");
        }
        else if (c == 0x1b)
        {
            fprintf(stderr, "(\\e)");
        }
        else
        {
            fprintf(stderr, "(%02x)", c);
        }
    }
    fprintf(stderr, "ESC\\\n");
}

void TerminalEmulator::strreset(void)
{
    auto old = strescseq.buf;
    strescseq = STREscape{};
    strescseq.buf = (char *)xrealloc(old, STR_BUF_SIZ);
    strescseq.siz = STR_BUF_SIZ;
}

void TerminalEmulator::sendbreak(const Arg *arg)
{
    // TODO: Do something about this
    // if (tcsendbreak(cmdfd, 0))
    //     perror("Error sending break");
}

void TerminalEmulator::tprinter(const char *s, size_t len)
{
    // TODO: Do something about this
    // if (iofd != -1 && xwrite(iofd, s, len) < 0)
    // {
    //     perror("Error writing to output file");
    //     close(iofd);
    //     iofd = -1;
    // }
}

void TerminalEmulator::toggleprinter(const Arg *arg)
{
    term.mode ^= MODE_PRINT;
}

void TerminalEmulator::printscreen(const Arg *arg)
{
    tdump();
}

void TerminalEmulator::printsel(const Arg *arg)
{
    tdumpsel();
}

void TerminalEmulator::tdumpsel(void)
{
    char *ptr;

    if ((ptr = getsel()))
    {
        tprinter(ptr, strlen(ptr));
        free(ptr);
    }
}

void TerminalEmulator::tdumpline(int n)
{
    char buf[UTF_SIZ];
    Glyph *bp, *end;

    bp = &term.line[n][0];
    end = &bp[MIN(tlinelen(n), term.col) - 1];
    if (bp != end || bp->u != ' ')
    {
        for (; bp <= end; ++bp)
            tprinter(buf, utf8encode(bp->u, buf));
    }
    tprinter("\n", 1);
}

void TerminalEmulator::tdump(void)
{
    int i;

    for (i = 0; i < term.row; ++i)
        tdumpline(i);
}

void TerminalEmulator::tputtab(int n)
{
    int x = term.c.x;

    if (n > 0)
    {
        while (x < term.col && n--)
            for (++x; x < term.col && !term.tabs[x]; ++x)
                /* nothing */;
    }
    else if (n < 0)
    {
        while (x > 0 && n++)
            for (--x; x > 0 && !term.tabs[x]; --x)
                /* nothing */;
    }
    term.c.x = LIMIT(x, 0, term.col - 1);
}

void TerminalEmulator::tdefutf8(char ascii)
{
    if (ascii == 'G')
        term.mode |= MODE_UTF8;
    else if (ascii == '@')
        term.mode &= ~MODE_UTF8;
}

void TerminalEmulator::tdeftran(char ascii)
{
    static char cs[] = "0B";
    static int vcs[] = {CS_GRAPHIC0, CS_USA};
    char *p;

    if ((p = strchr(cs, ascii)) == NULL)
    {
        fprintf(stderr, "esc unhandled charset: ESC ( %c\n", ascii);
    }
    else
    {
        term.trantbl[term.icharset] = vcs[p - cs];
    }
}

void TerminalEmulator::tdectest(char c)
{
    int x, y;

    if (c == '8')
    { /* DEC screen alignment test. */
        for (x = 0; x < term.col; ++x)
        {
            for (y = 0; y < term.row; ++y)
                tsetchar('E', &term.c.attr, x, y);
        }
    }
}

void TerminalEmulator::tstrsequence(uchar c)
{
    switch (c)
    {
    case 0x90: /* DCS -- Device Control String */
        c = 'P';
        break;
    case 0x9f: /* APC -- Application Program Command */
        c = '_';
        break;
    case 0x9e: /* PM -- Privacy Message */
        c = '^';
        break;
    case 0x9d: /* OSC -- Operating System Command */
        c = ']';
        break;
    }
    strreset();
    strescseq.type = c;
    term.esc |= ESC_STR;
}

void TerminalEmulator::tcontrolcode(uchar ascii)
{
    switch (ascii)
    {
    case '\t': /* HT */
        tputtab(1);
        return;
    case '\b': /* BS */
        tmoveto(term.c.x - 1, term.c.y);
        return;
    case '\r': /* CR */
        tmoveto(0, term.c.y);
        return;
    case '\f': /* LF */
    case '\v': /* VT */
    case '\n': /* LF */
        /* go to first col if the mode is set */
        tnewline(IS_SET(MODE_CRLF));
        return;
    case '\a': /* BEL */
        if (term.esc & ESC_STR_END)
        {
            /* backwards compatibility to xterm */
            strhandle();
        }
        else
        {
            auto belDpyPtr = m_dpy.lock();
            belDpyPtr->Bell();
        }
        break;
    case '\033': /* ESC */
        csireset();
        term.esc &= ~(ESC_CSI | ESC_ALTCHARSET | ESC_TEST);
        term.esc |= ESC_START;
        return;
    case '\016': /* SO (LS1 -- Locking shift 1) */
    case '\017': /* SI (LS0 -- Locking shift 0) */
        term.charset = 1 - (ascii - '\016');
        return;
    case '\032': /* SUB */
        tsetchar('?', &term.c.attr, term.c.x, term.c.y);
        /* FALLTHROUGH */
    case '\030': /* CAN */
        csireset();
        break;
    case '\005': /* ENQ (IGNORED) */
    case '\000': /* NUL (IGNORED) */
    case '\021': /* XON (IGNORED) */
    case '\023': /* XOFF (IGNORED) */
    case 0177:   /* DEL (IGNORED) */
        return;
    case 0x80: /* TODO: PAD */
    case 0x81: /* TODO: HOP */
    case 0x82: /* TODO: BPH */
    case 0x83: /* TODO: NBH */
    case 0x84: /* TODO: IND */
        break;
    case 0x85:       /* NEL -- Next line */
        tnewline(1); /* always go to first col */
        break;
    case 0x86: /* TODO: SSA */
    case 0x87: /* TODO: ESA */
        break;
    case 0x88: /* HTS -- Horizontal tab stop */
        term.tabs[term.c.x] = 1;
        break;
    case 0x89: /* TODO: HTJ */
    case 0x8a: /* TODO: VTS */
    case 0x8b: /* TODO: PLD */
    case 0x8c: /* TODO: PLU */
    case 0x8d: /* TODO: RI */
    case 0x8e: /* TODO: SS2 */
    case 0x8f: /* TODO: SS3 */
    case 0x91: /* TODO: PU1 */
    case 0x92: /* TODO: PU2 */
    case 0x93: /* TODO: STS */
    case 0x94: /* TODO: CCH */
    case 0x95: /* TODO: MW */
    case 0x96: /* TODO: SPA */
    case 0x97: /* TODO: EPA */
    case 0x98: /* TODO: SOS */
    case 0x99: /* TODO: SGCI */
        break;
    case 0x9a: /* DECID -- Identify Terminal */
        ttywrite(vtiden, strlen(vtiden), 0);
        break;
    case 0x9b: /* TODO: CSI */
    case 0x9c: /* TODO: ST */
        break;
    case 0x90: /* DCS -- Device Control String */
    case 0x9d: /* OSC -- Operating System Command */
    case 0x9e: /* PM -- Privacy Message */
    case 0x9f: /* APC -- Application Program Command */
        tstrsequence(ascii);
        return;
    }
    /* only CAN, SUB, \a and C1 chars interrupt a sequence */
    term.esc &= ~(ESC_STR_END | ESC_STR);
}

/*
 * returns 1 when the sequence is finished and it hasn't to read
 * more characters for this sequence, otherwise 0
 */
int TerminalEmulator::eschandle(uchar ascii)
{
    switch (ascii)
    {
    case '[':
        term.esc |= ESC_CSI;
        return 0;
    case '#':
        term.esc |= ESC_TEST;
        return 0;
    case '%':
        term.esc |= ESC_UTF8;
        return 0;
    case 'P': /* DCS -- Device Control String */
    case '_': /* APC -- Application Program Command */
    case '^': /* PM -- Privacy Message */
    case ']': /* OSC -- Operating System Command */
    case 'k': /* old title set compatibility */
        tstrsequence(ascii);
        return 0;
    case 'n': /* LS2 -- Locking shift 2 */
    case 'o': /* LS3 -- Locking shift 3 */
        term.charset = 2 + (ascii - 'n');
        break;
    case '(': /* GZD4 -- set primary charset G0 */
    case ')': /* G1D4 -- set secondary charset G1 */
    case '*': /* G2D4 -- set tertiary charset G2 */
    case '+': /* G3D4 -- set quaternary charset G3 */
        term.icharset = ascii - '(';
        term.esc |= ESC_ALTCHARSET;
        return 0;
    case 'D': /* IND -- Linefeed */
        if (term.c.y == term.bot)
        {
            tscrollup(term.top, 1);
        }
        else
        {
            tmoveto(term.c.x, term.c.y + 1);
        }
        break;
    case 'E':        /* NEL -- Next line */
        tnewline(1); /* always go to first col */
        break;
    case 'H': /* HTS -- Horizontal tab stop */
        term.tabs[term.c.x] = 1;
        break;
    case 'M': /* RI -- Reverse index */
        if (term.c.y == term.top)
        {
            tscrolldown(term.top, 1);
        }
        else
        {
            tmoveto(term.c.x, term.c.y - 1);
        }
        break;
    case 'Z': /* DECID -- Identify Terminal */
        ttywrite(vtiden, strlen(vtiden), 0);
        break;
    case 'c': /* RIS -- Reset to initial state */
        treset();
        resettitle();
        LoadColors();
        break;
    case '=': /* DECPAM -- Application keypad */
        xsetmode(1, MODE_APPKEYPAD);
        break;
    case '>': /* DECPNM -- Normal keypad */
        xsetmode(0, MODE_APPKEYPAD);
        break;
    case '7': /* DECSC -- Save Cursor */
        tcursor(CURSOR_SAVE);
        break;
    case '8': /* DECRC -- Restore Cursor */
        tcursor(CURSOR_LOAD);
        break;
    case '\\': /* ST -- String Terminator */
        if (term.esc & ESC_STR_END)
            strhandle();
        break;
    default:
        fprintf(stderr, "erresc: unknown sequence ESC 0x%02X '%c'\n",
                (uchar)ascii, isprint(ascii) ? ascii : '.');
        break;
    }
    return 1;
}

static const unsigned char table[] = {
#include "nonspacing.h"
};

static const unsigned char wtable[] = {
#include "wide.h"
};

static int Hexe_wcwidth(Rune wc)
{
    if (wc < 0xffU)
        return (wc + 1 & 0x7f) >= 0x21 ? 1 : wc ? -1 : 0;

    if (is_emoji(wc))
        return 2;

    if ((wc & 0xfffeffffU) < 0xfffe)
    {
        if ((table[table[wc >> 8] * 32 + ((wc & 255) >> 3)] >> (wc & 7)) & 1)
            return 0;
        if ((wtable[wtable[wc >> 8] * 32 + ((wc & 255) >> 3)] >> (wc & 7)) & 1)
            return 2;
        return 1;
    }

    if ((wc & 0xfffe) == 0xfffe)
        return -1;
    if (wc - 0x20000U < 0x20000)
        return 2;
    if (wc == 0xe0001 || wc - 0xe0020U < 0x5f || wc - 0xe0100 < 0xef)
        return 0;

    return 1;
}

void TerminalEmulator::tputc(Rune u)
{
    char c[UTF_SIZ];
    int control;
    int width;
    size_t len;
    Glyph *gp;

    control = ISCONTROL(u);
    if (u < 127 || !IS_SET(MODE_UTF8))
    {
        c[0] = u;
        width = (int)(len = 1);
    }
    else
    {
        len = utf8encode(u, c);
        //if (!control && (width = wcwidth9(u)) < 0) {
        if (!control && (width = Hexe_wcwidth(u)) < 0)
        {
            if (width == -1)
            {
                width = 0;
            }
            else
            {
                width = 1;
            }
        }
    }

    if (IS_SET(MODE_PRINT))
        tprinter(c, len);

    /*
	 * STR sequence must be checked before anything else
	 * because it uses all following characters until it
	 * receives a ESC, a SUB, a ST or any other C1 control
	 * character.
	 */
    if (term.esc & ESC_STR)
    {
        if (u == '\a' || u == 030 || u == 032 || u == 033 ||
            ISCONTROLC1(u))
        {
            term.esc &= ~(ESC_START | ESC_STR);
            term.esc |= ESC_STR_END;
            goto check_control_code;
        }

        if (strescseq.len + len >= strescseq.siz)
        {
            /*
			 * Here is a bug in terminals. If the user never sends
			 * some code to stop the str or esc command, then st
			 * will stop responding. But this is better than
			 * silently failing with unknown characters. At least
			 * then users will report back.
			 *
			 * In the case users ever get fixed, here is the code:
			 */
            /*
			 * term.esc = 0;
			 * strhandle();
			 */
            if (strescseq.siz > (SIZE_MAX - UTF_SIZ) / 2)
                return;
            strescseq.siz *= 2;
            strescseq.buf = (char *)xrealloc(strescseq.buf, strescseq.siz);
        }

        memmove(&strescseq.buf[strescseq.len], c, len);
        strescseq.len += len;
        return;
    }

check_control_code:
    /*
	 * Actions of control codes must be performed as soon they arrive
	 * because they can be embedded inside a control sequence, and
	 * they must not cause conflicts with sequences.
	 */
    if (control)
    {
        tcontrolcode(u);
        /*
		 * control codes are not shown ever
		 */
        if (!term.esc)
            term.lastc = 0;
        return;
    }
    else if (term.esc & ESC_START)
    {
        if (term.esc & ESC_CSI)
        {
            csiescseq.buf[csiescseq.len++] = u;
            if (BETWEEN(u, 0x40, 0x7E) || csiescseq.len >= sizeof(csiescseq.buf) - 1)
            {
                term.esc = 0;
                csiparse();
                csihandle();
            }
            return;
        }
        else if (term.esc & ESC_UTF8)
        {
            tdefutf8(u);
        }
        else if (term.esc & ESC_ALTCHARSET)
        {
            tdeftran(u);
        }
        else if (term.esc & ESC_TEST)
        {
            tdectest(u);
        }
        else
        {
            if (!eschandle(u))
                return;
            /* sequence already finished */
        }
        term.esc = 0;
        /*
		 * All characters which form part of a sequence are not
		 * printed
		 */
        return;
    }
    if (selected(term.c.x, term.c.y))
        selclear();

    gp = &term.line[term.c.y][term.c.x];
    if (IS_SET(MODE_WRAP) && (term.c.state & CURSOR_WRAPNEXT))
    {
        gp->mode |= ATTR_WRAP;
        tnewline(1);
        gp = &term.line[term.c.y][term.c.x];
    }

    if (IS_SET(MODE_INSERT) && term.c.x + width < term.col)
        memmove(gp + width, gp, (term.col - term.c.x - width) * sizeof(Glyph));

    if (term.c.x + width > term.col)
    {
        tnewline(1);
        gp = &term.line[term.c.y][term.c.x];
    }

    tsetchar(u, &term.c.attr, term.c.x, term.c.y);
    term.lastc = u;

    if (width == 2)
    {
        gp->mode |= ATTR_WIDE;
        if (term.c.x + 1 < term.col)
        {
            gp[1].u = '\0';
            gp[1].mode = ATTR_WDUMMY;
        }
        if (is_emoji(u))
        {
            gp->mode |= ATTR_EMOJI;
        }
    }

    if (term.c.x + width < term.col)
    {
        tmoveto(term.c.x + width, term.c.y);
    }
    else
    {
        term.c.state |= CURSOR_WRAPNEXT;
    }
}

int TerminalEmulator::twrite(const char *buf, int buflen, int show_ctrl)
{
    size_t charsize;
    Rune u;
    size_t n;

    for (n = 0; n < buflen; n += charsize)
    {
        if (IS_SET(MODE_UTF8))
        {
            /* process a complete utf8 char */
            charsize = utf8decode(buf + n, &u, buflen - n);
            if (charsize == 0)
                break;
        }
        else
        {
            u = buf[n] & 0xFF;
            charsize = 1;
        }
        if (show_ctrl && ISCONTROL(u))
        {
            if (u & 0x80)
            {
                u &= 0x7f;
                tputc('^');
                tputc('[');
            }
            else if (u != '\n' && u != '\r' && u != '\t')
            {
                u ^= 0x40;
                tputc('^');
            }
        }
        tputc(u);
    }
    return (int)n;
}

void TerminalEmulator::tresize(int col, int row)
{
    int i;
    int minrow = MIN(row, term.row);
    int mincol = MIN(col, term.col);
    int *bp;
    TCursor c;

    if (col < 1 || row < 1)
    {
        fprintf(stderr,
                "tresize: error resizing to %dx%d\n", col, row);
        return;
    }

    /*
	 * slide screen to keep cursor where we expect it -
	 * tscrollup would work here, but we can optimize to
	 * memmove because we're freeing the earlier lines
	 */
    for (i = 0; i <= term.c.y - row; i++)
    {
        free(term.line[i]);
        free(term.alt[i]);
    }
    /* ensure that both src and dst are not NULL */
    if (i > 0)
    {
        memmove(term.line, term.line + i, row * sizeof(Line));
        memmove(term.alt, term.alt + i, row * sizeof(Line));
    }
    for (i += row; i < term.row; i++)
    {
        free(term.line[i]);
        free(term.alt[i]);
    }

    /* resize to new height */
    term.line = (Line *)xrealloc(term.line, row * sizeof(Line));
    term.alt = (Line *)xrealloc(term.alt, row * sizeof(Line));
    term.dirty = (int *)xrealloc(term.dirty, row * sizeof(*term.dirty));
    term.tabs = (int *)xrealloc(term.tabs, col * sizeof(*term.tabs));

    /* resize each row to new width, zero-pad if needed */
    for (i = 0; i < minrow; i++)
    {
        term.line[i] = (Line)xrealloc(term.line[i], col * sizeof(Glyph));
        term.alt[i] = (Line)xrealloc(term.alt[i], col * sizeof(Glyph));
    }

    /* allocate any new rows */
    for (/* i = minrow */; i < row; i++)
    {
        term.line[i] = (Line)xmalloc(col * sizeof(Glyph));
        term.alt[i] = (Line)xmalloc(col * sizeof(Glyph));
    }
    if (col > term.col)
    {
        bp = term.tabs + term.col;

        memset(bp, 0, sizeof(*term.tabs) * (col - term.col));
        while (--bp > term.tabs && !*bp)
            /* nothing */;
        for (bp += tabspaces; bp < term.tabs + col; bp += tabspaces)
            *bp = 1;
    }
    /* update terminal size */
    term.col = col;
    term.row = row;
    /* reset scrolling region */
    tsetscroll(0, row - 1);
    /* make use of the LIMIT in tmoveto */
    tmoveto(term.c.x, term.c.y);
    /* Clearing both screens (it makes dirty all lines) */
    c = term.c;
    for (i = 0; i < 2; i++)
    {
        if (mincol < col && 0 < minrow)
        {
            tclearregion(mincol, 0, col - 1, minrow - 1);
        }
        if (0 < col && minrow < row)
        {
            tclearregion(0, minrow, col - 1, row - 1);
        }
        tswapscreen();
        tcursor(CURSOR_LOAD);
    }
    term.c = c;
}

void TerminalEmulator::resettitle(void)
{
    auto dpy = m_dpy.lock();
    if (dpy)
        dpy->SetTitle(NULL);
}

void TerminalEmulator::drawregion(TerminalDisplay &dpy, int x1, int y1, int x2, int y2)
{
    int y;

    for (y = y1; y < y2; y++)
    {
        if (!term.dirty[y])
            continue;

        term.dirty[y] = 0;

        dpy.DrawLine(term.line[y], x1, y, x2);
    }
}

void TerminalEmulator::draw(void)
{
    int cx = term.c.x, ocx = term.ocx, ocy = term.ocy;

    {
        auto dpy = m_dpy.lock();
        if (!dpy)
            return;

        dpy->DrawBegin(term.col, term.row);

        /* adjust cursor position */
        LIMIT(term.ocx, 0, term.col - 1);
        LIMIT(term.ocy, 0, term.row - 1);
        if (term.line[term.ocy][term.ocx].mode & ATTR_WDUMMY)
            term.ocx--;
        if (term.line[term.c.y][cx].mode & ATTR_WDUMMY)
            cx--;

        drawregion(*dpy, 0, 0, term.col, term.row);
        dpy->DrawCursor(cx, term.c.y, term.line[term.c.y][cx],
                        term.ocx, term.ocy, term.line[term.ocy][term.ocx]);
        term.ocx = cx;
        term.ocy = term.c.y;

        dpy->DrawEnd();
    }
    // if (ocx != term.ocx || ocy != term.ocy)
    //     xximspot(term.ocx, term.ocy);
}

void TerminalEmulator::Redraw()
{
    tfulldirt();
    draw();
}

void TerminalEmulator::xsetmode(int set, unsigned int mode)
{
    auto dpy = m_dpy.lock();
    dpy->SetMode((win_mode)mode, set);
}

void TerminalEmulator::xsetpointermotion(int)
{
    // TODO: Figure something out
}

std::unique_ptr<TerminalEmulator> TerminalEmulator::Create(PtyPtr &&pty, ProcPtr &&process, const std::shared_ptr<TerminalDisplay> &display)
{
    if (!pty || !process)
    {
        fprintf(stderr, "Must provide valid pseudoterminal and process");
        return nullptr;
    }

    return std::unique_ptr<TerminalEmulator>(new TerminalEmulator(std::move(pty), std::move(process), display));
}

TerminalEmulator::TerminalEmulator(PtyPtr &&pty, ProcPtr &&process, const std::shared_ptr<TerminalDisplay> &display)
    : m_dpy(display), m_pty(std::move(pty)), m_process(std::move(process)), m_colorsLoaded(false), m_exitCode(1), m_status(STARTING), m_buflen(0), defaultfg(7), defaultbg(0), defaultcs(7), defaultrcs(0), allowaltscreen(1), allowwindowops(1)
{
    memset(m_buf, 0, sizeof(m_buf));
    memset(&term, 0, sizeof(term));
    memset(&sel, 0, sizeof(sel));
    memset(&csiescseq, 0, sizeof(csiescseq));
    memset(&strescseq, 0, sizeof(strescseq));

    int col = m_pty->GetNumColumns();
    int row = m_pty->GetNumRows();

    tnew(col, row);
    if (display)
    {
        display->SetCursorMode((Hexe::Terminal::cursor_mode)cursorshape);
        display->Attach(this);
        LoadColors();
    }
    selinit();
    resettitle();
}

TerminalEmulator::~TerminalEmulator()
{
    {
        auto dpy = m_dpy.lock();
        if (dpy)
            dpy->Detach(this);
    }
    m_process->Terminate();
}

void TerminalEmulator::LogError(const char *msg)
{
    fprintf(stderr, "%s\n", msg);
}

void TerminalEmulator::Terminate()
{
    if (m_status == STARTING || m_status == RUNNING)
    {
        m_process->Terminate();
        m_exitCode = 1;
        m_status = TERMINATED;
    }
}

void TerminalEmulator::OnProcessExit(int exitCode)
{
}

void TerminalEmulator::SetClipboard(const char *str)
{
    auto dpy = m_dpy.lock();
    if (!dpy)
        return;
    dpy->SetClipboard(str);
}

void TerminalEmulator::LoadColors()
{
    auto dpy = m_dpy.lock();
    if (!dpy)
        return;

    dpy->ResetColors();
}

int TerminalEmulator::ResetColor(int i, const char *name)
{
    auto dpy = m_dpy.lock();
    if (!dpy)
        return 0;

    return dpy->ResetColor(i, name);
}

bool TerminalEmulator::HasExited() const
{
    return m_status == TERMINATED;
}

int TerminalEmulator::GetExitCode() const
{
    return m_exitCode;
}

void TerminalEmulator::Resize(int columns, int rows)
{
    if (!m_pty->Resize(columns, rows))
    {
        _die("Failed to resize pty!");
        return;
    }
    tresize(columns, rows);
    Redraw();
}

void TerminalEmulator::Update()
{
    if (m_status == TerminalEmulator::STARTING)
    {
        m_status = TerminalEmulator::RUNNING;
    }
    else if (m_status != TerminalEmulator::RUNNING)
    {
        return;
    }

    int n = 10;
    while (ttyread() > 0 && n > 0)
    {
        --n;
    }

    // TODO: Handle blink

    // TODO: Do not draw every update
    draw();

    m_process->CheckExitStatus();
    if (m_process->HasExited())
    {
        m_exitCode = m_process->GetExitCode();
        m_status = TERMINATED;
        OnProcessExit(m_exitCode);
    }
}
