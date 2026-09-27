/* Night Client core: config table, touch capture, FPS meter. Pure C, no game/ImGui deps. */
#ifndef NC_CORE_H
#define NC_CORE_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- config ----------------
 * X(name, type, default, min, max)   type: I = int, F = float
 * To add a setting for a new mod, add ONE line here. It is saved/loaded automatically. */
#define NC_FIELDS(X) \
  X(autosprint,   I, 1,    0, 1) \
  X(fps_on,       I, 0,    0, 1) X(fps_x, F, 0.02, 0, 1) X(fps_y, F, 0.02, 0, 1) X(fps_size, F, 3.0, 1.0, 8.0) \
  X(fps_alpha, F, 1.0, 0.05, 1) X(fps_col, I, 0xEBEBF5, 0, 0xFFFFFF) X(fps_bg_alpha, F, 0.75, 0.0, 1) X(fps_bg_col, I, 0x08080E, 0, 0xFFFFFF) X(fps_bg, I, 1, 0, 1) X(fps_menus, I, 1, 0, 1) \
  X(armor_on,     I, 1,    0, 1) X(armor_x, F, 0.005, 0, 1) X(armor_y, F, 0.03, 0, 1) X(armor_size, F, 3.0, 1.0, 8.0) \
  X(armor_alpha, F, 1.0, 0.05, 1) X(armor_bg_alpha, F, 0.75, 0.0, 1) X(armor_bg_col, I, 0x08080E, 0, 0xFFFFFF) X(armor_bg, I, 0, 0, 1) X(armor_bar, I, 0, 0, 1) X(armor_num, I, 1, 0, 2) \
  X(armor_horiz,  I, 0,    0, 1) X(armor_col, I, 0xEBEBF5, 0, 0xFFFFFF) \
  X(arrow_on,     I, 0,    0, 1) X(arrow_x, F, 0.50, 0, 1) X(arrow_y, F, 0.68, 0, 1) X(arrow_size, F, 3.0, 1.0, 8.0) \
  X(arrow_alpha, F, 1.0, 0.05, 1) X(arrow_bg_alpha, F, 0.75, 0.0, 1) X(arrow_col, I, 0xEBEBF5, 0, 0xFFFFFF) X(arrow_bg_col, I, 0x08080E, 0, 0xFFFFFF) X(arrow_bg, I, 1, 0, 1) \
  X(speed_on,     I, 0,    0, 1) X(speed_x, F, 0.50, 0, 1) X(speed_y, F, 0.74, 0, 1) X(speed_size, F, 3.0, 1.0, 8.0) \
  X(speed_alpha, F, 1.0, 0.05, 1) X(speed_bg_alpha, F, 0.75, 0.0, 1) X(speed_col, I, 0xEBEBF5, 0, 0xFFFFFF) X(speed_bg_col, I, 0x08080E, 0, 0xFFFFFF) X(speed_bg, I, 1, 0, 1) \
  X(elytra_on,    I, 1,    0, 1) X(elytra_x, F, 0.50, 0, 1) X(elytra_y, F, 0.56, 0, 1) X(elytra_size, F, 3.0, 1.0, 8.0) X(elytra_style, I, 0, 0, 2) \
  X(elytra_angle_on, I, 0, 0, 1) X(elytra_angle_x, F, 0.50, 0, 1) X(elytra_angle_y, F, 0.62, 0, 1) X(elytra_angle_size, F, 3.0, 1.0, 8.0) X(elytra_angle_alpha, F, 1.0, 0.05, 1) X(elytra_angle_bg_alpha, F, 0.75, 0.0, 1) X(elytra_angle_col, I, 0xEBEBF5, 0, 0xFFFFFF) X(elytra_angle_bg_col, I, 0x08080E, 0, 0xFFFFFF) X(elytra_angle_bg, I, 1, 0, 1) \
  X(elytra_alpha, F, 1.0, 0.05, 1) X(elytra_bg_alpha, F, 0.75, 0.0, 1) X(elytra_col, I, 0xAA8CFF, 0, 0xFFFFFF) X(elytra_bg_col, I, 0x08080E, 0, 0xFFFFFF) \
  X(nohurt,       I, 0,    0, 1) \
  X(zoom_on,      I, 0,    0, 1) X(zoom_x, F, 0.90, 0, 1) X(zoom_y, F, 0.30, 0, 1) X(zoom_btn, F, 3.0, 1.0, 8.0) \
  X(zoom_alpha,   F, 0.85, 0.0, 1) X(zoom_text_alpha, F, 1.0, 0.0, 1) X(zoom_bg_col, I, 0x38306E, 0, 0xFFFFFF) X(zoom_level, F, 4.0, 1.5, 12) X(zoom_label, I, 0, 0, 7) \
  X(zoom_col,     I, 0x38306E, 0, 0xFFFFFF) X(zoom_pause, I, 0, 0, 1) \
  X(persp_on,     I, 0,    0, 1) X(persp_x, F, 0.90, 0, 1) X(persp_y, F, 0.46, 0, 1) X(persp_btn, F, 3.0, 1.0, 8.0) \
  X(persp_alpha,  F, 0.85, 0.0, 1) X(persp_text_alpha, F, 1.0, 0.0, 1) X(persp_bg_col, I, 0x38306E, 0, 0xFFFFFF) X(persp_label, I, 0, 0, 7) \
  X(persp_col,    I, 0x38306E, 0, 0xFFFFFF) X(persp_pause, I, 0, 0, 1) \
  X(drop_on,      I, 0,    0, 1) X(drop_x, F, 0.90, 0, 1) X(drop_y, F, 0.62, 0, 1) X(drop_btn, F, 3.0, 1.0, 8.0) \
  X(drop_alpha,   F, 0.85, 0.0, 1) X(drop_text_alpha, F, 1.0, 0.0, 1) X(drop_bg_col, I, 0x38306E, 0, 0xFFFFFF) X(drop_label, I, 0, 0, 7) X(drop_col, I, 0x38306E, 0, 0xFFFFFF) X(drop_pause, I, 0, 0, 1) \
  X(fast_totem_on, I, 1, 0, 1) X(fast_totem_x, F, 0.78, 0, 1) X(fast_totem_y, F, 0.86, 0, 1) X(fast_totem_btn, F, 3.0, 1.0, 12.0) \
  X(fast_totem_alpha, F, 0.90, 0.0, 1) X(fast_totem_text_alpha, F, 1.0, 0.0, 1) \
  X(fast_totem_col, I, 0xF0F0FA, 0, 0xFFFFFF) X(fast_totem_bg_col, I, 0x38306E, 0, 0xFFFFFF) \
  X(perf_gfx,     I, 0,    0, 1) X(perf_light, I, 0, 0, 1) X(perf_skies, I, 0, 0, 1) X(perf_bob, I, 0, 0, 1) \
  X(perf_view_on, I, 0,    0, 1) X(perf_view, I, 6, 2, 16) \
  X(ui_font,      I, 0,    0, 6) \
  X(n_x,          F, 0.92, 0, 1) X(n_y, F, 0.80, 0, 1) X(n_btn, F, 3.0, 1.0, 8.0) X(n_alpha, F, 1.0, 0.0, 1) X(n_text_alpha, F, 1.0, 0.0, 1) X(n_col, I, 0xF0F0FA, 0, 0xFFFFFF) X(n_bg_col, I, 0x38306E, 0, 0xFFFFFF) \
  X(n_always,     I, 0,    0, 1) X(n_label, I, 0, 0, 7) \
  X(hitbox_on,    I, 0,    0, 1) X(hook_hitbox, I, 1, 0, 1) \
  X(hook_hurt,    I, 1,    0, 1) X(hook_fov, I, 1, 0, 1) X(hook_persp, I, 1, 0, 1) X(hook_perf, I, 1, 0, 1)

#define NC_TYPE_I int
#define NC_TYPE_F float
#define NC_DECL(n, t, d, mn, mx) NC_TYPE_##t n;
typedef struct {
    NC_FIELDS(NC_DECL)
    char zoom_text[17];
    char persp_text[17];
    char drop_text[17];
    char n_text[17];
} NcConfig;

void nc_cfg_defaults(NcConfig *c);
int  nc_cfg_load(NcConfig *c, const char *path);       /* returns 1 if the file existed */
int  nc_cfg_save(const NcConfig *c, const char *path); /* returns 1 on success */
int  nc_cfg_equal(const NcConfig *a, const NcConfig *b);

/* ---------------- touch capture ---------------- */
enum { NC_EV_DOWN = 1, NC_EV_MOVE = 2, NC_EV_UP = 3 };
#define NC_MAX_PTR 10
#define NC_MAX_HUD 8

typedef struct { int visible; float x, y, w, h; } NcRect;
typedef struct {
    NcRect n;                 /* the N button */
    NcRect win;               /* modal area (menu / edit mode): while visible, every touch is ours */
    NcRect hud[NC_MAX_HUD];   /* in-game buttons */
    int    cap_id;            /* pointer id we hold, or -1 */
} NcTouch;

/* one Android motion event, already unpacked */
typedef struct {
    int   action;             /* masked action: 0 down, 1 up, 2 move, 3 cancel, 5 pointer down, 6 pointer up */
    int   idx;                /* which pointer the action refers to */
    int   count;
    int   id[NC_MAX_PTR];
    float x[NC_MAX_PTR], y[NC_MAX_PTR];
} NcMotion;

typedef void (*NcPush)(void *user, int type, float x, float y);

void nc_touch_init(NcTouch *t);
/* Returns 1 if the event must NOT reach the game. Only the finger that pressed one of our
 * controls is taken; other fingers keep working in the game. */
int nc_touch_event(NcTouch *t, const NcMotion *m, NcPush push, void *user);

/* ---------------- fps ---------------- */
typedef struct { double window_start; int frames; float fps; } NcFps;
float nc_fps_frame(NcFps *f, double now);

#ifdef __cplusplus
}
#endif
#endif
