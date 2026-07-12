/* theme.h — shared visual constants for Aegis desktop shell (Citadel).
 * Color constants like C_ACCENT and C_TERM_FG are defined in glyph/draw.h. */
#ifndef CITADEL_THEME_H
#define CITADEL_THEME_H

#include <glyph_theme.h>   /* THEME_* semantic tokens (single source of truth) */

/* Top bar — a floating rounded capsule inset from the top + sides.
 * TOPBAR_HEIGHT is the reserved band; the capsule itself is BAR_H tall, inset
 * BAR_MARGIN_TOP from the top and BAR_MARGIN_SIDE from each side, with fully
 * rounded ends. Content sits BAR_PAD inside the capsule's inner edges.
 * Colors come from glyph/glyph_theme.h. */
#define TOPBAR_HEIGHT   34
#define BAR_MARGIN_TOP  5
#define BAR_MARGIN_SIDE 10
#define BAR_H           (TOPBAR_HEIGHT - 2 * BAR_MARGIN_TOP)   /* 24 */
#define BAR_PAD         22                                     /* content inset from the capsule ends */
#define BAR_EDGE        (BAR_MARGIN_SIDE + BAR_PAD)            /* content inset from screen edge */
#define TOPBAR_BG       THEME_BG
#define TOPBAR_TEXT     THEME_TEXT_DIM
#define AEGIS_AREA_W    120

/* Background */
#define DESKTOP_BG      THEME_BG

#endif /* CITADEL_THEME_H */
