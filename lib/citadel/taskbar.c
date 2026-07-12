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

/* Volume slider geometry (right side, just left of the clock). */
#define VOL_TRACK_W 56
#define VOL_GAP     18      /* gap between the track and the clock */
#define VOL_ICON_W  16      /* speaker icon + its gap, left of the track */

static int
clock_width(const char *clock_str)
{
    const char *c = (clock_str && clock_str[0]) ? clock_str : "00:00";
    if (g_font_ui) return font_text_width(g_font_ui, 14, c);
    return (int)strlen(c) * FONT_W;
}

/* Left x of the volume slider track. Clock is inset BAR_EDGE from the screen
 * edge so it sits inside the floating capsule. */
static int
vol_track_x(int screen_w, const char *clock_str)
{
    int clock_x = screen_w - clock_width(clock_str) - BAR_EDGE;
    return clock_x - VOL_GAP - VOL_TRACK_W;
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

    /* Volume widget: a small speaker icon + slider, left of the clock. */
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    int vx = vol_track_x(screen_w, clock_str);
    int vy = TOPBAR_HEIGHT / 2;

    /* Speaker: a small body rectangle + a cone widening to the right. */
    int ix = vx - VOL_ICON_W + 2;
    draw_fill_rect(s, ix, vy - 2, 3, 5, TOPBAR_TEXT);
    for (int dx = 0; dx <= 4; dx++)
        draw_fill_rect(s, ix + 3 + dx, vy - dx, 1, 2 * dx + 1, TOPBAR_TEXT);

    /* Track (faint), filled portion (accent), knob. */
    draw_blend_rect(s, vx, vy - 1, VOL_TRACK_W, 3, 0x00FFFFFF, 55);
    int fillw = VOL_TRACK_W * volume / 100;
    if (fillw > 0)
        draw_fill_rect(s, vx, vy - 1, fillw, 3, C_ACCENT);
    draw_circle_filled(s, vx + fillw, vy, 5, 0x00FFFFFF);
}

int
topbar_hit_aegis(int mx, int my, int screen_w)
{
    (void)screen_w;
    return mx >= BAR_MARGIN_SIDE && mx < BAR_MARGIN_SIDE + AEGIS_AREA_W &&
           my >= BAR_MARGIN_TOP && my < BAR_MARGIN_TOP + BAR_H;
}

int
topbar_volume_at(int mx, int my, int screen_w, const char *clock_str)
{
    if (my < BAR_MARGIN_TOP || my >= BAR_MARGIN_TOP + BAR_H) return -1;
    int vx = vol_track_x(screen_w, clock_str);
    if (mx < vx - 4 || mx > vx + VOL_TRACK_W + 4) return -1;   /* a touch wider */
    return topbar_volume_from_x(mx, screen_w, clock_str);
}

int
topbar_volume_from_x(int mx, int screen_w, const char *clock_str)
{
    int vx = vol_track_x(screen_w, clock_str);
    int v = (mx - vx) * 100 / VOL_TRACK_W;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    return v;
}
