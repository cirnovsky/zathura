/* SPDX-License-Identifier: Zlib */

/* vim-like text cursor, visual selection and PDF annotations (highlights + notes). */

#ifndef ZATHURA_ANNOTATIONS_H
#define ZATHURA_ANNOTATIONS_H

#include <cairo.h>
#include <stdbool.h>
#include <girara-gtk/types.h>

#include "types.h"

typedef struct zathura_annot_s zathura_annot_t;

/** Register modes, settings and key bindings (called from config_load_default). */
void zathura_annot_config(zathura_t* zathura);

/** Free all state (called from zathura_free). */
void zathura_annot_free(zathura_t* zathura);

/** Forget per-document state (called when a document is closed). */
void zathura_annot_reset(zathura_t* zathura);

/** Draw note markers, visual selection and the cursor on top of a page. */
void zathura_annot_draw(zathura_t* zathura, zathura_page_t* page, cairo_t* cairo);

/** Mouse click on a page at page coordinates (x, y). */
void zathura_annot_click(zathura_t* zathura, zathura_page_t* page, double x, double y);

/** Move the cursor to the current search result (called after a search). */
void zathura_annot_follow_search(zathura_t* zathura, zathura_page_t* page, const zathura_rectangle_t* rect);

/** True if the file on disk is exactly what we last saved (so no reload is needed). */
bool zathura_annot_is_own_save(zathura_t* zathura);

/** True if cursor or visual mode is active. */
bool zathura_annot_active(zathura_t* zathura);

#endif
