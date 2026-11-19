/* SPDX-License-Identifier: Zlib */

/*
 * Text cursor, visual selection and PDF annotations.
 *
 * Two girara modes are added on top of normal mode:
 *
 *   cursor  a vim-like caret over the page's text layer
 *   visual  a selection between an anchor and the caret, charwise or linewise
 *
 * A page's text layer is fetched from the plugin once and cached as a flat
 * array of characters in reading order. A position is (page, index into that
 * array). Annotations are fetched per page and cached until they change.
 *
 * Every change is written to the file at once (an incremental save) and pushed
 * on an undo stack. The file monitor would reload the document after our own
 * write; we remember the file's mtime and size after each save so that the
 * reload can be skipped.
 *
 * Keys are listed in the binding table at the end of the file.
 */

#include <glib/gi18n.h>
#include <girara-gtk/session.h>
#include <girara-gtk/settings.h>
#include <girara-gtk/shortcuts.h>
#include <girara-gtk/statusbar.h>
#include <girara/datastructures.h>
#include <gtk/gtk.h>
#include <math.h>

#include "adjustment.h"
#include "annotations.h"
#include "document-widget.h"
#include "document.h"
#include "page-widget.h"
#include "page.h"
#include "shortcuts.h"
#include "utils.h"
#include "zathura.h"

#define N_COLORS 4
#define UNDO_DEPTH 200

static const char* const default_colors[N_COLORS] = {"#ffe135", "#8ee07a", "#7cc4ff", "#ff9ecf"};
static const char* const default_cursor_color     = "#ff6a00";

struct pos {
  int page;
  int idx; /* index into the page's character array */
};

struct page_cache {
  GArray* chars;         /* zathura_text_char_t; NULL until first use */
  girara_list_t* annots; /* zathura_annotation_t; NULL until first use or after a change */
};

enum undo_kind { UNDO_ADD, UNDO_DELETE, UNDO_EDIT };

struct undo {
  enum undo_kind kind;
  int page;
  int id;          /* the annotation this entry is about */
  GArray* rects;   /* UNDO_DELETE: what to re-create */
  double color[3]; /* UNDO_DELETE */
  char* note;      /* UNDO_DELETE: its note; UNDO_EDIT: the previous note */
};

enum prompt_kind { PROMPT_NONE, PROMPT_NEW, PROMPT_EDIT };

struct file_stamp {
  gint64 mtime;
  goffset size;
};

struct zathura_annot_s {
  zathura_t* zathura;
  girara_mode_t cursor_mode;
  girara_mode_t visual_mode;

  bool has_cursor;
  bool linewise; /* visual mode selects whole lines */
  struct pos cursor;
  struct pos anchor; /* the other end of the visual selection */
  double goal_x;     /* column kept across j/k, like vim's curswant */

  GHashTable* cache; /* page index -> struct page_cache */
  GPtrArray* undo;   /* struct undo, oldest first */

  int noted_id; /* annotation whose note was announced last */

  struct {
    enum prompt_kind kind;
    int color; /* PROMPT_NEW */
    int page;  /* PROMPT_EDIT */
    int id;    /* PROMPT_EDIT */
  } prompt;

  bool saved_valid;
  struct file_stamp saved; /* the file as we last wrote it */

  girara_statusbar_item_t* status;
};

typedef zathura_annot_t annot_t;

#define CHAR(arr, i) (&g_array_index((arr), zathura_text_char_t, (i)))

/* ------------------------------------------------------------------------ */
/* lookup                                                                   */

static annot_t* from_session(girara_session_t* session) {
  zathura_t* zathura = session != NULL ? session->global.data : NULL;
  return zathura != NULL ? zathura->annot : NULL;
}

static int page_count(annot_t* an) {
  zathura_document_t* doc = zathura_get_document(an->zathura);
  return doc != NULL ? (int)zathura_document_get_number_of_pages(doc) : 0;
}

static zathura_page_t* page_get(annot_t* an, int page) {
  if (page < 0 || page >= page_count(an)) {
    return NULL;
  }
  return zathura_document_get_page(zathura_get_document(an->zathura), page);
}

static GtkWidget* page_widget(annot_t* an, int page) {
  zathura_page_t* p = page_get(an, page);
  return p != NULL ? zathura_page_get_widget(an->zathura, p) : NULL;
}

static bool mode_is(annot_t* an, girara_mode_t mode) {
  return girara_mode_get(an->zathura->ui.session) == mode;
}

static bool active(annot_t* an) {
  return mode_is(an, an->cursor_mode) || mode_is(an, an->visual_mode);
}

/* ------------------------------------------------------------------------ */
/* page cache                                                               */

static void cache_free(void* data) {
  struct page_cache* pc = data;

  if (pc->chars != NULL) {
    g_array_free(pc->chars, TRUE);
  }
  if (pc->annots != NULL) {
    girara_list_free(pc->annots);
  }
  g_free(pc);
}

static struct page_cache* cache_get(annot_t* an, int page) {
  struct page_cache* pc = g_hash_table_lookup(an->cache, GINT_TO_POINTER(page));

  if (pc == NULL) {
    pc = g_new0(struct page_cache, 1);
    g_hash_table_insert(an->cache, GINT_TO_POINTER(page), pc);
  }
  return pc;
}

/* The page's characters in reading order. Never NULL; empty without a text layer. */
static GArray* chars_of(annot_t* an, int page) {
  struct page_cache* pc = cache_get(an, page);

  if (pc->chars == NULL) {
    zathura_page_t* p   = page_get(an, page);
    girara_list_t* list = p != NULL ? zathura_page_get_chars(p, NULL) : NULL;

    pc->chars = g_array_new(FALSE, FALSE, sizeof(zathura_text_char_t));
    for (size_t i = 0; list != NULL && i < girara_list_size(list); i++) {
      g_array_append_vals(pc->chars, girara_list_nth(list, i), 1);
    }
    if (list != NULL) {
      girara_list_free(list);
    }
  }
  return pc->chars;
}

static girara_list_t* annots_of(annot_t* an, int page) {
  struct page_cache* pc = cache_get(an, page);

  if (pc->annots == NULL) {
    zathura_page_t* p = page_get(an, page);

    pc->annots = p != NULL ? zathura_page_get_annotations(p, NULL) : NULL;
    if (pc->annots == NULL) {
      pc->annots = girara_list_new();
    }
  }
  return pc->annots;
}

/* Forget the page's annotations and re-render it, so that MuPDF draws the change. */
static void annots_dirty(annot_t* an, int page) {
  struct page_cache* pc = cache_get(an, page);
  GtkWidget* w          = page_widget(an, page);

  if (pc->annots != NULL) {
    girara_list_free(pc->annots);
    pc->annots = NULL;
  }
  if (w != NULL) {
    zathura_page_widget_update_surface(ZATHURA_PAGE(w), NULL, true);
    gtk_widget_queue_draw(w);
  }
}

static const zathura_text_char_t* char_at(annot_t* an, struct pos p) {
  GArray* chars = chars_of(an, p.page);

  if (p.idx < 0 || (guint)p.idx >= chars->len) {
    return NULL;
  }
  return CHAR(chars, p.idx);
}

/* ------------------------------------------------------------------------ */
/* geometry                                                                 */

static bool rect_contains(const zathura_rectangle_t* r, double x, double y) {
  return x >= r->x1 && x <= r->x2 && y >= r->y1 && y <= r->y2;
}

static void rect_union(zathura_rectangle_t* r, const zathura_rectangle_t* s) {
  r->x1 = MIN(r->x1, s->x1);
  r->y1 = MIN(r->y1, s->y1);
  r->x2 = MAX(r->x2, s->x2);
  r->y2 = MAX(r->y2, s->y2);
}

static double center_x(const zathura_rectangle_t* r) {
  return (r->x1 + r->x2) / 2;
}

static double center_y(const zathura_rectangle_t* r) {
  return (r->y1 + r->y2) / 2;
}

/* Distance from a coordinate to the interval [lo, hi]; 0 inside. */
static double gap(double v, double lo, double hi) {
  return v < lo ? lo - v : (v > hi ? v - hi : 0);
}

/* Map a page rectangle into document-widget (viewport) coordinates. */
static bool to_viewport(annot_t* an, int page, zathura_rectangle_t r, zathura_rectangle_t* out) {
  GtkWidget* view = an->zathura->ui.document_widget;
  GtkWidget* w    = page_widget(an, page);
  int ax, ay, bx, by;

  if (w == NULL || gtk_widget_get_mapped(w) == FALSE) {
    return false;
  }
  r = recalc_rectangle(page_get(an, page), r);
  if (gtk_widget_translate_coordinates(w, view, (int)r.x1, (int)r.y1, &ax, &ay) == FALSE ||
      gtk_widget_translate_coordinates(w, view, (int)r.x2, (int)r.y2, &bx, &by) == FALSE) {
    return false;
  }
  *out = (zathura_rectangle_t){MIN(ax, bx), MIN(ay, by), MAX(ax, bx), MAX(ay, by)};
  return true;
}

static bool on_screen(annot_t* an, struct pos p) {
  GtkWidget* view              = an->zathura->ui.document_widget;
  const zathura_text_char_t* c = char_at(an, p);
  zathura_rectangle_t r;

  if (c == NULL || page_is_visible(an->zathura, p.page) == false || to_viewport(an, p.page, c->box, &r) == false) {
    return false;
  }
  return r.y1 >= 0 && r.y2 <= gtk_widget_get_allocated_height(view) && r.x2 >= 0 &&
         r.x1 <= gtk_widget_get_allocated_width(view);
}

/* Center the view on a page rectangle; the same computation search results use. */
static void center_on(annot_t* an, int page, zathura_rectangle_t r) {
  ZathuraDocumentWidget* dw = ZATHURA_DOCUMENT_WIDGET(an->zathura->ui.document_widget);
  unsigned int cell_h = 0, cell_w = 0, doc_h = 0, doc_w = 0;
  double pos_x, pos_y;

  zathura_document_widget_get_cell_size(dw, page, &cell_h, &cell_w);
  zathura_document_widget_get_document_size(dw, &doc_h, &doc_w);
  if (doc_h == 0 || doc_w == 0) {
    return;
  }

  r = recalc_rectangle(page_get(an, page), r);
  page_number_to_position(an->zathura, page, 0.5, 0.5, &pos_x, &pos_y);
  pos_y += (center_y(&r) - cell_h / 2.0) / doc_h;
  if (center_x(&r) < 0 || center_x(&r) > gtk_widget_get_allocated_width(GTK_WIDGET(dw))) {
    pos_x += (center_x(&r) - cell_w / 2.0) / doc_w;
  }
  position_set(an->zathura, pos_x, pos_y);
}

static void scroll_by(GtkAdjustment* adj, double delta) {
  if (delta != 0) {
    gtk_adjustment_set_value(adj, gtk_adjustment_get_value(adj) + delta);
  }
}

/* Scroll as little as possible to keep the cursor in view, with a margin. */
static void scroll_to_cursor(annot_t* an) {
  GtkWidget* view              = an->zathura->ui.document_widget;
  const zathura_text_char_t* c = char_at(an, an->cursor);
  const double vh              = gtk_widget_get_allocated_height(view);
  const double vw              = gtk_widget_get_allocated_width(view);
  const double margin          = MIN(48.0, vh / 6);
  double dx = 0, dy = 0;
  zathura_rectangle_t r;

  if (c == NULL) {
    return;
  }
  if (page_is_visible(an->zathura, an->cursor.page) == false || to_viewport(an, an->cursor.page, c->box, &r) == false) {
    center_on(an, an->cursor.page, c->box);
    return;
  }

  if (r.y1 < margin) {
    dy = r.y1 - margin;
  } else if (r.y2 > vh - margin) {
    dy = r.y2 - (vh - margin);
  }
  if (r.x1 < 0) {
    dx = r.x1 - margin;
  } else if (r.x2 > vw) {
    dx = r.x2 - vw + margin;
  }

  if (fabs(dy) > vh) {
    center_on(an, an->cursor.page, c->box);
    return;
  }
  scroll_by(gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(view)), dy);
  scroll_by(gtk_scrollable_get_hadjustment(GTK_SCROLLABLE(view)), dx);
}

/* ------------------------------------------------------------------------ */
/* annotations under the cursor, status bar                                 */

static bool annot_covers(const zathura_annotation_t* a, double x, double y) {
  if (a->n_rects == 0) {
    return rect_contains(&a->bounds, x, y);
  }
  for (size_t i = 0; i < a->n_rects; i++) {
    if (rect_contains(&a->rects[i], x, y)) {
      return true;
    }
  }
  return false;
}

static zathura_annotation_t* annot_at(annot_t* an, struct pos p) {
  const zathura_text_char_t* c = char_at(an, p);
  girara_list_t* list;

  if (c == NULL) {
    return NULL;
  }
  list = annots_of(an, p.page);
  for (size_t i = 0; i < girara_list_size(list); i++) {
    zathura_annotation_t* a = girara_list_nth(list, i);
    if (annot_covers(a, center_x(&c->box), center_y(&c->box))) {
      return a;
    }
  }
  return NULL;
}

static zathura_annotation_t* annot_by_id(annot_t* an, int page, int id) {
  girara_list_t* list = annots_of(an, page);

  for (size_t i = 0; i < girara_list_size(list); i++) {
    zathura_annotation_t* a = girara_list_nth(list, i);
    if (a->id == id) {
      return a;
    }
  }
  return NULL;
}

static void status_set(annot_t* an, const char* text) {
  girara_session_t* session = an->zathura->ui.session;

  if (an->status == NULL) {
    an->status = girara_statusbar_item_add(session, FALSE, FALSE, FALSE, NULL);
  }
  if (an->status != NULL) {
    girara_statusbar_item_set_text(session, an->status, text);
  }
}

static void status_update(annot_t* an) {
  zathura_annotation_t* a;
  const char* mode;
  const char* tag;

  if (active(an) == false) {
    status_set(an, "");
    an->noted_id = -1;
    return;
  }

  a    = an->has_cursor ? annot_at(an, an->cursor) : NULL;
  mode = mode_is(an, an->cursor_mode) ? "CURSOR" : an->linewise ? "V-LINE" : "VISUAL";
  tag  = a == NULL ? "" : a->contents != NULL ? "  [note]" : "  [highlight]";

  g_autofree char* text = g_strdup_printf("   -- %s --%s  ", mode, tag);
  status_set(an, text);

  /* Announce a note once, when the cursor moves onto it. */
  if ((a != NULL ? a->id : -1) == an->noted_id) {
    return;
  }
  an->noted_id = a != NULL ? a->id : -1;
  if (a != NULL && a->contents != NULL) {
    g_autofree char* note = g_markup_escape_text(a->contents, -1);
    girara_notify(an->zathura->ui.session, GIRARA_INFO, "Note: %s", note);
  }
}

/* ------------------------------------------------------------------------ */
/* moving through text                                                      */

static int pos_cmp(struct pos a, struct pos b) {
  if (a.page != b.page) {
    return a.page < b.page ? -1 : 1;
  }
  return (a.idx > b.idx) - (a.idx < b.idx);
}

/* The first page from `page` on, in direction `dir`, that has text; -1 if none. */
static int page_with_text(annot_t* an, int page, int dir) {
  for (; page >= 0 && page < page_count(an); page += dir) {
    if (chars_of(an, page)->len > 0) {
      return page;
    }
  }
  return -1;
}

/* Move one character in direction `dir`, crossing pages. False at either end. */
static bool step(annot_t* an, struct pos* p, int dir) {
  int idx = p->idx + dir;
  int page;

  if (idx >= 0 && (guint)idx < chars_of(an, p->page)->len) {
    p->idx = idx;
    return true;
  }
  page = page_with_text(an, p->page + dir, dir);
  if (page < 0) {
    return false;
  }
  p->page = page;
  p->idx  = dir > 0 ? 0 : (int)chars_of(an, page)->len - 1;
  return true;
}

static bool same_line(annot_t* an, struct pos a, struct pos b) {
  return a.page == b.page && char_at(an, a)->line == char_at(an, b)->line;
}

static bool same_block(annot_t* an, struct pos a, struct pos b) {
  return a.page == b.page && char_at(an, a)->block == char_at(an, b)->block;
}

/* First and last index of the line containing p. */
static void line_span(annot_t* an, struct pos p, int* first, int* last) {
  GArray* chars     = chars_of(an, p.page);
  unsigned int line = CHAR(chars, p.idx)->line;

  *first = *last = p.idx;
  while (*first > 0 && CHAR(chars, *first - 1)->line == line) {
    (*first)--;
  }
  while ((guint)(*last + 1) < chars->len && CHAR(chars, *last + 1)->line == line) {
    (*last)++;
  }
}

enum char_class { CLASS_SPACE, CLASS_WORD, CLASS_PUNCT };

static enum char_class class_at(annot_t* an, struct pos p) {
  gunichar c = char_at(an, p)->c;

  if (c == 0 || g_unichar_isspace(c)) {
    return CLASS_SPACE;
  }
  if (g_unichar_isalnum(c) || g_unichar_ismark(c) || c == '_') {
    return CLASS_WORD;
  }
  return CLASS_PUNCT;
}

/* Move p to the last character of its word in direction `dir`. */
static void word_extend(annot_t* an, struct pos* p, int dir) {
  enum char_class class = class_at(an, *p);
  struct pos next       = *p;

  while (step(an, &next, dir) && same_line(an, *p, next) && class_at(an, next) == class) {
    *p = next;
  }
}

/* Move p over whitespace (including line breaks) in direction `dir`. */
static void skip_space(annot_t* an, struct pos* p, int dir) {
  while (class_at(an, *p) == CLASS_SPACE && step(an, p, dir)) {
  }
}

enum motion_id {
  M_LEFT,
  M_RIGHT,
  M_DOWN,
  M_UP,
  M_WORD,
  M_WORD_BACK,
  M_WORD_END,
  M_LINE_START,
  M_LINE_TEXT,
  M_LINE_END,
  M_BLOCK_NEXT,
  M_BLOCK_PREV,
  M_PAGE_FIRST,
  M_PAGE_LAST,
  M_PAGE_NEXT,
  M_PAGE_PREV,
  M_COUNT,
};

static void mv_left(annot_t* an, struct pos* p) {
  step(an, p, -1);
}

static void mv_right(annot_t* an, struct pos* p) {
  step(an, p, 1);
}

/* Next or previous line, landing on the character nearest the goal column. */
static void mv_line(annot_t* an, struct pos* p, int dir) {
  struct pos q = *p;
  double best  = INFINITY;
  int first, last;

  line_span(an, q, &first, &last);
  q.idx = dir > 0 ? last : first;
  if (step(an, &q, dir) == false) {
    return;
  }

  line_span(an, q, &first, &last);
  for (int i = first; i <= last; i++) {
    const zathura_rectangle_t* b = &CHAR(chars_of(an, q.page), i)->box;
    double d                     = gap(an->goal_x, b->x1, b->x2);

    if (d < best) {
      best  = d;
      q.idx = i;
    }
  }
  *p = q;
}

static void mv_down(annot_t* an, struct pos* p) {
  mv_line(an, p, 1);
}

static void mv_up(annot_t* an, struct pos* p) {
  mv_line(an, p, -1);
}

/* w: start of the next word */
static void mv_word(annot_t* an, struct pos* p) {
  if (class_at(an, *p) != CLASS_SPACE) {
    word_extend(an, p, 1);
    if (step(an, p, 1) == false) {
      return;
    }
  }
  skip_space(an, p, 1);
}

/* e: end of this or the next word */
static void mv_word_end(annot_t* an, struct pos* p) {
  if (step(an, p, 1)) {
    skip_space(an, p, 1);
    word_extend(an, p, 1);
  }
}

/* b: start of this or the previous word */
static void mv_word_back(annot_t* an, struct pos* p) {
  if (step(an, p, -1)) {
    skip_space(an, p, -1);
    word_extend(an, p, -1);
  }
}

/* 0 */
static void mv_line_start(annot_t* an, struct pos* p) {
  int first, last;

  line_span(an, *p, &first, &last);
  p->idx = first;
}

/* ^: first non-blank of the line */
static void mv_line_text(annot_t* an, struct pos* p) {
  int first, last;

  line_span(an, *p, &first, &last);
  for (p->idx = first; p->idx < last && class_at(an, *p) == CLASS_SPACE; p->idx++) {
  }
}

/* $: last non-blank of the line */
static void mv_line_end(annot_t* an, struct pos* p) {
  int first, last;

  line_span(an, *p, &first, &last);
  for (p->idx = last; p->idx > first && class_at(an, *p) == CLASS_SPACE; p->idx--) {
  }
}

/* }: first character of the next paragraph */
static void mv_block_next(annot_t* an, struct pos* p) {
  struct pos q = *p;

  while (step(an, &q, 1)) {
    if (same_block(an, *p, q) == false) {
      *p = q;
      return;
    }
  }
}

/* {: start of this paragraph, or of the previous one if already at its start */
static void mv_block_prev(annot_t* an, struct pos* p) {
  struct pos q = *p, prev = *p;

  if (step(an, &prev, -1) && same_block(an, prev, q) == false) {
    q = prev;
  }
  for (prev = q; step(an, &prev, -1) && same_block(an, prev, q); q = prev) {
  }
  *p = q;
}

/* gg */
static void mv_page_first(annot_t* UNUSED(an), struct pos* p) {
  p->idx = 0;
}

/* G */
static void mv_page_last(annot_t* an, struct pos* p) {
  p->idx = (int)chars_of(an, p->page)->len - 1;
}

static void mv_page(annot_t* an, struct pos* p, int dir) {
  int page = page_with_text(an, p->page + dir, dir);

  if (page >= 0) {
    *p = (struct pos){page, 0};
  }
}

/* J */
static void mv_page_next(annot_t* an, struct pos* p) {
  mv_page(an, p, 1);
}

/* K */
static void mv_page_prev(annot_t* an, struct pos* p) {
  mv_page(an, p, -1);
}

static const struct motion {
  void (*fn)(annot_t* an, struct pos* p);
  bool vertical; /* keeps the goal column */
} motions[M_COUNT] = {
    [M_LEFT]       = {mv_left, false},
    [M_RIGHT]      = {mv_right, false},
    [M_DOWN]       = {mv_down, true},
    [M_UP]         = {mv_up, true},
    [M_WORD]       = {mv_word, false},
    [M_WORD_BACK]  = {mv_word_back, false},
    [M_WORD_END]   = {mv_word_end, false},
    [M_LINE_START] = {mv_line_start, false},
    [M_LINE_TEXT]  = {mv_line_text, false},
    [M_LINE_END]   = {mv_line_end, false},
    [M_BLOCK_NEXT] = {mv_block_next, false},
    [M_BLOCK_PREV] = {mv_block_prev, false},
    [M_PAGE_FIRST] = {mv_page_first, false},
    [M_PAGE_LAST]  = {mv_page_last, false},
    [M_PAGE_NEXT]  = {mv_page_next, false},
    [M_PAGE_PREV]  = {mv_page_prev, false},
};

/* ------------------------------------------------------------------------ */
/* cursor                                                                   */

static void redraw_pages(annot_t* an, int from, int to) {
  for (int page = MAX(MIN(from, to), 0); page <= MAX(from, to); page++) {
    GtkWidget* w = page_widget(an, page);
    if (w != NULL) {
      gtk_widget_queue_draw(w);
    }
  }
}

/* Redraw the pages the cursor moved between and, in visual mode, the whole selection. */
static void redraw_cursor(annot_t* an, struct pos old) {
  int from = MIN(old.page, an->cursor.page);
  int to   = MAX(old.page, an->cursor.page);

  if (mode_is(an, an->visual_mode)) {
    from = MIN(from, an->anchor.page);
    to   = MAX(to, an->anchor.page);
  }
  redraw_pages(an, from, to);
}

/* Place the cursor. Every cursor change goes through here. */
static void cursor_set(annot_t* an, struct pos p, bool keep_goal) {
  struct pos old = an->has_cursor ? an->cursor : p;

  an->cursor     = p;
  an->has_cursor = true;
  if (keep_goal == false) {
    an->goal_x = center_x(&char_at(an, p)->box);
  }
  redraw_cursor(an, old);
  scroll_to_cursor(an);
  status_update(an);
}

/* The first character fully inside the viewport, else the current page's first. */
static bool first_visible(annot_t* an, struct pos* out) {
  zathura_document_t* doc = zathura_get_document(an->zathura);
  int page;

  for (page = 0; page < page_count(an); page++) {
    if (page_is_visible(an->zathura, page) == false) {
      continue;
    }
    for (guint i = 0; i < chars_of(an, page)->len; i++) {
      struct pos p = {page, (int)i};
      if (on_screen(an, p)) {
        *out = p;
        return true;
      }
    }
  }

  page = page_with_text(an, (int)zathura_document_get_current_page_number(doc), 1);
  if (page < 0) {
    page = page_with_text(an, page_count(an) - 1, -1);
  }
  if (page < 0) {
    return false;
  }
  *out = (struct pos){page, 0};
  return true;
}

/* The character nearest a page point, preferring the same line. */
static bool nearest_char(annot_t* an, int page, double x, double y, struct pos* out) {
  GArray* chars = chars_of(an, page);
  double best   = INFINITY;

  for (guint i = 0; i < chars->len; i++) {
    const zathura_rectangle_t* b = &CHAR(chars, i)->box;
    double dx                    = gap(x, b->x1, b->x2);
    double dy                    = gap(y, b->y1, b->y2);
    double d                     = dx * dx + 4 * dy * dy;

    if (d < best) {
      best = d;
      *out = (struct pos){page, (int)i};
    }
  }
  return chars->len > 0;
}

static void mode_set(annot_t* an, girara_mode_t mode) {
  girara_mode_set(an->zathura->ui.session, mode);
  status_update(an);
}

/* ------------------------------------------------------------------------ */
/* selection                                                                */

static void selection(annot_t* an, struct pos* start, struct pos* end) {
  int first, last;

  *start = an->anchor;
  *end   = an->cursor;
  if (pos_cmp(*start, *end) > 0) {
    *start = an->cursor;
    *end   = an->anchor;
  }
  if (an->linewise) {
    line_span(an, *start, &first, &last);
    start->idx = first;
    line_span(an, *end, &first, &last);
    end->idx = last;
  }
}

/* The part of [start, end] on `page` as an index range; false if none. */
static bool selection_on_page(annot_t* an, struct pos start, struct pos end, int page, int* first, int* last) {
  GArray* chars = chars_of(an, page);

  if (page < start.page || page > end.page || chars->len == 0) {
    return false;
  }
  *first = page == start.page ? start.idx : 0;
  *last  = page == end.page ? end.idx : (int)chars->len - 1;
  return *first <= *last;
}

/* One rectangle per line, covering the non-blank characters in [first, last]. */
static GArray* line_rects(annot_t* an, int page, int first, int last) {
  GArray* chars = chars_of(an, page);
  GArray* rects = g_array_new(FALSE, FALSE, sizeof(zathura_rectangle_t));

  for (int i = first; i <= last;) {
    unsigned int line = CHAR(chars, i)->line;
    bool any          = false;
    zathura_rectangle_t r;

    for (; i <= last && CHAR(chars, i)->line == line; i++) {
      const zathura_text_char_t* c = CHAR(chars, i);

      if (c->c == 0 || g_unichar_isspace(c->c)) {
        continue;
      }
      if (any) {
        rect_union(&r, &c->box);
      } else {
        r   = c->box;
        any = true;
      }
    }
    if (any) {
      g_array_append_val(rects, r);
    }
  }
  return rects;
}

/* Join two lines of copied text: a newline between paragraphs, else undo a hyphenation or add a space. */
static void line_join(GString* s, bool new_paragraph) {
  char last = s->len > 0 ? s->str[s->len - 1] : ' ';

  if (new_paragraph) {
    g_string_append_c(s, '\n');
  } else if (last == '-') {
    g_string_truncate(s, s->len - 1);
  } else if (last != ' ') {
    g_string_append_c(s, ' ');
  }
}

static char* selection_text(annot_t* an, struct pos start, struct pos end) {
  GString* s                      = g_string_new(NULL);
  const zathura_text_char_t* prev = NULL;
  int prev_page                   = -1;
  struct pos p                    = start;

  do {
    const zathura_text_char_t* c = char_at(an, p);

    if (prev != NULL && (p.page != prev_page || c->line != prev->line)) {
      line_join(s, p.page == prev_page && c->block != prev->block);
    }
    if (c->c != 0) {
      g_string_append_unichar(s, c->c);
    }
    prev      = c;
    prev_page = p.page;
  } while (pos_cmp(p, end) < 0 && step(an, &p, 1));

  return g_string_free(s, FALSE);
}

/* ------------------------------------------------------------------------ */
/* saving                                                                   */

static bool file_stamp(annot_t* an, struct file_stamp* st) {
  zathura_document_t* doc = zathura_get_document(an->zathura);
  const char* path        = doc != NULL ? zathura_document_get_path(doc) : NULL;

  if (path == NULL) {
    return false;
  }
  g_autoptr(GFile) file     = g_file_new_for_path(path);
  g_autoptr(GFileInfo) info = g_file_query_info(
      file, G_FILE_ATTRIBUTE_TIME_MODIFIED "," G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC "," G_FILE_ATTRIBUTE_STANDARD_SIZE,
      G_FILE_QUERY_INFO_NONE, NULL, NULL);
  if (info == NULL) {
    return false;
  }
  st->mtime = (gint64)g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED) * G_USEC_PER_SEC +
              g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC);
  st->size = g_file_info_get_size(info);
  return true;
}

static gboolean reload_idle(gpointer session) {
  sc_reload(session, NULL, NULL, 0);
  return G_SOURCE_REMOVE;
}

static const char* unsupported_or(zathura_error_t err, const char* msg) {
  return err == ZATHURA_ERROR_NOT_IMPLEMENTED ? _("Annotations are not supported for this file type") : msg;
}

static void save(annot_t* an) {
  bool needs_reload   = false;
  zathura_error_t err = zathura_document_save(zathura_get_document(an->zathura), &needs_reload);

  an->saved_valid = false;
  if (err != ZATHURA_ERROR_OK) {
    girara_notify(an->zathura->ui.session, GIRARA_ERROR, "%s",
                  unsupported_or(err, _("Could not save annotations to the file")));
  } else if (needs_reload) {
    /* The file had to be rewritten rather than appended to: reopen it. */
    g_idle_add(reload_idle, an->zathura->ui.session);
  } else {
    an->saved_valid = file_stamp(an, &an->saved);
  }
}

/* ------------------------------------------------------------------------ */
/* annotation changes; each one records how to undo itself                  */

static struct undo* undo_push(annot_t* an, enum undo_kind kind, int page, int id) {
  struct undo* u = g_new0(struct undo, 1);

  u->kind = kind;
  u->page = page;
  u->id   = id;
  g_ptr_array_add(an->undo, u);
  if (an->undo->len > UNDO_DEPTH) {
    g_ptr_array_remove_index(an->undo, 0);
  }
  return u;
}

static void undo_free(void* data) {
  struct undo* u = data;

  if (u->rects != NULL) {
    g_array_free(u->rects, TRUE);
  }
  g_free(u->note);
  g_free(u);
}

static char* author_name(annot_t* an) {
  char* name = NULL;

  girara_setting_get(an->zathura->ui.session, "annotation-author", &name);
  if (name != NULL && *name != '\0') {
    return name;
  }
  g_free(name);
  name = g_strdup(g_get_real_name());
  if (*name == '\0' || g_strcmp0(name, "Unknown") == 0) {
    g_free(name);
    name = g_strdup(g_get_user_name());
  }
  return name;
}

static void color_get(annot_t* an, const char* key, const char* fallback, GdkRGBA* c) {
  g_autofree char* value = NULL;

  girara_setting_get(an->zathura->ui.session, key, &value);
  if (value == NULL || parse_color(c, value) == false) {
    parse_color(c, fallback);
  }
}

static void highlight_color(annot_t* an, int index, double rgb[3]) {
  g_autofree char* key = g_strdup_printf("annotation-color-%d", index + 1);
  GdkRGBA c;

  color_get(an, key, default_colors[index], &c);
  rgb[0] = c.red;
  rgb[1] = c.green;
  rgb[2] = c.blue;
}

/* Create a highlight without recording undo. Returns its id, or -1. */
static int highlight_create(annot_t* an, int page, GArray* rects, const double rgb[3], const char* note) {
  girara_list_t* list     = girara_list_new_with_free(g_free);
  g_autofree char* author = author_name(an);
  zathura_error_t err     = ZATHURA_ERROR_OK;
  int id;

  for (guint i = 0; i < rects->len; i++) {
    girara_list_append(list, g_memdup2(&g_array_index(rects, zathura_rectangle_t, i), sizeof(zathura_rectangle_t)));
  }
  id = zathura_page_add_highlight(page_get(an, page), list, rgb, note, author, &err);
  girara_list_free(list);

  if (id < 0) {
    girara_notify(an->zathura->ui.session, GIRARA_ERROR, "%s", unsupported_or(err, _("Could not add highlight")));
    return -1;
  }
  annots_dirty(an, page);
  return id;
}

static bool highlight_add(annot_t* an, int page, GArray* rects, const double rgb[3], const char* note) {
  int id = highlight_create(an, page, rects, rgb, note);

  if (id < 0) {
    return false;
  }
  undo_push(an, UNDO_ADD, page, id);
  return true;
}

/* Delete `a`, which is invalid afterwards. */
static bool annot_delete(annot_t* an, int page, zathura_annotation_t* a) {
  struct undo* u;

  if (zathura_page_delete_annotation(page_get(an, page), a->id) != ZATHURA_ERROR_OK) {
    girara_notify(an->zathura->ui.session, GIRARA_ERROR, _("Could not delete annotation"));
    return false;
  }
  u        = undo_push(an, UNDO_DELETE, page, a->id);
  u->rects = g_array_new(FALSE, FALSE, sizeof(zathura_rectangle_t));
  g_array_append_vals(u->rects, a->rects, a->n_rects);
  memcpy(u->color, a->color, sizeof(u->color));
  u->note = g_strdup(a->contents);
  annots_dirty(an, page);
  return true;
}

/* Set the note of `a` (empty removes it); `a` is invalid afterwards. */
static bool note_set(annot_t* an, int page, zathura_annotation_t* a, const char* note) {
  if (zathura_page_set_annotation_contents(page_get(an, page), a->id, note) != ZATHURA_ERROR_OK) {
    girara_notify(an->zathura->ui.session, GIRARA_ERROR, _("Could not change note"));
    return false;
  }
  undo_push(an, UNDO_EDIT, page, a->id)->note = g_strdup(a->contents);
  annots_dirty(an, page);
  return true;
}

/* A re-created annotation gets a new id; older undo entries must follow it. */
static void undo_rename(annot_t* an, int page, int old_id, int new_id) {
  for (guint i = 0; i < an->undo->len; i++) {
    struct undo* u = g_ptr_array_index(an->undo, i);
    if (u->page == page && u->id == old_id) {
      u->id = new_id;
    }
  }
}

static const char* undo_apply(annot_t* an, struct undo* u) {
  zathura_page_t* page = page_get(an, u->page);
  int id;

  switch (u->kind) {
  case UNDO_ADD:
    zathura_page_delete_annotation(page, u->id);
    return _("Removed highlight");
  case UNDO_DELETE:
    id = highlight_create(an, u->page, u->rects, u->color, u->note);
    if (id >= 0) {
      undo_rename(an, u->page, u->id, id);
    }
    return _("Restored highlight");
  case UNDO_EDIT:
    zathura_page_set_annotation_contents(page, u->id, u->note);
    return _("Restored previous note");
  }
  return NULL;
}

/* ------------------------------------------------------------------------ */
/* commands                                                                 */

static bool sc_move(girara_session_t* session, girara_argument_t* arg, girara_event_t* UNUSED(event), unsigned int t) {
  annot_t* an = from_session(session);
  const struct motion* m;
  struct pos p;

  if (an == NULL || an->has_cursor == false || arg->n < 0 || arg->n >= M_COUNT) {
    return false;
  }

  /* Like vim: if the view was scrolled away from the cursor, bring it back first. */
  if (mode_is(an, an->cursor_mode) && on_screen(an, an->cursor) == false && first_visible(an, &p)) {
    cursor_set(an, p, false);
    return false;
  }

  m = &motions[arg->n];
  p = an->cursor;
  for (unsigned int i = 0; i < MAX(t, 1u); i++) {
    m->fn(an, &p);
  }
  cursor_set(an, p, m->vertical);
  return false;
}

/* v / V in normal mode */
static bool sc_enter(girara_session_t* session, girara_argument_t* arg, girara_event_t* UNUSED(event),
                     unsigned int UNUSED(t)) {
  annot_t* an = from_session(session);
  struct pos p;

  if (an == NULL || zathura_get_document(an->zathura) == NULL) {
    return false;
  }
  p = an->cursor;
  if ((an->has_cursor == false || on_screen(an, p) == false) && first_visible(an, &p) == false) {
    girara_notify(session, GIRARA_WARNING, _("This document has no text layer (scanned?)"));
    return false;
  }

  an->anchor   = p;
  an->linewise = arg->n == 1;
  mode_set(an, an->linewise ? an->visual_mode : an->cursor_mode);
  cursor_set(an, p, false);
  return false;
}

/* Esc: visual -> cursor -> normal */
static bool sc_leave(girara_session_t* session, girara_argument_t* UNUSED(arg), girara_event_t* UNUSED(event),
                     unsigned int UNUSED(t)) {
  annot_t* an = from_session(session);

  if (an == NULL) {
    return false;
  }
  redraw_cursor(an, an->cursor);
  mode_set(an, mode_is(an, an->visual_mode) ? an->cursor_mode : an->zathura->modes.normal);
  return false;
}

/* v / V in cursor or visual mode; the same key again leaves visual mode */
static bool sc_visual(girara_session_t* session, girara_argument_t* arg, girara_event_t* UNUSED(event),
                      unsigned int UNUSED(t)) {
  annot_t* an         = from_session(session);
  const bool linewise = arg->n == 1;

  if (an == NULL || an->has_cursor == false) {
    return false;
  }
  if (mode_is(an, an->visual_mode) == false) {
    an->anchor = an->cursor;
  } else if (an->linewise == linewise) {
    return sc_leave(session, NULL, NULL, 0);
  }
  an->linewise = linewise;
  mode_set(an, an->visual_mode);
  redraw_cursor(an, an->cursor);
  return false;
}

/* o: swap the ends of the selection */
static bool sc_swap(girara_session_t* session, girara_argument_t* UNUSED(arg), girara_event_t* UNUSED(event),
                    unsigned int UNUSED(t)) {
  annot_t* an = from_session(session);
  struct pos p;

  if (an == NULL || mode_is(an, an->visual_mode) == false) {
    return false;
  }
  p          = an->anchor;
  an->anchor = an->cursor;
  cursor_set(an, p, false);
  return false;
}

/* Leave visual mode with the cursor at the start of what was selected. */
static void visual_done(annot_t* an, struct pos start) {
  redraw_cursor(an, an->cursor); /* erase the selection */
  mode_set(an, an->cursor_mode);
  cursor_set(an, start, false);
}

static void highlight_selection(annot_t* an, int color, const char* note) {
  struct pos start, end;
  double rgb[3];
  int added = 0;

  selection(an, &start, &end);
  highlight_color(an, color, rgb);

  for (int page = start.page; page <= end.page; page++) {
    int first, last;

    if (selection_on_page(an, start, end, page, &first, &last) == false) {
      continue;
    }
    GArray* rects = line_rects(an, page, first, last);
    /* A selection spanning pages gives one highlight per page; the note goes on the first. */
    if (rects->len > 0 && highlight_add(an, page, rects, rgb, added == 0 ? note : NULL)) {
      added++;
    }
    g_array_free(rects, TRUE);
  }

  if (added > 0) {
    save(an);
    girara_notify(an->zathura->ui.session, GIRARA_INFO,
                  note != NULL && *note != '\0' ? _("Highlighted with note") : _("Highlighted"));
  }
  visual_done(an, start);
}

/* H / Enter: highlight the selection. A count picks the colour: 2H is colour 2. */
static bool sc_highlight(girara_session_t* session, girara_argument_t* UNUSED(arg), girara_event_t* UNUSED(event),
                         unsigned int t) {
  annot_t* an = from_session(session);

  if (an == NULL || mode_is(an, an->visual_mode) == false) {
    return false;
  }
  if (t > N_COLORS) {
    girara_notify(session, GIRARA_WARNING, _("Colours are 1-%d (e.g. 2H)"), N_COLORS);
    return false;
  }
  highlight_selection(an, t == 0 ? 0 : (int)t - 1, NULL);
  return false;
}

/* y: copy the selection */
static bool sc_yank(girara_session_t* session, girara_argument_t* UNUSED(arg), girara_event_t* UNUSED(event),
                    unsigned int UNUSED(t)) {
  annot_t* an = from_session(session);
  struct pos start, end;

  if (an == NULL || mode_is(an, an->visual_mode) == false) {
    return false;
  }
  selection(an, &start, &end);

  g_autofree char* text         = selection_text(an, start, end);
  g_autofree GdkAtom* clipboard = get_selection(an->zathura);
  if (clipboard != NULL) {
    gtk_clipboard_set_text(gtk_clipboard_get(*clipboard), text, -1);
  }
  girara_notify(session, GIRARA_INFO, _("Copied %ld characters"), g_utf8_strlen(text, -1));
  visual_done(an, start);
  return false;
}

static gboolean cb_prompt(GtkEntry* entry, void* data) {
  annot_t* an            = data;
  enum prompt_kind kind  = an->prompt.kind;
  g_autofree char* input = g_strstrip(gtk_editable_get_chars(GTK_EDITABLE(entry), 0, -1));
  zathura_annotation_t* a;

  an->prompt.kind = PROMPT_NONE;
  if (kind == PROMPT_NEW && mode_is(an, an->visual_mode)) {
    highlight_selection(an, an->prompt.color, input);
    return FALSE;
  }
  if (kind != PROMPT_EDIT) {
    return FALSE;
  }

  a = annot_by_id(an, an->prompt.page, an->prompt.id);
  if (a != NULL && note_set(an, an->prompt.page, a, input)) {
    save(an);
    an->noted_id = an->prompt.id; /* just typed; don't echo it back */
    status_update(an);
    girara_notify(an->zathura->ui.session, GIRARA_INFO, *input == '\0' ? _("Note removed") : _("Note saved"));
  }
  return FALSE;
}

static void prompt_open(annot_t* an, const char* label, const char* text) {
  girara_session_t* session = an->zathura->ui.session;

  girara_dialog(session, label, false, NULL, cb_prompt, an);
  if (text != NULL) {
    gtk_entry_set_text(session->gtk.inputbar_entry, text);
    gtk_editable_set_position(GTK_EDITABLE(session->gtk.inputbar_entry), -1);
  }
}

/* a: in visual mode, highlight with a note (3a: colour 3); in cursor mode, edit the note under the cursor */
static bool sc_note(girara_session_t* session, girara_argument_t* UNUSED(arg), girara_event_t* UNUSED(event),
                    unsigned int t) {
  annot_t* an = from_session(session);
  zathura_annotation_t* a;

  if (an == NULL || an->has_cursor == false) {
    return false;
  }
  if (mode_is(an, an->visual_mode)) {
    an->prompt.kind  = PROMPT_NEW;
    an->prompt.color = t >= 1 && t <= N_COLORS ? (int)t - 1 : 0;
    prompt_open(an, _("Note:"), NULL);
    return false;
  }

  a = annot_at(an, an->cursor);
  if (a == NULL) {
    girara_notify(session, GIRARA_WARNING, _("No highlight under the cursor (select text with v, then press a)"));
    return false;
  }
  an->prompt.kind = PROMPT_EDIT;
  an->prompt.page = an->cursor.page;
  an->prompt.id   = a->id;
  prompt_open(an, a->contents != NULL ? _("Edit note (empty to remove):") : _("Note:"), a->contents);
  return false;
}

/* x: delete the highlight under the cursor */
static bool sc_delete(girara_session_t* session, girara_argument_t* UNUSED(arg), girara_event_t* UNUSED(event),
                      unsigned int UNUSED(t)) {
  annot_t* an = from_session(session);
  zathura_annotation_t* a;

  if (an == NULL || an->has_cursor == false) {
    return false;
  }
  a = annot_at(an, an->cursor);
  if (a == NULL || a->type != ZATHURA_ANNOTATION_HIGHLIGHT || a->n_rects == 0) {
    girara_notify(session, GIRARA_WARNING, _("No highlight under the cursor"));
    return false;
  }
  if (annot_delete(an, an->cursor.page, a)) {
    save(an);
    status_update(an);
    girara_notify(session, GIRARA_INFO, _("Highlight deleted (u to undo)"));
  }
  return false;
}

/* u: undo the last annotation change */
static bool sc_undo(girara_session_t* session, girara_argument_t* UNUSED(arg), girara_event_t* UNUSED(event),
                    unsigned int UNUSED(t)) {
  annot_t* an = from_session(session);
  struct undo* u;
  const char* msg;

  if (an == NULL) {
    return false;
  }
  if (an->undo->len == 0) {
    girara_notify(session, GIRARA_INFO, _("Nothing to undo"));
    return false;
  }
  u   = g_ptr_array_steal_index(an->undo, an->undo->len - 1);
  msg = undo_apply(an, u);
  annots_dirty(an, u->page);
  save(an);
  an->noted_id = -1;
  status_update(an);
  girara_notify(session, GIRARA_INFO, "%s", msg);
  undo_free(u);
  return false;
}

/* Reading order of two page points: by line (with `slack` vertical tolerance), then by x. */
static bool reads_before(double ax, double ay, double bx, double by, double slack) {
  return ay < by - slack || (fabs(ay - by) <= slack && ax < bx);
}

/*
 * The first highlight after (dir > 0) or last one before (dir < 0) the point
 * (x, y) on `page`, skipping `skip`. Sets *ax, *ay to where it starts.
 */
static zathura_annotation_t* highlight_near(annot_t* an, int page, int dir, double x, double y, double slack,
                                            const zathura_annotation_t* skip, double* ax, double* ay) {
  girara_list_t* list         = annots_of(an, page);
  zathura_annotation_t* found = NULL;

  for (size_t i = 0; i < girara_list_size(list); i++) {
    zathura_annotation_t* a = girara_list_nth(list, i);
    double hx, hy;

    if (a->n_rects == 0 || a == skip) {
      continue;
    }
    hx = a->rects[0].x1;
    hy = center_y(&a->rects[0]);
    if (dir > 0 ? !reads_before(x, y, hx, hy, slack) : !reads_before(hx, hy, x, y, slack)) {
      continue;
    }
    if (found == NULL || (dir > 0 ? reads_before(hx, hy, *ax, *ay, 0) : reads_before(*ax, *ay, hx, hy, 0))) {
      found = a;
      *ax   = hx;
      *ay   = hy;
    }
  }
  return found;
}

/* ] and [: jump to the next or previous highlight */
static bool sc_jump(girara_session_t* session, girara_argument_t* arg, girara_event_t* UNUSED(event),
                    unsigned int UNUSED(t)) {
  annot_t* an   = from_session(session);
  const int dir = arg->n;
  const zathura_text_char_t* c;
  zathura_annotation_t* here;

  if (an == NULL || an->has_cursor == false) {
    return false;
  }
  c    = char_at(an, an->cursor);
  here = annot_at(an, an->cursor);

  for (int page = an->cursor.page; page >= 0 && page < page_count(an); page += dir) {
    /* On the cursor's page search from the cursor; on other pages, from the near edge. */
    const bool own  = page == an->cursor.page;
    const double x  = own ? center_x(&c->box) : (dir > 0 ? -INFINITY : INFINITY);
    const double y  = own ? center_y(&c->box) : (dir > 0 ? -INFINITY : INFINITY);
    const double sl = own ? (c->box.y2 - c->box.y1) / 2 : 0;
    double hx, hy;
    struct pos p;

    if (highlight_near(an, page, dir, x, y, sl, here, &hx, &hy) != NULL && nearest_char(an, page, hx + 1, hy, &p)) {
      cursor_set(an, p, false);
      return false;
    }
  }
  girara_notify(session, GIRARA_INFO, dir > 0 ? _("No more highlights below") : _("No more highlights above"));
  return false;
}

/* ------------------------------------------------------------------------ */
/* drawing                                                                  */

/* A small folded-note badge above the end of each highlight that has a note. */
static void draw_note_markers(annot_t* an, zathura_page_t* page, cairo_t* cr) {
  girara_list_t* list = annots_of(an, zathura_page_get_index(page));
  const double s      = 7;

  for (size_t i = 0; i < girara_list_size(list); i++) {
    zathura_annotation_t* a = girara_list_nth(list, i);
    zathura_rectangle_t r;
    double x, y;

    if (a->contents == NULL) {
      continue;
    }
    r = recalc_rectangle(page, a->n_rects > 0 ? a->rects[a->n_rects - 1] : a->bounds);
    x = MAX(r.x1, r.x2) - s * 0.6;
    y = MIN(r.y1, r.y2) - s * 1.25;

    cairo_move_to(cr, x, y);
    cairo_line_to(cr, x + s, y);
    cairo_line_to(cr, x + s, y + s);
    cairo_line_to(cr, x + s * 0.35, y + s);
    cairo_line_to(cr, x, y + s * 1.4);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, a->color[0] * 0.75, a->color[1] * 0.75, a->color[2] * 0.75, 0.95);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
  }
}

static void draw_selection(annot_t* an, zathura_page_t* page, cairo_t* cr) {
  const GdkRGBA color = an->zathura->ui.colors.highlight_color;
  const int index     = zathura_page_get_index(page);
  struct pos start, end;
  int first, last;

  selection(an, &start, &end);
  if (selection_on_page(an, start, end, index, &first, &last) == false) {
    return;
  }

  GArray* rects = line_rects(an, index, first, last);
  for (guint i = 0; i < rects->len; i++) {
    zathura_rectangle_t r = recalc_rectangle(page, g_array_index(rects, zathura_rectangle_t, i));
    cairo_rectangle(cr, MIN(r.x1, r.x2), MIN(r.y1, r.y2), fabs(r.x2 - r.x1), fabs(r.y2 - r.y1));
  }
  g_array_free(rects, TRUE);
  cairo_set_source_rgba(cr, color.red, color.green, color.blue, MAX(color.alpha, 0.35));
  cairo_fill(cr);
}

/* A block cursor, like vim's. */
static void draw_cursor(annot_t* an, zathura_page_t* page, cairo_t* cr) {
  zathura_rectangle_t r = recalc_rectangle(page, char_at(an, an->cursor)->box);
  const double x        = MIN(r.x1, r.x2);
  const double y        = MIN(r.y1, r.y2);
  const double w        = MAX(fabs(r.x2 - r.x1), 2.0);
  const double h        = MAX(fabs(r.y2 - r.y1), 4.0);
  GdkRGBA c;

  color_get(an, "cursor-color", default_cursor_color, &c);
  cairo_rectangle(cr, x, y, w, h);
  cairo_set_source_rgba(cr, c.red, c.green, c.blue, 0.35);
  cairo_fill(cr);
  cairo_rectangle(cr, x - 0.5, y - 0.5, w + 1, h + 1);
  cairo_set_source_rgba(cr, c.red, c.green, c.blue, 0.95);
  cairo_set_line_width(cr, 1.5);
  cairo_stroke(cr);
}

/* ------------------------------------------------------------------------ */
/* hooks called from the rest of zathura                                    */

void zathura_annot_draw(zathura_t* zathura, zathura_page_t* page, cairo_t* cr) {
  annot_t* an = zathura->annot;

  if (an == NULL || zathura_get_document(zathura) == NULL) {
    return;
  }
  cairo_save(cr);
  draw_note_markers(an, page, cr);
  if (active(an) && an->has_cursor) {
    if (mode_is(an, an->visual_mode)) {
      draw_selection(an, page, cr);
    }
    if (an->cursor.page == (int)zathura_page_get_index(page)) {
      draw_cursor(an, page, cr);
    }
  }
  cairo_restore(cr);
}

bool zathura_annot_active(zathura_t* zathura) {
  return zathura->annot != NULL && active(zathura->annot);
}

void zathura_annot_click(zathura_t* zathura, zathura_page_t* page, double x, double y) {
  annot_t* an = zathura->annot;
  struct pos p;

  if (an != NULL && active(an) && nearest_char(an, zathura_page_get_index(page), x, y, &p)) {
    cursor_set(an, p, false);
  }
}

void zathura_annot_follow_search(zathura_t* zathura, zathura_page_t* page, const zathura_rectangle_t* rect) {
  annot_t* an = zathura->annot;
  struct pos p;

  if (an != NULL && active(an) && nearest_char(an, zathura_page_get_index(page), rect->x1 + 0.5, center_y(rect), &p)) {
    cursor_set(an, p, false);
  }
}

bool zathura_annot_is_own_save(zathura_t* zathura) {
  annot_t* an = zathura->annot;
  struct file_stamp now;

  return an != NULL && an->saved_valid && file_stamp(an, &now) && now.mtime == an->saved.mtime &&
         now.size == an->saved.size;
}

void zathura_annot_reset(zathura_t* zathura) {
  annot_t* an = zathura->annot;

  if (an == NULL) {
    return;
  }
  g_hash_table_remove_all(an->cache);
  g_ptr_array_set_size(an->undo, 0);
  an->has_cursor  = false;
  an->saved_valid = false;
  an->prompt.kind = PROMPT_NONE;
  an->noted_id    = -1;
  if (active(an)) {
    girara_mode_set(zathura->ui.session, zathura->modes.normal);
  }
  if (an->status != NULL) {
    girara_statusbar_item_set_text(zathura->ui.session, an->status, "");
  }
}

void zathura_annot_free(zathura_t* zathura) {
  annot_t* an = zathura->annot;

  if (an == NULL) {
    return;
  }
  g_hash_table_destroy(an->cache);
  g_ptr_array_free(an->undo, TRUE);
  g_free(an);
  zathura->annot = NULL;
}

/* ------------------------------------------------------------------------ */
/* setup                                                                    */

enum {
  IN_NORMAL = 1 << 0, /* and fullscreen */
  IN_CURSOR = 1 << 1,
  IN_VISUAL = 1 << 2,
  IN_TEXT   = IN_CURSOR | IN_VISUAL,
};

static const struct binding {
  unsigned int modes;
  guint mask;
  guint key;
  const char* seq; /* a key sequence, instead of `key` */
  girara_shortcut_function_t fn;
  int arg;
  const char* data;
} bindings[] = {
    {IN_NORMAL, 0, GDK_KEY_v, NULL, sc_enter, 0, NULL},
    {IN_NORMAL, 0, GDK_KEY_V, NULL, sc_enter, 1, NULL},

    {IN_TEXT, 0, GDK_KEY_h, NULL, sc_move, M_LEFT, NULL},
    {IN_TEXT, 0, GDK_KEY_Left, NULL, sc_move, M_LEFT, NULL},
    {IN_TEXT, 0, GDK_KEY_l, NULL, sc_move, M_RIGHT, NULL},
    {IN_TEXT, 0, GDK_KEY_Right, NULL, sc_move, M_RIGHT, NULL},
    {IN_TEXT, 0, GDK_KEY_j, NULL, sc_move, M_DOWN, NULL},
    {IN_TEXT, 0, GDK_KEY_Down, NULL, sc_move, M_DOWN, NULL},
    {IN_TEXT, 0, GDK_KEY_k, NULL, sc_move, M_UP, NULL},
    {IN_TEXT, 0, GDK_KEY_Up, NULL, sc_move, M_UP, NULL},
    {IN_TEXT, 0, GDK_KEY_w, NULL, sc_move, M_WORD, NULL},
    {IN_TEXT, 0, GDK_KEY_b, NULL, sc_move, M_WORD_BACK, NULL},
    {IN_TEXT, 0, GDK_KEY_e, NULL, sc_move, M_WORD_END, NULL},
    {IN_TEXT, 0, GDK_KEY_0, NULL, sc_move, M_LINE_START, NULL},
    {IN_TEXT, 0, GDK_KEY_asciicircum, NULL, sc_move, M_LINE_TEXT, NULL},
    {IN_TEXT, 0, GDK_KEY_dollar, NULL, sc_move, M_LINE_END, NULL},
    {IN_TEXT, 0, GDK_KEY_braceright, NULL, sc_move, M_BLOCK_NEXT, NULL},
    {IN_TEXT, 0, GDK_KEY_braceleft, NULL, sc_move, M_BLOCK_PREV, NULL},
    {IN_TEXT, 0, 0, "gg", sc_move, M_PAGE_FIRST, NULL},
    {IN_TEXT, 0, GDK_KEY_G, NULL, sc_move, M_PAGE_LAST, NULL},
    {IN_TEXT, 0, GDK_KEY_J, NULL, sc_move, M_PAGE_NEXT, NULL},
    {IN_TEXT, 0, GDK_KEY_K, NULL, sc_move, M_PAGE_PREV, NULL},

    /* scrolling leaves the cursor where it is; the next motion brings it back */
    {IN_TEXT, GDK_CONTROL_MASK, GDK_KEY_d, NULL, sc_scroll, HALF_DOWN, NULL},
    {IN_TEXT, GDK_CONTROL_MASK, GDK_KEY_u, NULL, sc_scroll, HALF_UP, NULL},
    {IN_TEXT, GDK_CONTROL_MASK, GDK_KEY_f, NULL, sc_scroll, FULL_DOWN, NULL},
    {IN_TEXT, GDK_CONTROL_MASK, GDK_KEY_b, NULL, sc_scroll, FULL_UP, NULL},
    {IN_TEXT, GDK_CONTROL_MASK, GDK_KEY_e, NULL, sc_scroll, DOWN, NULL},
    {IN_TEXT, GDK_CONTROL_MASK, GDK_KEY_y, NULL, sc_scroll, UP, NULL},
    {IN_TEXT, 0, GDK_KEY_plus, NULL, sc_zoom, ZOOM_IN, NULL},
    {IN_TEXT, 0, GDK_KEY_minus, NULL, sc_zoom, ZOOM_OUT, NULL},

    /* the cursor follows search matches; see zathura_annot_follow_search() */
    {IN_TEXT, 0, GDK_KEY_slash, NULL, sc_focus_inputbar, 0, "/"},
    {IN_TEXT, 0, GDK_KEY_question, NULL, sc_focus_inputbar, 0, "?"},
    {IN_TEXT, 0, GDK_KEY_colon, NULL, sc_focus_inputbar, 0, ":"},
    {IN_TEXT, 0, GDK_KEY_n, NULL, sc_search, FORWARD, NULL},
    {IN_TEXT, 0, GDK_KEY_N, NULL, sc_search, BACKWARD, NULL},

    {IN_TEXT, 0, GDK_KEY_v, NULL, sc_visual, 0, NULL},
    {IN_TEXT, 0, GDK_KEY_V, NULL, sc_visual, 1, NULL},
    {IN_TEXT, 0, GDK_KEY_a, NULL, sc_note, 0, NULL},
    {IN_TEXT, 0, GDK_KEY_Escape, NULL, sc_leave, 0, NULL},
    {IN_TEXT, GDK_CONTROL_MASK, GDK_KEY_bracketleft, NULL, sc_leave, 0, NULL},
    {IN_TEXT, GDK_CONTROL_MASK, GDK_KEY_c, NULL, sc_leave, 0, NULL},

    {IN_CURSOR, 0, GDK_KEY_q, NULL, sc_leave, 0, NULL},
    {IN_CURSOR, 0, GDK_KEY_x, NULL, sc_delete, 0, NULL},
    {IN_CURSOR, 0, GDK_KEY_u, NULL, sc_undo, 0, NULL},
    {IN_CURSOR, 0, GDK_KEY_bracketright, NULL, sc_jump, 1, NULL},
    {IN_CURSOR, 0, GDK_KEY_bracketleft, NULL, sc_jump, -1, NULL},

    {IN_VISUAL, 0, GDK_KEY_o, NULL, sc_swap, 0, NULL},
    {IN_VISUAL, 0, GDK_KEY_y, NULL, sc_yank, 0, NULL},
    {IN_VISUAL, 0, GDK_KEY_H, NULL, sc_highlight, 0, NULL},
    {IN_VISUAL, 0, GDK_KEY_Return, NULL, sc_highlight, 0, NULL},
};

static void bind(girara_session_t* gs, const struct binding* b, girara_mode_t mode) {
  girara_shortcut_add(gs, b->mask, b->key, b->seq, b->fn, mode, b->arg, (void*)b->data);
}

void zathura_annot_config(zathura_t* zathura) {
  girara_session_t* gs = zathura->ui.session;
  annot_t* an          = g_new0(annot_t, 1);

  an->zathura     = zathura;
  an->cache       = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, cache_free);
  an->undo        = g_ptr_array_new_with_free_func(undo_free);
  an->noted_id    = -1;
  an->cursor_mode = girara_mode_add(gs, "cursor");
  an->visual_mode = girara_mode_add(gs, "visual");
  zathura->annot  = an;

  for (int i = 0; i < N_COLORS; i++) {
    g_autofree char* key = g_strdup_printf("annotation-color-%d", i + 1);
    girara_setting_add(gs, key, (void*)default_colors[i], STRING, false, _("Highlight colour"), NULL, NULL);
  }
  girara_setting_add(gs, "cursor-color", (void*)default_cursor_color, STRING, false, _("Text cursor colour"), NULL,
                     NULL);
  girara_setting_add(gs, "annotation-author", "", STRING, false,
                     _("Author stored in new annotations (empty: your account's full name)"), NULL, NULL);

  for (size_t i = 0; i < G_N_ELEMENTS(bindings); i++) {
    const struct binding* b = &bindings[i];

    if (b->modes & IN_NORMAL) {
      bind(gs, b, zathura->modes.normal);
      bind(gs, b, zathura->modes.fullscreen);
    }
    if (b->modes & IN_CURSOR) {
      bind(gs, b, an->cursor_mode);
    }
    if (b->modes & IN_VISUAL) {
      bind(gs, b, an->visual_mode);
    }
  }

  girara_shortcut_mapping_add(gs, "cursor_mode", sc_enter);
  girara_shortcut_mapping_add(gs, "annotation_undo", sc_undo);
  girara_shortcut_mapping_add(gs, "annotation_delete", sc_delete);
  girara_shortcut_mapping_add(gs, "annotation_note", sc_note);
  girara_shortcut_mapping_add(gs, "annotation_highlight", sc_highlight);
}
