/*
 * xpick — an X11 file picker for Unix pipelines.
 *
 * Read a list of file and directory names, show them relative to the current
 * working directory, let the user expand directories and check entries, then
 * either write the checked paths relative to the working directory (OK)
 * or report failure on the controlling terminal (Cancel).
 *
 * Printed form is relative to the cwd at startup, with "." and ".." collapsed
 * and a "./" prefix on names that do not already start with a dot. Symlinks
 * are not followed and are not resolved.
 *
 * Build:  make xpick
 * Use:    find . -name '*.c' | xpick | xargs ...
 *
 * SPDX-License-Identifier: MIT
 */

#define _XOPEN_SOURCE 700

#include <X11/Xatom.h>
#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <dirent.h>
#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

#define APP_NAME "xpick"

enum {
	KIND_FILE = 0,
	KIND_DIR,
	KIND_LINK,
	KIND_OTHER,
	KIND_MISSING
};

typedef struct {
	int x, y, w, h;
} Rect;

typedef struct {
	char *path;
	char *name;
	char *extra; /* link target or missing note, display only */
	int parent;
	int first_child;
	int next_sibling;
	int depth;
	int kind;
	int checked;
	int expanded;
	int loaded;
} Node;

static Display *dpy;
static int scr;
static Window win;
static GC gc;
static Pixmap pm;
static int pm_w, pm_h;
static XftDraw *draw;
static XftFont *font;
static Visual *visual;
static Colormap cmap;

static XftColor col_fg, col_bg, col_sel, col_caret, col_muted;
static XftColor col_okbg, col_okfg, col_btn, col_btnfg, col_paper, col_line;
static XftColor col_dir;

static int win_w = 760, win_h = 520;
static int char_w = 8, line_h = 16, ascent = 12;
static int text_x, text_y, text_w, text_h, status_y;
static Rect ok_r, cancel_r, all_r;
static int hot_ok, hot_cancel, hot_all;

static char *cwd_path;
static char *title = "xpick";
static const char *font_name = "monospace:size=13";
static int exit_kind;

static Node *nodes;
static int nnodes, nodes_cap;
static int *visible;
static int nvis, vis_cap;
static int cursor_row;
static int top_row;

static Atom xa_wm_del;

static int tty_msg(const char *msg)
{
	FILE *t = fopen("/dev/tty", "w");
	if (!t)
		return 0;
	fprintf(t, APP_NAME ": %s\n", msg);
	fflush(t);
	fclose(t);
	return 1;
}

static int x_io_error(Display *d)
{
	(void)d;
	if (!tty_msg("lost connection to the X server"))
		fprintf(stderr, APP_NAME ": lost connection to the X server\n");
	_exit(2);
}

static void die(const char *msg, int code)
{
	if (!tty_msg(msg))
		fprintf(stderr, APP_NAME ": %s\n", msg);
	exit(code);
}

static void *xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);
	if (!p)
		die("out of memory", 2);
	return p;
}

static void *xrealloc(void *p, size_t n)
{
	void *q = realloc(p, n ? n : 1);
	if (!q)
		die("out of memory", 2);
	return q;
}

static char *xstrdup(const char *s)
{
	size_t n = strlen(s);
	char *p = xmalloc(n + 1);
	memcpy(p, s, n + 1);
	return p;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: " APP_NAME " [-t title] [-fn font] [-g WxH] [path ...]\n"
		"\n"
		"Read file and directory names (arguments, or one per line on stdin).\n"
		"Paths are resolved against the current working directory. Directories\n"
		"expand in place when clicked; symlinks are shown but not followed.\n"
		"OK writes each checked path, relative to the working directory, to stdout.\n"
		"A name that does not start with '.' is printed as ./name. Cancel writes\n"
		"a message to the controlling terminal and exits 1.\n"
		"\n"
		"Keys: Up/Down move, Space toggles the check, Right expands,\n"
		"Left collapses, Ctrl+A selects all or none, Ctrl+Enter accepts, Esc cancels.\n");
}

static char *get_cwd(void)
{
	size_t n = 256;
	for (;;) {
		char *b = xmalloc(n);
		if (getcwd(b, n))
			return b;
		free(b);
		if (errno != ERANGE)
			die("getcwd failed", 2);
		n *= 2;
	}
}

static char *join_path(const char *dir, const char *name)
{
	size_t ld = strlen(dir);
	size_t ln = strlen(name);
	int need_slash = !(ld > 0 && dir[ld - 1] == '/');
	char *s = xmalloc(ld + ln + 2);
	memcpy(s, dir, ld);
	if (need_slash)
		s[ld++] = '/';
	memcpy(s + ld, name, ln + 1);
	return s;
}

/* Absolute path, "." and ".." collapsed, symlinks not resolved. */
static char *canon(const char *in)
{
	const char *src;
	char *full = NULL;
	char *out;
	char **parts;
	int nparts = 0, cap = 8, i, sp;
	size_t len;

	if (!in || !in[0])
		return NULL;
	if (in[0] == '/')
		full = xstrdup(in);
	else
		full = join_path(cwd_path, in);
	parts = xmalloc((size_t)cap * sizeof *parts);
	src = full;
	while (*src) {
		const char *slash = strchr(src, '/');
		size_t n = slash ? (size_t)(slash - src) : strlen(src);
		if (n == 0 || (n == 1 && src[0] == '.')) {
			/* skip */
		} else if (n == 2 && src[0] == '.' && src[1] == '.') {
			if (nparts > 0) {
				free(parts[nparts - 1]);
				nparts--;
			}
		} else {
			char *bit = xmalloc(n + 1);
			memcpy(bit, src, n);
			bit[n] = 0;
			if (nparts == cap) {
				cap *= 2;
				parts = xrealloc(parts, (size_t)cap * sizeof *parts);
			}
			parts[nparts++] = bit;
		}
		if (!slash)
			break;
		src = slash + 1;
	}
	len = 1;
	for (i = 0; i < nparts; i++)
		len += strlen(parts[i]) + 1;
	out = xmalloc(len + 1);
	out[0] = '/';
	sp = 1;
	for (i = 0; i < nparts; i++) {
		size_t n = strlen(parts[i]);
		if (sp > 1)
			out[sp++] = '/';
		memcpy(out + sp, parts[i], n);
		sp += (int)n;
		free(parts[i]);
	}
	out[sp] = 0;
	free(parts);
	free(full);
	return out;
}

/* Relative to cwd. "foo" becomes "./foo"; the cwd itself is ".". */
static char *relative_out(const char *abs)
{
	const char *cwd = cwd_path;
	size_t cl = strlen(cwd);
	char *out;
	int up, i, nabs, ncwd;
	const char *pa, *pc;

	if (strcmp(abs, cwd) == 0)
		return xstrdup(".");
	if (strcmp(cwd, "/") == 0) {
		out = xmalloc(strlen(abs) + 2);
		sprintf(out, ".%s", abs);
		return out;
	}
	if (strncmp(abs, cwd, cl) == 0 && abs[cl] == '/') {
		out = xmalloc(strlen(abs + cl + 1) + 3);
		sprintf(out, "./%s", abs + cl + 1);
		return out;
	}
	up = 0;
	ncwd = 0;
	for (pc = cwd; *pc; pc++)
		if (*pc == '/')
			ncwd++;
	nabs = 0;
	for (pa = abs; *pa; pa++)
		if (*pa == '/')
			nabs++;
	pc = cwd;
	pa = abs;
	while (*pc && *pa && *pc == *pa) {
		pc++;
		pa++;
	}
	while (pc > cwd && *pc != '/') {
		pc--;
		pa--;
	}
	if (*pc == '/') {
		pc++;
		pa++;
	}
	for (i = 0; pc[i]; i++)
		if (pc[i] == '/')
			up++;
	if (pc[0])
		up++;
	(void)ncwd;
	(void)nabs;
	{
		size_t need = (size_t)up * 3 + strlen(pa) + 2;
		char *p;
		out = xmalloc(need);
		p = out;
		for (i = 0; i < up; i++) {
			memcpy(p, "../", 3);
			p += 3;
		}
		if (*pa) {
			memcpy(p, pa, strlen(pa) + 1);
		} else if (p > out) {
			p[-1] = 0;
		} else {
			strcpy(out, ".");
		}
	}
	return out;
}

static const char *base_name(const char *path)
{
	const char *s = strrchr(path, '/');
	if (!s || !s[1])
		return path;
	return s + 1;
}

static int add_node(void)
{
	Node *n;
	if (nnodes == nodes_cap) {
		nodes_cap = nodes_cap ? nodes_cap * 2 : 32;
		nodes = xrealloc(nodes, (size_t)nodes_cap * sizeof *nodes);
	}
	n = &nodes[nnodes];
	memset(n, 0, sizeof *n);
	n->parent = -1;
	n->first_child = -1;
	n->next_sibling = -1;
	return nnodes++;
}

static void classify(int idx)
{
	struct stat st;
	if (lstat(nodes[idx].path, &st) != 0) {
		nodes[idx].kind = KIND_MISSING;
		nodes[idx].extra = xstrdup("missing");
		return;
	}
	if (S_ISLNK(st.st_mode)) {
		char buf[256];
		ssize_t n = readlink(nodes[idx].path, buf, sizeof buf - 1);
		nodes[idx].kind = KIND_LINK;
		if (n >= 0) {
			buf[n] = 0;
			nodes[idx].extra = xstrdup(buf);
		}
		return;
	}
	if (S_ISDIR(st.st_mode)) {
		nodes[idx].kind = KIND_DIR;
		return;
	}
	if (S_ISREG(st.st_mode)) {
		nodes[idx].kind = KIND_FILE;
		return;
	}
	nodes[idx].kind = KIND_OTHER;
}

static int cmpstr(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static void load_children(int idx)
{
	DIR *d;
	struct dirent *de;
	char **names = NULL;
	int nnames = 0, cap = 0, i, prev;
	if (nodes[idx].loaded || nodes[idx].kind != KIND_DIR)
		return;
	nodes[idx].loaded = 1;
	d = opendir(nodes[idx].path);
	if (!d) {
		nodes[idx].extra = xstrdup(strerror(errno));
		return;
	}
	while ((de = readdir(d)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		if (nnames == cap) {
			cap = cap ? cap * 2 : 16;
			names = xrealloc(names, (size_t)cap * sizeof *names);
		}
		names[nnames++] = xstrdup(de->d_name);
	}
	closedir(d);
	qsort(names, (size_t)nnames, sizeof *names, cmpstr);
	prev = -1;
	for (i = 0; i < nnames; i++) {
		int c = add_node();
		nodes[c].parent = idx;
		nodes[c].depth = nodes[idx].depth + 1;
		nodes[c].name = names[i];
		nodes[c].path = join_path(nodes[idx].path, names[i]);
		classify(c);
		if (prev < 0)
			nodes[idx].first_child = c;
		else
			nodes[prev].next_sibling = c;
		prev = c;
	}
	free(names);
}

static void add_root(const char *raw)
{
	char *path = canon(raw);
	int idx;
	if (!path)
		return;
	idx = add_node();
	nodes[idx].path = path;
	nodes[idx].name = xstrdup(base_name(path));
	nodes[idx].depth = 0;
	classify(idx);
}

static void load_inputs(int argc, char **argv)
{
	int i;
	if (argc > 0) {
		for (i = 0; i < argc; i++)
			add_root(argv[i]);
		return;
	}
	if (isatty(STDIN_FILENO)) {
		add_root(".");
		return;
	}
	for (;;) {
		char buf[8192];
		size_t n = 0;
		int c;
		while ((c = getchar()) != EOF && c != '\n') {
			if (n + 1 < sizeof buf)
				buf[n++] = (char)c;
		}
		buf[n] = 0;
		while (n > 0 && (buf[n - 1] == '\r' || buf[n - 1] == ' ' || buf[n - 1] == '\t'))
			buf[--n] = 0;
		if (n)
			add_root(buf);
		if (c == EOF)
			break;
	}
}

static void append_visible(int idx)
{
	int c;
	if (nvis == vis_cap) {
		vis_cap = vis_cap ? vis_cap * 2 : 64;
		visible = xrealloc(visible, (size_t)vis_cap * sizeof *visible);
	}
	visible[nvis++] = idx;
	if (!nodes[idx].expanded || nodes[idx].kind != KIND_DIR)
		return;
	if (!nodes[idx].loaded)
		load_children(idx);
	for (c = nodes[idx].first_child; c >= 0; c = nodes[c].next_sibling)
		append_visible(c);
}

static void rebuild_visible_real(void)
{
	int i;
	nvis = 0;
	for (i = 0; i < nnodes; i++) {
		if (nodes[i].parent < 0)
			append_visible(i);
	}
	if (cursor_row >= nvis)
		cursor_row = nvis > 0 ? nvis - 1 : 0;
	if (cursor_row < 0)
		cursor_row = 0;
}

static int count_checked(void)
{
	int i, n = 0;
	for (i = 0; i < nnodes; i++)
		if (nodes[i].checked)
			n++;
	return n;
}

static int parse_geom(const char *s)
{
	int w, h;
	if (sscanf(s, "%dx%d", &w, &h) != 2 || w < 200 || h < 160)
		return -1;
	win_w = w;
	win_h = h;
	return 0;
}

static int alloc_color(XftColor *c, const char *name)
{
	if (!XftColorAllocName(dpy, visual, cmap, name, c))
		return -1;
	return 0;
}

static XftFont *open_font(const char *prefer)
{
	static const char *fallbacks[] = {
		"DejaVu Sans Mono:size=13",
		"Liberation Mono:size=13",
		"Nimbus Mono PS:size=13",
		"monospace:size=13",
		"fixed:size=13",
		NULL
	};
	XftFont *f;
	int i;
	if (prefer && (f = XftFontOpenName(dpy, scr, prefer)))
		return f;
	for (i = 0; fallbacks[i]; i++) {
		f = XftFontOpenName(dpy, scr, fallbacks[i]);
		if (f)
			return f;
	}
	return NULL;
}

static void layout(void)
{
	int pad = 12;
	int btn_h = 30;
	int btn_w = 108;
	int gap = 8;
	status_y = win_h - pad - btn_h - gap - line_h - 6;
	text_x = pad;
	text_y = pad;
	text_w = win_w - pad * 2;
	text_h = status_y - gap - text_y;
	if (text_h < line_h)
		text_h = line_h;
	cancel_r.w = btn_w;
	cancel_r.h = btn_h;
	cancel_r.x = win_w - pad - btn_w - gap - btn_w;
	cancel_r.y = win_h - pad - btn_h;
	ok_r = cancel_r;
	ok_r.x = win_w - pad - btn_w;
	all_r.x = pad;
	all_r.y = cancel_r.y;
	all_r.w = 132;
	all_r.h = btn_h;
}

static void ensure_pixmap(void)
{
	if (pm && pm_w == win_w && pm_h == win_h)
		return;
	if (draw)
		XftDrawDestroy(draw);
	if (pm)
		XFreePixmap(dpy, pm);
	pm = XCreatePixmap(dpy, win, (unsigned)win_w, (unsigned)win_h,
			   (unsigned)DefaultDepth(dpy, scr));
	pm_w = win_w;
	pm_h = win_h;
	draw = XftDrawCreate(dpy, pm, visual, cmap);
}

static void fill_rect(int x, int y, int w, int h, unsigned long pixel)
{
	if (w <= 0 || h <= 0)
		return;
	XSetForeground(dpy, gc, pixel);
	XFillRectangle(dpy, pm, gc, x, y, (unsigned)w, (unsigned)h);
}

static void stroke_rect(int x, int y, int w, int h, unsigned long pixel)
{
	XSetForeground(dpy, gc, pixel);
	XDrawRectangle(dpy, pm, gc, x, y, (unsigned)(w - 1), (unsigned)(h - 1));
}

static void draw_button(Rect r, const char *label, int hot, int primary)
{
	XftColor *bg = primary ? &col_okbg : &col_btn;
	XftColor *fg = primary ? &col_okfg : &col_btnfg;
	XGlyphInfo ext;
	int tw, tx, ty;
	fill_rect(r.x, r.y, r.w, r.h, bg->pixel);
	if (hot)
		stroke_rect(r.x, r.y, r.w, r.h, col_caret.pixel);
	else
		stroke_rect(r.x, r.y, r.w, r.h, col_muted.pixel);
	XftTextExtentsUtf8(dpy, font, (FcChar8 *)label, (int)strlen(label), &ext);
	tw = ext.xOff;
	tx = r.x + (r.w - tw) / 2;
	ty = r.y + (r.h + ascent - 2) / 2;
	XftDrawStringUtf8(draw, fg, font, tx, ty, (FcChar8 *)label, (int)strlen(label));
}

static void reveal_cursor(void)
{
	int vis_rows = text_h / line_h;
	if (vis_rows < 1)
		vis_rows = 1;
	if (cursor_row < top_row)
		top_row = cursor_row;
	if (cursor_row >= top_row + vis_rows)
		top_row = cursor_row - vis_rows + 1;
	if (top_row < 0)
		top_row = 0;
}

static int all_checked(void);

static void draw_all(void)
{
	int vis_rows, i, y;
	char status[192];
	XRectangle clip;
	ensure_pixmap();
	layout();
	fill_rect(0, 0, win_w, win_h, col_bg.pixel);
	fill_rect(text_x - 4, text_y - 4, text_w + 8, text_h + 8, col_paper.pixel);
	stroke_rect(text_x - 4, text_y - 4, text_w + 8, text_h + 8, col_line.pixel);
	clip.x = (short)text_x;
	clip.y = (short)text_y;
	clip.width = (unsigned short)(text_w > 0 ? text_w : 1);
	clip.height = (unsigned short)(text_h > 0 ? text_h : 1);
	XftDrawSetClipRectangles(draw, 0, 0, &clip, 1);
	vis_rows = text_h / line_h;
	if (vis_rows < 1)
		vis_rows = 1;
	for (i = 0, y = text_y; i < vis_rows && top_row + i < nvis; i++, y += line_h) {
		int ni = visible[top_row + i];
		Node *n = &nodes[ni];
		int x = text_x + n->depth * 2 * char_w;
		char box[4];
		char mark[4];
		XftColor *name_col = n->kind == KIND_DIR ? &col_dir : &col_fg;
		if (top_row + i == cursor_row)
			fill_rect(text_x, y, text_w, line_h, col_sel.pixel);
		snprintf(box, sizeof box, n->checked ? "[x]" : "[ ]");
		if (n->kind == KIND_DIR)
			snprintf(mark, sizeof mark, n->expanded ? "v " : "> ");
		else if (n->kind == KIND_LINK)
			snprintf(mark, sizeof mark, "@ ");
		else
			snprintf(mark, sizeof mark, "  ");
		XftDrawStringUtf8(draw, &col_fg, font, x, y + ascent, (FcChar8 *)box, 3);
		XftDrawStringUtf8(draw, &col_muted, font, x + 4 * char_w, y + ascent,
				  (FcChar8 *)mark, 2);
		XftDrawStringUtf8(draw, name_col, font, x + 6 * char_w, y + ascent,
				  (FcChar8 *)n->name, (int)strlen(n->name));
		if (n->extra) {
			XGlyphInfo ext;
			int nx;
			char note[128];
			XftTextExtentsUtf8(dpy, font, (FcChar8 *)n->name, (int)strlen(n->name), &ext);
			nx = x + 6 * char_w + ext.xOff + char_w;
			if (n->kind == KIND_LINK)
				snprintf(note, sizeof note, "-> %s", n->extra);
			else
				snprintf(note, sizeof note, "(%s)", n->extra);
			XftDrawStringUtf8(draw, &col_muted, font, nx, y + ascent,
					  (FcChar8 *)note, (int)strlen(note));
		}
	}
	XftDrawSetClip(draw, NULL);
	snprintf(status, sizeof status,
		 "%d selected    %d shown          Space toggles    Right expands    Ctrl+Enter accepts",
		 count_checked(), nvis);
	XftDrawStringUtf8(draw, &col_muted, font, text_x, status_y + ascent,
			  (FcChar8 *)status, (int)strlen(status));
	draw_button(all_r, all_checked() ? "Select none" : "Select all", hot_all, 0);
	draw_button(cancel_r, "Cancel", hot_cancel, 0);
	draw_button(ok_r, "OK", hot_ok, 1);
	XCopyArea(dpy, pm, win, gc, 0, 0, (unsigned)win_w, (unsigned)win_h, 0, 0);
}

static int in_rect(Rect r, int x, int y)
{
	return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static int all_checked(void)
{
	int i;
	if (nnodes == 0)
		return 0;
	for (i = 0; i < nnodes; i++)
		if (!nodes[i].checked)
			return 0;
	return 1;
}

static void select_all_or_none(void)
{
	int on = !all_checked();
	int i;
	for (i = 0; i < nnodes; i++)
		nodes[i].checked = on;
}

static void emit_checked(int idx)
{
	int c;
	if (nodes[idx].checked) {
		char *rel = relative_out(nodes[idx].path);
		size_t n = strlen(rel);
		size_t off = 0;
		while (off < n) {
			ssize_t w = write(STDOUT_FILENO, rel + off, n - off);
			if (w < 0) {
				if (errno == EINTR)
					continue;
				free(rel);
				die("write to stdout failed", 2);
			}
			off += (size_t)w;
		}
		free(rel);
		while (write(STDOUT_FILENO, "\n", 1) < 0) {
			if (errno != EINTR)
				die("write to stdout failed", 2);
		}
	}
	for (c = nodes[idx].first_child; c >= 0; c = nodes[c].next_sibling)
		emit_checked(c);
}

static void accept(void)
{
	int i;
	for (i = 0; i < nnodes; i++) {
		if (nodes[i].parent < 0)
			emit_checked(i);
	}
	exit_kind = 0;
}

static void cancel(void)
{
	if (!tty_msg("cancelled"))
		fprintf(stderr, APP_NAME ": cancelled\n");
	exit_kind = 1;
}

static void toggle_expand(int ni)
{
	if (nodes[ni].kind != KIND_DIR)
		return;
	nodes[ni].expanded = !nodes[ni].expanded;
	if (nodes[ni].expanded)
		load_children(ni);
	rebuild_visible_real();
}

static void toggle_check(int ni)
{
	nodes[ni].checked = !nodes[ni].checked;
}

static void on_row_click(int row, int x)
{
	int ni, indent, box_r;
	if (row < 0 || row >= nvis)
		return;
	cursor_row = row;
	ni = visible[row];
	indent = text_x + nodes[ni].depth * 2 * char_w;
	box_r = indent + 3 * char_w;
	if (x < box_r + char_w)
		toggle_check(ni);
	else if (nodes[ni].kind == KIND_DIR)
		toggle_expand(ni);
	else
		toggle_check(ni);
	reveal_cursor();
}

static void scroll_by(int delta)
{
	int vis_rows = text_h / line_h;
	if (vis_rows < 1)
		vis_rows = 1;
	top_row += delta;
	if (top_row < 0)
		top_row = 0;
	if (nvis <= vis_rows)
		top_row = 0;
	else if (top_row > nvis - vis_rows)
		top_row = nvis - vis_rows;
}

static void move_row(int delta)
{
	cursor_row += delta;
	if (cursor_row < 0)
		cursor_row = 0;
	if (nvis == 0)
		cursor_row = 0;
	else if (cursor_row >= nvis)
		cursor_row = nvis - 1;
	reveal_cursor();
}

static void handle_key(XKeyEvent *ev)
{
	KeySym ks = 0;
	int shift = (ev->state & ShiftMask) != 0;
	int ctrl = (ev->state & ControlMask) != 0;
	char buf[8];
	XLookupString(ev, buf, (int)sizeof buf, &ks, NULL);
	if (ctrl && (ks == XK_Return || ks == XK_KP_Enter || ks == XK_s || ks == XK_S)) {
		accept();
		return;
	}
	if (ks == XK_Escape || (ctrl && (ks == XK_g || ks == XK_G))) {
		cancel();
		return;
	}
	if (ctrl && (ks == XK_a || ks == XK_A)) {
		select_all_or_none();
		return;
	}
	if (ks == XK_Up || ks == XK_KP_Up) {
		move_row(-1);
		return;
	}
	if (ks == XK_Down || ks == XK_KP_Down) {
		move_row(1);
		return;
	}
	if (ks == XK_Page_Up || ks == XK_KP_Page_Up) {
		move_row(-(text_h / line_h > 1 ? text_h / line_h - 1 : 1));
		return;
	}
	if (ks == XK_Page_Down || ks == XK_KP_Page_Down) {
		move_row(text_h / line_h > 1 ? text_h / line_h - 1 : 1);
		return;
	}
	if (ks == XK_Home) {
		cursor_row = 0;
		reveal_cursor();
		return;
	}
	if (ks == XK_End) {
		cursor_row = nvis > 0 ? nvis - 1 : 0;
		reveal_cursor();
		return;
	}
	if (nvis == 0)
		return;
	if (ks == XK_space) {
		toggle_check(visible[cursor_row]);
		return;
	}
	if (ks == XK_Right || ks == XK_KP_Right || ks == XK_Return) {
		int ni = visible[cursor_row];
		if (nodes[ni].kind == KIND_DIR && !nodes[ni].expanded)
			toggle_expand(ni);
		else if (nodes[ni].kind == KIND_DIR && nodes[ni].first_child >= 0)
			move_row(1);
		return;
	}
	if (ks == XK_Left || ks == XK_KP_Left) {
		int ni = visible[cursor_row];
		if (nodes[ni].kind == KIND_DIR && nodes[ni].expanded)
			toggle_expand(ni);
		else if (nodes[ni].parent >= 0) {
			int p = nodes[ni].parent;
			int r;
			for (r = 0; r < nvis; r++) {
				if (visible[r] == p) {
					cursor_row = r;
					break;
				}
			}
			reveal_cursor();
		}
		return;
	}
	(void)shift;
}

static void open_ui(void)
{
	XSetWindowAttributes attr;
	XSizeHints hints;
	XClassHint classh;
	XWMHints wmh;
	unsigned long mask;

	dpy = XOpenDisplay(NULL);
	if (!dpy)
		die("cannot open display (is DISPLAY set?)", 2);
	XSetIOErrorHandler(x_io_error);
	scr = DefaultScreen(dpy);
	visual = DefaultVisual(dpy, scr);
	cmap = DefaultColormap(dpy, scr);
	font = open_font(font_name);
	if (!font)
		die("cannot open an Xft font", 2);
	ascent = font->ascent;
	line_h = font->ascent + font->descent + 2;
	{
		XGlyphInfo ext;
		XftTextExtentsUtf8(dpy, font, (FcChar8 *)"M", 1, &ext);
		char_w = ext.xOff > 0 ? ext.xOff : 8;
	}
	if (alloc_color(&col_fg, "#1c1914") ||
	    alloc_color(&col_bg, "#efeae2") ||
	    alloc_color(&col_paper, "#fbf8f2") ||
	    alloc_color(&col_sel, "#d4e4f5") ||
	    alloc_color(&col_caret, "#b84324") ||
	    alloc_color(&col_muted, "#6e675c") ||
	    alloc_color(&col_okbg, "#245c45") ||
	    alloc_color(&col_okfg, "#f7f4ee") ||
	    alloc_color(&col_btn, "#e4ddd0") ||
	    alloc_color(&col_btnfg, "#1c1914") ||
	    alloc_color(&col_line, "#cfc6b8") ||
	    alloc_color(&col_dir, "#1e4d78"))
		die("cannot allocate colours", 2);

	attr.background_pixel = col_bg.pixel;
	attr.event_mask = ExposureMask | KeyPressMask | ButtonPressMask |
			  ButtonReleaseMask | PointerMotionMask | StructureNotifyMask;
	attr.colormap = cmap;
	mask = CWBackPixel | CWEventMask | CWColormap;
	win = XCreateWindow(dpy, RootWindow(dpy, scr), 80, 80,
			    (unsigned)win_w, (unsigned)win_h, 0,
			    DefaultDepth(dpy, scr), InputOutput, visual, mask, &attr);
	gc = XCreateGC(dpy, win, 0, NULL);
	xa_wm_del = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(dpy, win, &xa_wm_del, 1);
	hints.flags = PMinSize | PSize;
	hints.min_width = 420;
	hints.min_height = 220;
	hints.width = win_w;
	hints.height = win_h;
	XSetWMNormalHints(dpy, win, &hints);
	classh.res_name = "xpick";
	classh.res_class = "Xpick";
	XSetClassHint(dpy, win, &classh);
	wmh.flags = InputHint;
	wmh.input = True;
	XSetWMHints(dpy, win, &wmh);
	XStoreName(dpy, win, title);
	XSetIconName(dpy, win, title);
	{
		XTextProperty tp;
		if (Xutf8TextListToTextProperty(dpy, &title, 1, XUTF8StringStyle, &tp) == Success) {
			XSetWMName(dpy, win, &tp);
			XFree(tp.value);
		}
	}
	XStoreName(dpy, win, title);
	XMapRaised(dpy, win);
	XFlush(dpy);
}

static void run(void)
{
	int xfd = ConnectionNumber(dpy);
	exit_kind = -1;
	while (exit_kind < 0) {
		fd_set fds;
		struct timeval tv;
		int r;
		while (XPending(dpy)) {
			XEvent ev;
			XNextEvent(dpy, &ev);
			if (ev.type == KeyPress && XLookupKeysym(&ev.xkey, 0) == XK_Escape) {
				cancel();
				break;
			}
			switch (ev.type) {
			case Expose:
				if (ev.xexpose.count == 0)
					draw_all();
				break;
			case ConfigureNotify:
				if (ev.xconfigure.width != win_w || ev.xconfigure.height != win_h) {
					win_w = ev.xconfigure.width;
					win_h = ev.xconfigure.height;
					draw_all();
				}
				break;
			case KeyPress:
				handle_key(&ev.xkey);
				if (exit_kind < 0)
					draw_all();
				break;
			case ButtonPress:
				if (ev.xbutton.button == Button1) {
					if (in_rect(all_r, ev.xbutton.x, ev.xbutton.y)) {
						select_all_or_none();
						draw_all();
						break;
					}
					if (in_rect(ok_r, ev.xbutton.x, ev.xbutton.y)) {
						accept();
						break;
					}
					if (in_rect(cancel_r, ev.xbutton.x, ev.xbutton.y)) {
						cancel();
						break;
					}
					if (ev.xbutton.y >= text_y && ev.xbutton.y < text_y + text_h) {
						int row = top_row + (ev.xbutton.y - text_y) / line_h;
						on_row_click(row, ev.xbutton.x);
						draw_all();
					}
				} else if (ev.xbutton.button == Button4) {
					scroll_by(-3);
					draw_all();
				} else if (ev.xbutton.button == Button5) {
					scroll_by(3);
					draw_all();
				}
				break;
			case MotionNotify: {
				int o1 = in_rect(ok_r, ev.xmotion.x, ev.xmotion.y);
				int o2 = in_rect(cancel_r, ev.xmotion.x, ev.xmotion.y);
				int o3 = in_rect(all_r, ev.xmotion.x, ev.xmotion.y);
				if (o1 != hot_ok || o2 != hot_cancel || o3 != hot_all) {
					hot_ok = o1;
					hot_cancel = o2;
					hot_all = o3;
					draw_all();
				}
				break;
			}
			case ClientMessage:
				if ((Atom)ev.xclient.data.l[0] == xa_wm_del)
					cancel();
				break;
			default:
				break;
			}
		}
		if (exit_kind >= 0)
			break;
		FD_ZERO(&fds);
		FD_SET(xfd, &fds);
		tv.tv_sec = 0;
		tv.tv_usec = 200000;
		r = select(xfd + 1, &fds, NULL, NULL, &tv);
		if (r < 0 && errno == EINTR)
			continue;
	}
}

int main(int argc, char **argv)
{
	int i;
	char **paths = NULL;
	int npaths = 0;
	setlocale(LC_ALL, "");
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			usage();
			return 0;
		}
		if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
			title = argv[++i];
			continue;
		}
		if (strcmp(argv[i], "-fn") == 0 && i + 1 < argc) {
			font_name = argv[++i];
			continue;
		}
		if (strcmp(argv[i], "-g") == 0 && i + 1 < argc) {
			if (parse_geom(argv[++i]) != 0)
				die("bad geometry, want WIDTHxHEIGHT", 2);
			continue;
		}
		if (argv[i][0] == '-' && argv[i][1]) {
			usage();
			return 2;
		}
		paths = xrealloc(paths, (size_t)(npaths + 1) * sizeof *paths);
		paths[npaths++] = argv[i];
	}
	cwd_path = get_cwd();
	load_inputs(npaths, paths);
	free(paths);
	rebuild_visible_real();
	open_ui();
	run();
	XCloseDisplay(dpy);
	return exit_kind == 0 ? 0 : 1;
}
