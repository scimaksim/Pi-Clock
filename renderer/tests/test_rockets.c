/* Rocket picker: real Launch Library names -> the right drawing and
 * options; every vehicle draws without leaving cairo in an error state */
#include "check.h"
#include "../rocket_icons.c"

int main(void)
{
    struct { const char *rk, *ms; vkind_t kind; int nsrb, dragon, crew, longfair; } T[] = {
        { "Falcon 9", "Starlink Group 10-30", V_F9, 0, 0, 0, 0 },
        { "Falcon 9", "Crew-13", V_F9, 0, 2, 0, 0 },
        { "Falcon 9", "Dragon CRS-2 SpX-35", V_F9, 0, 1, 0, 0 },
        { "Falcon 9", "Cygnus CRS-2 NG-25", V_F9, 0, 0, 0, 0 },
        { "Falcon Heavy", "USSF-75", V_FH, 0, 0, 0, 0 },
        { "Starship", "Flight 13", V_STARSHIP, 0, 0, 0, 0 },
        { "Electron", "x", V_ELECTRON, 0, 0, 0, 0 },
        { "New Glenn", "x", V_NEWGLENN, 0, 0, 0, 0 },
        { "Vulcan VC6L", "x", V_VULCAN, 6, 0, 0, 1 },
        { "Vulcan VC4S", "x", V_VULCAN, 4, 0, 0, 0 },
        { "Vulcan", "x", V_VULCAN, 4, 0, 0, 0 },
        { "Atlas V 551", "x", V_ATLAS, 5, 0, 0, 0 },
        { "Space Launch System (SLS) Block 1", "Artemis III", V_SLS, 0, 0, 0, 0 },
        { "Soyuz 2.1a", "Soyuz MS-29", V_SOYUZ, 0, 0, 1, 0 },
        { "Soyuz 2.1b", "Progress MS-36", V_SOYUZ, 0, 0, 0, 0 },
        { "Ariane 64", "x", V_ARIANE6, 4, 0, 0, 0 },
        { "Ariane 62", "x", V_ARIANE6, 2, 0, 0, 0 },
        { "H3-24", "x", V_H3, 4, 0, 0, 0 },
        { "Long March 2F/G", "Shenzhou 24", V_LM, 0, 0, 0, 0 },
        { "Firefly Alpha Block 2", "x", V_DARK, 0, 0, 0, 0 },
        { "LVM-3 (GSLV Mk III)", "x", V_GENERIC, 2, 0, 0, 0 },
        { "PSLV-XL", "x", V_GENERIC, 6, 0, 0, 0 },
        { "Mir", "x", V_GENERIC, 0, 0, 0, 0 },
    };
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 200, 200);
    cairo_t *cr = cairo_create(s);
    int i;
    for (i = 0; i < (int)(sizeof(T) / sizeof(T[0])); i++) {
        veh_t v;
        pick(&v, T[i].rk, T[i].ms);
        CHECK(v.kind == T[i].kind, "%s: kind %d, want %d", T[i].rk, v.kind, T[i].kind);
        CHECK(v.nsrb == T[i].nsrb, "%s: %d solids, want %d", T[i].rk, v.nsrb, T[i].nsrb);
        CHECK(v.dragon == T[i].dragon, "%s / %s: dragon %d", T[i].rk, T[i].ms, v.dragon);
        CHECK(v.crew == T[i].crew, "%s / %s: crew %d", T[i].rk, T[i].ms, v.crew);
        CHECK(v.longfair == T[i].longfair, "%s: long fairing %d", T[i].rk, v.longfair);
        CHECK(v.L > 10 && v.L < 130, "%s: height %.1f m", T[i].rk, v.L);
        /* Sizes from normal down to degenerate must not break cairo */
        pic_draw_launch_icon(cr, T[i].rk, T[i].ms, 100, 190, 180, 1);
        pic_draw_launch_icon(cr, T[i].rk, T[i].ms, 100, 190, 30, 0);
        pic_draw_launch_icon(cr, T[i].rk, T[i].ms, 100, 190, 0, 0);
        pic_draw_launch_icon(cr, T[i].rk, T[i].ms, 100, 190, -5, 0);
        CHECK(cairo_status(cr) == CAIRO_STATUS_SUCCESS, "%s: cairo %s", T[i].rk,
              cairo_status_to_string(cairo_status(cr)));
    }
    cairo_destroy(cr);
    cairo_surface_destroy(s);
    return CHECK_DONE("test_rockets");
}
