/* taskbar.h — top bar for Aegis desktop shell */
#ifndef CITADEL_TASKBAR_H
#define CITADEL_TASKBAR_H

#include <glyph.h>

/* Draw the top bar: Aegis menu (left), a speaker icon + clock (right).
 * `volume` is the output level 0..100 (the speaker icon reflects it).
 * `backdrop_dirty` must be non-zero whenever the desktop background behind the
 * bar changed (a full redraw) so the cached frosted-glass blur is recomputed;
 * pass 0 to reuse the cache. */
void topbar_draw(surface_t *s, int screen_w, const char *clock_str, int volume,
                 int backdrop_dirty);

int topbar_hit_aegis(int mx, int my, int screen_w);

/* X where the focused app's menu titles begin (right of the brand icon). */
int topbar_appmenu_x(void);

/* Speaker-icon hit-testing (the icon toggles the compositor's volume popup).
 * _icon_x returns the icon's left x so the popup can be positioned under it. */
int topbar_volume_icon_x(int screen_w, const char *clock_str);
int topbar_volume_icon_hit(int mx, int my, int screen_w, const char *clock_str);

#endif
