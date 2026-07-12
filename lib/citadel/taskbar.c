/* taskbar.c -- Top bar and context menu for Lumen compositor
 *
 * This file replaces the old bottom taskbar with a macOS-style top bar.
 * The "LoricaOS" text on the left side is clickable and opens a context menu.
 * The right side carries a volume slider and the clock.
 */
#include <glyph.h>   /* pulls glyph/theme.h tokens — must precede citadel theme.h */
#include "theme.h"
#include "taskbar.h"
#include "menu_icon.h"
#include <font.h>
#include <stdlib.h>
#include <string.h>

/* Speaker icon geometry (right side, just left of the clock). Clicking the
 * icon opens the vertical volume-slider popup (drawn by the compositor). */
#define VOL_ICON_W  10      /* speaker body + cone width */
#define VOL_GAP     16      /* gap between the speaker icon and the clock */

static int
clock_width(const char *clock_str)
{
    const char *c = (clock_str && clock_str[0]) ? clock_str : "00:00";
    if (g_font_ui) return font_text_width(g_font_ui, 14, c);
    return (int)strlen(c) * FONT_W;
}

/* Left x of the speaker icon: inset VOL_GAP left of the clock, which itself is
 * inset BAR_EDGE from the screen edge so it sits inside the floating capsule. */
static int
vol_icon_x(int screen_w, const char *clock_str)
{
    int clock_x = screen_w - clock_width(clock_str) - BAR_EDGE;
    return clock_x - VOL_GAP - VOL_ICON_W;
}

/* A thin right-facing arc (a speaker "sound wave") of radius r centred at
 * (cx,cy). Traces the right semicircle pixel-by-pixel via isqrt. */
static void
draw_arc_right(surface_t *s, int cx, int cy, int r, uint32_t col)
{
    for (int dy = -r + 1; dy <= r - 1; dy++) {
        int q = r * r - dy * dy, dx = 0;
        while ((dx + 1) * (dx + 1) <= q) dx++;   /* isqrt(q) */
        draw_blend_rect(s, cx + dx, cy + dy, 1, 1, col, 210);
    }
}

static void
draw_clock(surface_t *s, int screen_w, const char *clock_str)
{
    const char *c = (clock_str && clock_str[0]) ? clock_str : "00:00";
    if (g_font_ui) {
        int cw = font_text_width(g_font_ui, 14, c);
        int cx = screen_w - cw - BAR_EDGE;
        int ty = BAR_MARGIN_TOP + (BAR_H - font_height(g_font_ui, 14)) / 2;
        font_draw_text(s, g_font_ui, 14, cx, ty, c, TOPBAR_TEXT);
    } else {
        draw_text_t(s, screen_w - (int)strlen(c) * FONT_W - BAR_EDGE,
                    BAR_MARGIN_TOP + 6, c, TOPBAR_TEXT);
    }
}

void
topbar_draw(surface_t *s, int screen_w, const char *clock_str, int volume,
            int backdrop_dirty)
{
    /* Floating rounded capsule top bar, inset from the top + sides with fully
     * rounded ends. topbar_draw runs (via on_draw_desktop) after the desktop
     * background is painted and BEFORE any window composites, so cache the clean
     * desktop band once (on a full redraw) and stamp it back on every other
     * frame — that gives the translucent capsule a fresh, non-accumulating base
     * to blend over, and leaves the margins + rounded corners showing the
     * wallpaper. Process-lifetime static (one top bar). */
    static uint32_t *bg_cache = NULL;
    static int cache_w = 0;
    static int cache_wp = -1;
    int wp = glyph_theme_wallpaper();
    int rebuild = backdrop_dirty || !bg_cache || cache_w != screen_w ||
                  cache_wp != wp;
    if (rebuild) {
        if (cache_w != screen_w) {
            free(bg_cache);
            bg_cache = malloc((size_t)screen_w * TOPBAR_HEIGHT * sizeof(uint32_t));
            cache_w = bg_cache ? screen_w : 0;
        }
        if (bg_cache) {
            for (int y = 0; y < TOPBAR_HEIGHT; y++)
                memcpy(&bg_cache[y * screen_w], &s->buf[y * s->pitch],
                       (size_t)screen_w * sizeof(uint32_t));
            cache_wp = wp;
        }
    } else if (bg_cache) {
        for (int y = 0; y < TOPBAR_HEIGHT; y++)
            memcpy(&s->buf[y * s->pitch], &bg_cache[y * screen_w],
                   (size_t)screen_w * sizeof(uint32_t));
    }

    int bx = BAR_MARGIN_SIDE, by = BAR_MARGIN_TOP;
    int bw = screen_w - 2 * BAR_MARGIN_SIDE, bh = BAR_H;
    int rad = bh / 2;   /* fully rounded ends */
    /* Base fill + a very slight vertical gradient: a soft top sheen fading to
     * nothing by mid-height, drawn in the straight middle so it doesn't overrun
     * the rounded ends. No border — keeps it clean and glassy. */
    draw_blend_rounded_rect(s, bx, by, bw, bh, rad, TOPBAR_BG, 205);
    /* Each sheen row spans the full capsule width AT THAT ROW — including the
     * rounded ends — so the gradient reaches the left/right edges instead of
     * stopping at the caps. ex = how far the semicircle end extends at row i. */
    for (int i = 0; i < bh / 2; i++) {
        int a = 18 * (bh / 2 - i) / (bh / 2);
        if (a <= 0) continue;
        int d = rad - i, q = rad * rad - d * d, ex = 0;
        while ((ex + 1) * (ex + 1) <= q) ex++;   /* isqrt(q) */
        ex += 1;   /* cover the fill's 1px AA cap fringe (no black crescent) */
        draw_blend_rect(s, bx + rad - ex, by + i, (bw - 2 * rad) + 2 * ex, 1,
                        0x00FFFFFF, a);
    }

    /* Brand icon + "LoricaOS" label at the left end of the capsule. Inset the
     * icon ~equidistant from the left cap as from the top/bottom (small, so it
     * doesn't crowd the middle), separate from BAR_PAD (which insets the right). */
    int brand_x = bx + 10;
    draw_blit_alpha_scaled(s, brand_x, by + (bh - MENU_ICON_H) / 2,
                           MENU_ICON_W, MENU_ICON_H,
                           (uint32_t *)s_menu_icon, MENU_ICON_W, MENU_ICON_H);
    if (g_font_ui) {
        int ty = by + (bh - font_height(g_font_ui, 14)) / 2;
        font_draw_text(s, g_font_ui, 14, brand_x + MENU_ICON_W + 6, ty,
                       "LoricaOS", 0x00FFFFFF);
    } else {
        draw_text_t(s, brand_x + MENU_ICON_W + 6, by + 6,
                    "LoricaOS", 0x00FFFFFF);
    }

    draw_clock(s, screen_w, clock_str);

    /* Volume: a clickable speaker icon (a click opens the vertical slider
     * popup). The wave arcs reflect the level; 0 draws a muted glyph. */
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    int ix = vol_icon_x(screen_w, clock_str);
    int iy = TOPBAR_HEIGHT / 2;

    /* Speaker: a small body rectangle + a cone widening to the right. */
    draw_fill_rect(s, ix, iy - 2, 3, 5, TOPBAR_TEXT);
    for (int dx = 0; dx <= 4; dx++)
        draw_fill_rect(s, ix + 3 + dx, iy - dx, 1, 2 * dx + 1, TOPBAR_TEXT);

    if (volume <= 0) {
        /* Muted: a small × just right of the cone. */
        for (int t = 0; t < 4; t++) {
            draw_blend_rect(s, ix + 9 + t, iy - 2 + t, 1, 1, TOPBAR_TEXT, 220);
            draw_blend_rect(s, ix + 9 + t, iy + 1 - t, 1, 1, TOPBAR_TEXT, 220);
        }
    } else {
        draw_arc_right(s, ix + 3, iy, 5, TOPBAR_TEXT);            /* inner wave */
        if (volume > 55) draw_arc_right(s, ix + 3, iy, 8, TOPBAR_TEXT); /* outer */
    }
}

int
topbar_hit_aegis(int mx, int my, int screen_w)
{
    (void)screen_w;
    return mx >= BAR_MARGIN_SIDE && mx < BAR_MARGIN_SIDE + AEGIS_AREA_W &&
           my >= BAR_MARGIN_TOP && my < BAR_MARGIN_TOP + BAR_H;
}

int
topbar_volume_icon_x(int screen_w, const char *clock_str)
{
    return vol_icon_x(screen_w, clock_str);
}

int
topbar_volume_icon_hit(int mx, int my, int screen_w, const char *clock_str)
{
    if (my < BAR_MARGIN_TOP || my >= BAR_MARGIN_TOP + BAR_H) return 0;
    int ix = vol_icon_x(screen_w, clock_str);
    return mx >= ix - 4 && mx <= ix + VOL_ICON_W + 6;   /* generous hit box */
}
