/*
 * Night Client 0.2 - Toolbox mod for Minecraft PE 1.1.5 (32-bit ARM)
 *
 * Mods: Autosprint, FPS counter, Armor HUD, Elytra indicator, No hurt cam, Zoom,
 *       Perspective button, FPS optimizer.
 * UI:   N button (only in Settings) -> dark menu with a mod list and a config panel,
 *       plus a "Move on screen" mode where you drag every HUD element with your finger.
 * All settings are saved to games/com.mojang/NightClient/config.txt
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <float.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <jni.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/input.h>

#include "imgui.h"
#include "imgui_impl_opengl3.h"

extern "C" {
#include "nc_core.h"
#include "gothook.h"
}
#include "nc_font.h"
#include "nc_icons.h"

#define NC_VERSION "0.4.4"
#define NC_DIR "/sdcard/games/com.mojang/NightClient/"
#define NC_CFG NC_DIR "config.txt"
#define NC_LOG NC_DIR "log.txt"

/* ---- game functions (found in libminecraftpe.so when the mod loads) ---- */
extern "C" bool mih_isMovingForward(void *self) __asm__("_ZNK16MoveInputHandler15isMovingForwardEv");
extern "C" bool mob_isSneaking(void *self)      __asm__("_ZNK3Mob10isSneakingEv");
extern "C" bool mob_isSprinting(void *self)     __asm__("_ZNK3Mob11isSprintingEv");
extern "C" bool mob_isGliding(void *self)       __asm__("_ZNK3Mob9isGlidingEv");
extern "C" bool player_isUsingItem(void *self)  __asm__("_ZNK6Player11isUsingItemEv");
extern "C" void lp_setSprinting(void *self, bool on) __asm__("_ZN11LocalPlayer12setSprintingEb");
extern "C" const void *mob_getArmor(void *self, int slot) __asm__("_ZNK3Mob8getArmorE9ArmorSlot");
extern "C" bool ii_isNull(const void *it)       __asm__("_ZNK12ItemInstance6isNullEv");
extern "C" int  ii_getId(const void *it)        __asm__("_ZNK12ItemInstance5getIdEv");
extern "C" int  ii_getDamage(const void *it)    __asm__("_ZNK12ItemInstance14getDamageValueEv");
extern "C" int  ii_getMaxDamage(const void *it) __asm__("_ZNK12ItemInstance12getMaxDamageEv");
extern "C" void cic_toggle3rd(void *self, void *ci)
    __asm__("_ZN20ClientInputCallbacks38handleToggleThirdPersonViewButtonPressER14ClientInstance");
extern "C" void cic_drop(void *self, void *ci)
    __asm__("_ZN20ClientInputCallbacks21handleDropButtonPressER14ClientInstance");
extern "C" const void *player_getSelectedItem(void *self) __asm__("_ZNK6Player15getSelectedItemEv");
extern "C" const float *entity_getPos(void *self) __asm__("_ZNK6Entity6getPosEv");

struct NcVec2 { float x, y; };
struct NcVec3 { float x, y, z; };
extern "C" void entity_getInterpolatedPosition(NcVec3 *out, void *self, float a)
    __asm__("_ZNK6Entity23getInterpolatedPositionEf");
extern "C" void entity_getInterpolatedRotation(NcVec2 *out, void *self, float a)
    __asm__("_ZNK6Entity23getInterpolatedRotationEf");
/*
 * Arrow inventory access is intentionally not called yet.
 *
 * The previous implementation used Entity::getInventory() and
 * PlayerInventoryProxy::getItemCount(), but that combination is not ABI-safe
 * on the exact 1.1.5 binary and caused a crash when entering a world.
 * Keep the HUD itself alive while we use a verified inventory path.
 */

extern "C" void entity_getRotation(NcVec2 *out, void *self) __asm__("_ZNK6Entity11getRotationEv");

/* ---- Toolbox mod loader: hook registration (libmodloader.so) ---- */
extern "C" void tml_registerHook(const char *symbol, void *hook, void **original)
    __asm__("_ZN3tml17StaticHookManager12registerHookEPKcPvPS3_");

/* ------------------------------------------------------------------ state */
static NcConfig g_cfg, g_saved;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;   /* guards g_touch + event queue */
static NcTouch g_touch;
static struct { int type; float x, y; } g_q[256];
static int g_qn = 0;

static void *volatile g_settings_this = 0;   /* the open SettingsScreenController, if any */
static void *volatile g_pause_this = 0;      /* the open PauseScreenController, if any */
static void *volatile g_cic = 0;             /* ClientInputCallbacks* (captured) */
static void *volatile g_ci = 0;              /* ClientInstance* (captured) */
static void *g_apply_self = 0;               /* which gameplay screen instance g_ci was captured for */
static volatile double g_play_time = 0;      /* last time the gameplay screen was on top */
static volatile double g_tick_time = 0;      /* last time the local player ticked */
static volatile int    g_zoom_active = 0;
static float           g_zoom_cur = 1.0f;
static struct { volatile int gliding; int present[4], id[4], dur[4], max[4]; int holding_bow; float speed_bps; int arrow_count; float elytra_angle; int elytra_angle_valid; float x, y, z; } g_snap;
static float g_last_px = 0.0f, g_last_py = 0.0f, g_last_pz = 0.0f;
static double g_last_pos_time = 0.0;
static bool g_have_last_pos = false;

static bool  g_menu_open = false, g_edit = false;
static int   g_sel = 0;
static bool  g_imgui_ready = false, g_imgui_failed = false, g_tex_done = false;
static float g_base = 2.0f;                  /* layout scale chosen from the screen height */
static float g_w = 1920, g_h = 1080;
static NcFps g_fps;
static int   g_log_lines = 0, g_frames = 0, g_beats = 0, g_touch_logged = 0;
static bool  g_dbg_logged = false;
static bool  g_drawing_logged = false, g_gl_err_logged = false;
static GLuint g_icon_tex[NC_ICON_COUNT];
static JavaVM *g_jvm = 0;
static jobject g_kb_activity = 0;
static bool g_kb_open = false;
static int g_kb_status = -1;
static int g_kb_target = 0; /* 1 zoom, 2 perspective, 3 drop, 4 N */
static char *g_kb_text = 0;

/* Forward declaration: the keyboard bridge is above the logger definition. */
static void nclog(const char *fmt, ...);

typedef jint (*fn_JNI_GetCreatedJavaVMs)(JavaVM **, jsize, jsize *);

static bool kb_get_vm() {
    if (g_jvm) return true;
    void *art = dlopen("libart.so", RTLD_NOW | RTLD_NOLOAD);
    if (!art) art = dlopen("libart.so", RTLD_NOW);
    if (!art) return false;
    fn_JNI_GetCreatedJavaVMs getv = (fn_JNI_GetCreatedJavaVMs)dlsym(art, "JNI_GetCreatedJavaVMs");
    if (!getv) return false;
    jsize n = 0;
    if (getv(&g_jvm, 1, &n) != JNI_OK || n <= 0 || !g_jvm) { g_jvm = 0; return false; }
    return true;
}

static JNIEnv *kb_env(bool *attached) {
    *attached = false;
    if (!kb_get_vm()) return 0;
    JNIEnv *env = 0;
    jint r = g_jvm->GetEnv((void **)&env, JNI_VERSION_1_6);
    if (r == JNI_EDETACHED) {
        if (g_jvm->AttachCurrentThread(&env, 0) != JNI_OK) return 0;
        *attached = true;
    }
    return env;
}

static void kb_release(JNIEnv *env, bool attached) { if (attached && g_jvm) g_jvm->DetachCurrentThread(); (void)env; }

/* Finds the current Activity through ActivityThread. This is deliberately best-effort:
 * if the old MCPE build exposes no Java VM/activity, the text field still works as an
 * ImGui field but no soft keyboard is requested. */
static jobject kb_find_activity(JNIEnv *env) {
    /* MCPE 1.1.5 keeps the real MainActivity in a public static singleton.
     * ActivityThread/mActivities reflection is unreliable on newer Android
     * releases, which is why the old keyboard bridge could create the ImGui
     * cursor but never bring up the IME. */
    jclass mc = env->FindClass("com/mojang/minecraftpe/MainActivity");
    if (!mc) return 0;
    jfieldID inst = env->GetStaticFieldID(mc, "mInstance", "Lcom/mojang/minecraftpe/MainActivity;");
    if (!inst) { env->DeleteLocalRef(mc); return 0; }
    jobject obj = env->GetStaticObjectField(mc, inst);
    if (!obj) { env->DeleteLocalRef(mc); return 0; }
    jobject global = env->NewGlobalRef(obj);
    env->DeleteLocalRef(obj);
    env->DeleteLocalRef(mc);
    return global;
}

static bool kb_start(int target, char *text) {
    bool attached = false;
    JNIEnv *env = kb_env(&attached);
    if (!env) return false;

    if (!g_kb_activity)
        g_kb_activity = kb_find_activity(env);
    if (!g_kb_activity) {
        nclog("keyboard: MainActivity.mInstance unavailable");
        kb_release(env, attached);
        return false;
    }

    jclass ac = env->GetObjectClass(g_kb_activity);
    jmethodID show = ac ? env->GetMethodID(ac, "showKeyboard", "(Ljava/lang/String;IZZ)V") : 0;
    if (!show) {
        nclog("keyboard: showKeyboard method unavailable");
        if (ac) env->DeleteLocalRef(ac);
        kb_release(env, attached);
        return false;
    }

    jstring js = env->NewStringUTF(text ? text : "");
    g_kb_target = target;
    g_kb_text = text;
    g_kb_status = -1;
    g_kb_open = false;

    /* MainActivity.showKeyboard() is MCPE's own input bridge.  It creates and
     * focuses TextInputProxyEditTextbox on the UI thread internally, so do not
     * create another EditText or call InputMethodManager ourselves. */
    env->CallVoidMethod(g_kb_activity, show, js, 16, JNI_FALSE, JNI_FALSE);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        nclog("keyboard: showKeyboard threw");
    } else {
        g_kb_open = true;
        nclog("keyboard: showKeyboard requested");
    }

    if (js) env->DeleteLocalRef(js);
    if (ac) env->DeleteLocalRef(ac);
    kb_release(env, attached);
    return g_kb_open;
}

static void kb_poll() {
    if (!g_kb_open || !g_kb_text || !g_kb_activity) return;

    bool attached = false;
    JNIEnv *env = kb_env(&attached);
    if (!env) return;

    jclass ac = env->GetObjectClass(g_kb_activity);
    jmethodID get = ac ? env->GetMethodID(ac, "getUserInputString", "()[Ljava/lang/String;") : 0;
    jmethodID status = ac ? env->GetMethodID(ac, "getUserInputStatus", "()I") : 0;

    if (get) {
        jobjectArray arr = (jobjectArray)env->CallObjectMethod(g_kb_activity, get);
        if (!env->ExceptionCheck() && arr) {
            jsize n = env->GetArrayLength(arr);
            if (n > 0) {
                jstring js = (jstring)env->GetObjectArrayElement(arr, 0);
                if (js) {
                    const char *u = env->GetStringUTFChars(js, 0);
                    if (u) {
                        strncpy(g_kb_text, u, 16);
                        g_kb_text[16] = 0;
                        env->ReleaseStringUTFChars(js, u);
                    }
                    env->DeleteLocalRef(js);
                }
            }
            env->DeleteLocalRef(arr);
        } else if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
    }

    /* MCPE uses -1 while the hidden Android textbox is still active.  The old
     * implementation treated 1/0 backwards and immediately called hideKeyboard()
     * on the very first frame, so the real keyboard never became visible. */
    int st = 0;
    if (status) {
        st = env->CallIntMethod(g_kb_activity, status);
        if (env->ExceptionCheck()) { env->ExceptionClear(); st = 0; }
    }

    if (ac) env->DeleteLocalRef(ac);
    kb_release(env, attached);

    if (st != -1) {
        /* hideKeyboard is idempotent in the 1.1.5 activity implementation. */
        bool a2 = false;
        JNIEnv *e2 = kb_env(&a2);
        if (e2) {
            jclass c2 = e2->GetObjectClass(g_kb_activity);
            jmethodID hide = c2 ? e2->GetMethodID(c2, "hideKeyboard", "()V") : 0;
            if (hide) e2->CallVoidMethod(g_kb_activity, hide);
            if (e2->ExceptionCheck()) e2->ExceptionClear();
            if (c2) e2->DeleteLocalRef(c2);
            kb_release(e2, a2);
        }
        g_kb_open = false;
        g_kb_status = st;
        g_kb_target = 0;
        g_kb_text = 0;
    }
}

static void kb_stop() {
    if (!g_kb_open || !g_kb_activity) {
        g_kb_open = false;
        g_kb_status = -1;
        g_kb_target = 0;
        g_kb_text = 0;
        return;
    }

    bool attached = false;
    JNIEnv *env = kb_env(&attached);
    if (!env) return;
    jclass ac = env->GetObjectClass(g_kb_activity);
    jmethodID hide = ac ? env->GetMethodID(ac, "hideKeyboard", "()V") : 0;
    if (hide) env->CallVoidMethod(g_kb_activity, hide);
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (ac) env->DeleteLocalRef(ac);
    g_kb_open = false;
    g_kb_target = 0;
    g_kb_text = 0;
    kb_release(env, attached);
}


/* ------------------------------------------------------------------ helpers */
static void nclog(const char *fmt, ...) {
    if (g_log_lines > 300) return;
    g_log_lines++;
    FILE *f = fopen(NC_LOG, "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f);
    fclose(f);
}
static double now_s() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
static ImVec2 V(float x, float y) { return ImVec2(x, y); }
static ImVec2 vadd(ImVec2 a, ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static ImU32 rgba(int r, int g, int b, float a) { return IM_COL32(r, g, b, (int)(clampf(a, 0, 1) * 255.0f)); }
static ImU32 packed(int rgb, float a) { return rgba((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff, a); }
static void color_picker(const char *label, int *rgb) {
    float c[3] = { ((*rgb >> 16) & 0xff) / 255.0f, ((*rgb >> 8) & 0xff) / 255.0f, (*rgb & 0xff) / 255.0f };
    if (ImGui::ColorEdit3(label, c, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel))
        *rgb = ((int)(c[0] * 255.0f + 0.5f) << 16) | ((int)(c[1] * 255.0f + 0.5f) << 8) | (int)(c[2] * 255.0f + 0.5f);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", label);
}

/* ------------------------------------------------------------------ game hooks */
typedef void  (*fn_this)(void *);
typedef void  (*fn_tick)(void *, void *);
typedef void  (*fn_apply)(void *, float);
typedef void  (*fn_bob)(void *, void *, float);
typedef float (*fn_fov)(void *, float, bool);
typedef int   (*fn_ptr)(void *, void *, void *, int);
typedef bool  (*fn_getb)(void *);
typedef int   (*fn_geti)(void *);
typedef void  (*fn_entity_render)(void *, void *, const void *, float, float);
static fn_this  g_orig_onOpen = 0, g_orig_dtor = 0;
static fn_tick  g_orig_tick = 0;
static fn_apply g_orig_apply = 0;
static fn_bob   g_orig_bob = 0;
static fn_fov   g_orig_fov = 0;
static fn_ptr   g_orig_ptr = 0;
static fn_getb  g_orig_fancy = 0, g_orig_skies = 0, g_orig_light = 0, g_orig_bobview = 0;
static fn_geti  g_orig_view = 0;
static fn_entity_render g_orig_entity_render = 0;
static fn_entity_render g_orig_xp_render = 0;
static fn_entity_render g_orig_crystal_render = 0;
static fn_entity_render g_orig_crystal_effects = 0;
static void *g_local_player = 0;

/* Exact 1.1.5 Entity::bb layout: Entity + 0x104 contains
 * six floats in AABB order: minX,minY,minZ,maxX,maxY,maxZ.
 * We discovered this from Entity::setSize() and AABB::set(). */
struct NcAabb6 { float minx, miny, minz, maxx, maxy, maxz; };

/* MatrixStack globals are exported data symbols in the 1.1.5 lib.
 * They are MatrixStack* variables; dlsym() gives us their storage address. */
typedef void *(*fn_matrix_get_top)(void *);
static void **g_projection_slot = 0;
static void **g_view_slot = 0;
static fn_matrix_get_top g_matrix_get_top = 0;
static GLuint g_hit_prog = 0, g_hit_vbo = 0;
static GLint g_hit_mvp = -1, g_hit_col = -1;
static bool g_hit_gl_ready = false;
static bool g_hit_symbols_ready = false;
static bool g_hit_logged_symbols = false;
static uintptr_t g_hit_dragon_vtable = 0;
static bool g_hit_dragon_ready = false;
static void *g_hit_seen[256];
static int g_hit_seen_n = 0;

static int snapshot_arrow_count(void *player) {
    /*
     * Do not call the unverified 1.1.5 inventory APIs here.
     *
     * Returning -1 keeps the HUD in its old "not yet available" state rather
     * than touching an incompatible inventory object and crashing the client.
     * The actual total-arrow-count path will be added only after its ABI is
     * verified against this exact 1.1.5 libminecraftpe.so.
     */
    (void)player;
    return -1;
}

static void hook_settings_open(void *self) {
    if (g_orig_onOpen) g_orig_onOpen(self);
    g_settings_this = self;
    nclog("settings screen opened");
}
static void hook_settings_dtor(void *self) {
    if (self == g_settings_this) { g_settings_this = 0; nclog("settings screen closed"); }
    if (g_orig_dtor) g_orig_dtor(self);
}

/* PauseScreenController has no exported onOpen, so we mark it open on its first tick
 * and closed on destruction - it ticks every frame while it is on screen. */
typedef void (*fn_pausetick)(void *);
static fn_pausetick g_orig_pausetick = 0;
static fn_this g_orig_pausedtor = 0;
static void hook_pause_tick(void *self) {
    if (g_pause_this != self) { g_pause_this = self; nclog("pause screen opened"); }
    if (g_orig_pausetick) g_orig_pausetick(self);
}
static void hook_pause_dtor(void *self) {
    if (self == g_pause_this) { g_pause_this = 0; nclog("pause screen closed"); }
    if (g_orig_pausedtor) g_orig_pausedtor(self);
}

/* InGamePlayScreen::applyInput(float): runs only while the gameplay screen is on top */
static void hook_apply(void *self, float dt) {
    g_play_time = now_s();
    if (self != g_apply_self) {
        if (g_apply_self) nclog("gameplay screen changed - clearing captured game pointers");
        g_apply_self = self;
        g_cic = 0; g_ci = 0; g_dbg_logged = false;
    }
    if (g_orig_apply) g_orig_apply(self, dt);
}

/* MoveInputHandler::tick(LocalPlayer&): autosprint + data snapshot for the HUD mods */
static void hook_tick(void *self, void *player) {
    if (g_orig_tick) g_orig_tick(self, player);
    if (!player) return;
    g_local_player = player;
    g_tick_time = now_s();
    const float *pos = entity_getPos(player);
    if (pos) {
        g_snap.x = pos[0]; g_snap.y = pos[1]; g_snap.z = pos[2];
    }
    if (g_cfg.speed_on) {
        double t = g_tick_time;
        if (pos) {
            float px = pos[0], py = pos[1], pz = pos[2];
            if (g_have_last_pos) {
                double dt = t - g_last_pos_time;
                if (dt > 0.001 && dt < 1.0) {
                    float dx = px - g_last_px, dz = pz - g_last_pz;
                    float dist = sqrtf(dx * dx + dz * dz);
                    float bps = dist / (float)dt;
                    if (isfinite(bps)) g_snap.speed_bps = clampf(bps, 0.0f, 1000.0f);
                }
            }
            g_last_px = px; g_last_py = py; g_last_pz = pz;
            g_last_pos_time = t;
            g_have_last_pos = true;
        }
    } else {
        g_snap.speed_bps = 0.0f;
        g_have_last_pos = false;
    }
    if (g_cfg.elytra_angle_on) {
        NcVec2 rot = {0.0f, 0.0f};
        entity_getRotation(&rot, player);
        if (isfinite(rot.x)) {
            g_snap.elytra_angle = rot.x;
            g_snap.elytra_angle_valid = 1;
        } else g_snap.elytra_angle_valid = 0;
    } else {
        g_snap.elytra_angle_valid = 0;
    }
    if (g_cfg.arrow_on) g_snap.arrow_count = snapshot_arrow_count(player);
    else g_snap.arrow_count = -1;
    if (g_cfg.elytra_on) g_snap.gliding = mob_isGliding(player) ? 1 : 0;
    if (g_cfg.arrow_on) {
        const void *held = player_getSelectedItem(player);
        g_snap.holding_bow = (held && !ii_isNull(held) && ii_getId(held) == 261) ? 1 : 0;   /* 261 = bow */
    }
    if (g_cfg.armor_on) {
        for (int i = 0; i < 4; i++) {
            const void *it = mob_getArmor(player, i);
            if (it && !ii_isNull(it)) {
                g_snap.id[i] = ii_getId(it);
                g_snap.max[i] = ii_getMaxDamage(it);
                g_snap.dur[i] = g_snap.max[i] - ii_getDamage(it);
                g_snap.present[i] = 1;
            } else g_snap.present[i] = 0;
        }
    }
    if (g_cfg.autosprint && mih_isMovingForward(self) && !mob_isSneaking(player) &&
        !player_isUsingItem(player) && !mob_isSprinting(player))
        lp_setSprinting(player, true);
}

/* No hurt cam */
static void hook_bobhurt(void *self, void *m, float t) {
    if (g_cfg.nohurt) return;
    if (g_orig_bob) g_orig_bob(self, m, t);
}

/* Zoom: smoothly divides the field of view */
static float hook_fov(void *self, float pt, bool world) {
    float f = g_orig_fov ? g_orig_fov(self, pt, world) : 70.0f;
    if (!world) return f;                       /* the call with flag=false is the hand: leave it alone */
    float target = (g_cfg.zoom_on && g_zoom_active) ? g_cfg.zoom_level : 1.0f;
    g_zoom_cur += (target - g_zoom_cur) * 0.30f;
    if (fabsf(target - g_zoom_cur) < 0.01f) g_zoom_cur = target;
    return g_zoom_cur > 1.001f ? f / g_zoom_cur : f;
}

/* Perspective button: remember the objects the game's own handler needs */
static int hook_ptr(void *self, void *ci, void *data, int focus) {
    g_cic = self; g_ci = ci;
    return g_orig_ptr ? g_orig_ptr(self, ci, data, focus) : 0;
}


/* ------------------------------------------------------------------ researched 1.1.5 hitbox renderer
 *
 * The native EntityRenderDispatcher::renderDebug(Entity&) symbol is NOT the
 * Java-style hitbox drawer in this exact 1.1.5 build.  It only dispatches to
 * each entity renderer's debug virtual.  MobRenderer::renderDebug() is mostly
 * navigation/debug information, not the desired AABB renderer.
 *
 * Instead we hook the REAL entity render call.  Its Vec3 parameter is already
 * the interpolated entity position relative to the game's camera/player offset.
 * The game's Entity AABB lives at Entity + 0x104.  We transform that AABB through
 * the game's own Projection and View MatrixStack tops, so there is no guessed
 * FOV/camera math.  Drawing happens immediately after the entity renderer and
 * while the world depth buffer is still active, so blocks can occlude it.
 */
static void *hit_dlsym(const char *name) {
    void *p = dlsym(RTLD_DEFAULT, name);
    return p;
}

static bool hit_resolve_symbols() {
    if (g_hit_symbols_ready) return true;
    if (!g_hit_dragon_ready) {
        void *vt = hit_dlsym("_ZTV11EnderDragon");
        if (vt) g_hit_dragon_vtable = (uintptr_t)vt + sizeof(void*) * 2;
        g_hit_dragon_ready = true;
    }
    g_projection_slot = (void **)hit_dlsym("_ZN11MatrixStack10ProjectionE");
    g_view_slot       = (void **)hit_dlsym("_ZN11MatrixStack4ViewE");
    g_matrix_get_top  = (fn_matrix_get_top)hit_dlsym("_ZN11MatrixStack6getTopEv");
    if (!g_projection_slot || !g_view_slot || !g_matrix_get_top) {
        if (!g_hit_logged_symbols) {
            g_hit_logged_symbols = true;
            nclog("hitboxes: MatrixStack symbols unavailable p=%p v=%p top=%p",
                  g_projection_slot, g_view_slot, (void *)g_matrix_get_top);
        }
        return false;
    }
    g_hit_symbols_ready = true;
    if (!g_hit_logged_symbols) {
        g_hit_logged_symbols = true;
        nclog("hitboxes: MatrixStack symbols resolved");
    }
    return true;
}

static GLuint hit_compile_shader(GLenum type, const char *src) {
    GLuint sh = glCreateShader(type);
    if (!sh) return 0;
    glShaderSource(sh, 1, &src, 0);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static bool hit_init_gl() {
    if (g_hit_gl_ready) return true;
    const char *vs =
        "attribute vec3 aPos;"
        "uniform mat4 uMVP;"
        "void main(){ gl_Position = uMVP * vec4(aPos,1.0); }";
    const char *fs =
        "precision mediump float;"
        "uniform vec4 uColor;"
        "void main(){ gl_FragColor = uColor; }";

    GLuint v = hit_compile_shader(GL_VERTEX_SHADER, vs);
    GLuint f = hit_compile_shader(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) {
        if (v) glDeleteShader(v);
        if (f) glDeleteShader(f);
        nclog("hitboxes: shader compile failed");
        return false;
    }

    GLuint p = glCreateProgram();
    if (!p) {
        glDeleteShader(v); glDeleteShader(f);
        return false;
    }
    glAttachShader(p, v);
    glAttachShader(p, f);
    glBindAttribLocation(p, 0, "aPos");
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);

    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        glDeleteProgram(p);
        nclog("hitboxes: program link failed");
        return false;
    }

    g_hit_prog = p;
    g_hit_mvp = glGetUniformLocation(p, "uMVP");
    g_hit_col = glGetUniformLocation(p, "uColor");
    glGenBuffers(1, &g_hit_vbo);
    if (!g_hit_vbo || g_hit_mvp < 0 || g_hit_col < 0) {
        if (g_hit_vbo) glDeleteBuffers(1, &g_hit_vbo);
        glDeleteProgram(g_hit_prog);
        g_hit_prog = 0;
        return false;
    }

    g_hit_gl_ready = true;
    nclog("hitboxes: GL renderer initialized");
    return true;
}

static void hit_copy_matrix(void *stack, float out[16]) {
    const void *top = g_matrix_get_top ? g_matrix_get_top(stack) : 0;
    if (!top) {
        memset(out, 0, sizeof(float) * 16);
        return;
    }
    memcpy(out, top, sizeof(float) * 16);
}

static void hit_identity(float *m) {
    memset(m, 0, sizeof(float) * 16);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void hit_mul(float *out, const float *a, const float *b) {
    float r[16];
    /* Column-major matrix multiplication, matching OpenGL/Matrix::ptr(). */
    for (int c = 0; c < 4; ++c) {
        for (int rr = 0; rr < 4; ++rr) {
            r[c * 4 + rr] =
                a[0 * 4 + rr] * b[c * 4 + 0] +
                a[1 * 4 + rr] * b[c * 4 + 1] +
                a[2 * 4 + rr] * b[c * 4 + 2] +
                a[3 * 4 + rr] * b[c * 4 + 3];
        }
    }
    memcpy(out, r, sizeof(r));
}

static void hit_translate(float *m, float x, float y, float z) {
    hit_identity(m);
    m[12] = x;
    m[13] = y;
    m[14] = z;
}

static void hit_v(float *v, int *n, float x, float y, float z) {
    v[*n * 3 + 0] = x;
    v[*n * 3 + 1] = y;
    v[*n * 3 + 2] = z;
    ++(*n);
}
static void hit_line(float *v, int *n,
                     float ax, float ay, float az,
                     float bx, float by, float bz) {
    hit_v(v, n, ax, ay, az);
    hit_v(v, n, bx, by, bz);
}

static void hit_aabb_edges(float *v, int *n, const NcAabb6 &b) {
    /* 12 edges => 24 vertices. */
    hit_line(v,n,b.minx,b.miny,b.minz, b.maxx,b.miny,b.minz);
    hit_line(v,n,b.maxx,b.miny,b.minz, b.maxx,b.miny,b.maxz);
    hit_line(v,n,b.maxx,b.miny,b.maxz, b.minx,b.miny,b.maxz);
    hit_line(v,n,b.minx,b.miny,b.maxz, b.minx,b.miny,b.minz);

    hit_line(v,n,b.minx,b.maxy,b.minz, b.maxx,b.maxy,b.minz);
    hit_line(v,n,b.maxx,b.maxy,b.minz, b.maxx,b.maxy,b.maxz);
    hit_line(v,n,b.maxx,b.maxy,b.maxz, b.minx,b.maxy,b.maxz);
    hit_line(v,n,b.minx,b.maxy,b.maxz, b.minx,b.maxy,b.minz);

    hit_line(v,n,b.minx,b.miny,b.minz, b.minx,b.maxy,b.minz);
    hit_line(v,n,b.maxx,b.miny,b.minz, b.maxx,b.maxy,b.minz);
    hit_line(v,n,b.maxx,b.miny,b.maxz, b.maxx,b.maxy,b.maxz);
    hit_line(v,n,b.minx,b.miny,b.maxz, b.minx,b.maxy,b.maxz);
}

static void hit_look_line(float *v, int *n, const NcAabb6 &b, float pitch, float yaw) {
    const float k = 0.01745329251994329577f;
    float p = pitch * k;
    float y = yaw * k;
    float cp = cosf(p), sp = sinf(p);
    float sy = sinf(y), cy = cosf(y);

    /* MC/PE convention: yaw 0 points toward -Z. */
    float dx = -sy * cp;
    float dy = -sp;
    float dz =  cy * cp;

    float sx = (b.minx + b.maxx) * 0.5f;
    float sy0 = b.maxy - 0.12f;
    float sz = (b.minz + b.maxz) * 0.5f;
    const float len = 1.25f;
    hit_line(v, n, sx, sy0, sz,
             sx + dx * len, sy0 + dy * len, sz + dz * len);
}

static bool hit_is_dragon(void *entity) {
    if (!entity || !g_hit_dragon_vtable) return false;
    uintptr_t vt = *(const uintptr_t *)entity;
    return vt == g_hit_dragon_vtable;
}

static bool hit_seen_entity(void *entity) {
    for (int i = 0; i < g_hit_seen_n; ++i)
        if (g_hit_seen[i] == entity) return true;
    if (g_hit_seen_n < (int)(sizeof(g_hit_seen) / sizeof(g_hit_seen[0])))
        g_hit_seen[g_hit_seen_n++] = entity;
    return false;
}

static void hit_add_box(float *v, int *n, const NcAabb6 &b) {
    hit_aabb_edges(v, n, b);
}

static void hit_box_center(float *v, int *n, float cx, float cy, float cz,
                          float hx, float hy, float hz) {
    NcAabb6 b;
    b.minx = cx - hx; b.miny = cy - hy; b.minz = cz - hz;
    b.maxx = cx + hx; b.maxy = cy + hy; b.maxz = cz + hz;
    hit_aabb_edges(v, n, b);
}

/*
 * EnderDragon is a multipart entity in the old PE renderer.  Its single Entity::bb
 * is the coarse overall bounds; drawing that alone looks wrong because the dragon
 * is made from a body plus head/neck/tail/wing parts.  The legacy renderer uses the
 * classic part sizes: body 8x8, head 6x6, and 4x4 tail/wing sections.  Build stable
 * local-space component boxes from those dimensions and the entity's current yaw.
 * This intentionally avoids getLatencyPos(): in this binary that method returns
 * cached movement offsets, not world-space XYZ coordinates.
 */
static void hit_add_dragon_boxes(float *v, int *n, void *entity,
                                 const NcAabb6 &main_box, const float *entity_pos,
                                 float yaw) {
    if (!hit_is_dragon(entity) || !entity_pos) return;

    const float pi = 3.14159265358979323846f;
    const float rad = yaw * (pi / 180.0f);
    const float sy = sinf(rad);
    const float cy = cosf(rad);

    /* Same forward convention as the blue facing line: yaw 0 -> -Z. */
    const float fx = -sy, fz = cy;
    const float rx = cy,  rz = sy;

    const float world_w = fmaxf(main_box.maxx - main_box.minx,
                                main_box.maxz - main_box.minz);
    const float scale = clampf(world_w / 16.0f, 0.60f, 1.40f);

    /* Entity::bb is world-space. Convert its center into this entity's local space. */
    const float center_x = ((main_box.minx + main_box.maxx) * 0.5f) - entity_pos[0];
    const float center_y = ((main_box.miny + main_box.maxy) * 0.5f) - entity_pos[1];
    const float center_z = ((main_box.minz + main_box.maxz) * 0.5f) - entity_pos[2];

    const float body_h = 4.0f * scale;
    const float head_h = 3.0f * scale;
    const float part_h = 2.0f * scale;

    /* Main body. */
    hit_box_center(v, n, center_x, center_y, center_z,
                   4.0f * scale, body_h, 4.0f * scale);

    /* Neck + head toward the dragon's facing direction. */
    float x = center_x + fx * (2.0f * scale);
    float z = center_z + fz * (2.0f * scale);
    hit_box_center(v, n, x, center_y + 1.0f * scale, z,
                   2.0f * scale, 2.0f * scale, 2.0f * scale);

    x = center_x + fx * (5.5f * scale);
    z = center_z + fz * (5.5f * scale);
    hit_box_center(v, n, x, center_y + 1.5f * scale, z,
                   head_h, head_h, head_h);

    /* Three tail sections in the opposite direction. */
    const float tail_off[3] = { 4.5f, 7.5f, 10.0f };
    const float tail_half[3] = { 2.0f, 1.8f, 1.6f };
    for (int i = 0; i < 3; ++i) {
        x = center_x - fx * (tail_off[i] * scale);
        z = center_z - fz * (tail_off[i] * scale);
        hit_box_center(v, n, x, center_y, z,
                       tail_half[i] * scale, tail_half[i] * scale,
                       tail_half[i] * scale);
    }

    /* Two wings, one on each side of the body. */
    const float wing_side = 5.0f * scale;
    const float wing_forward = 0.5f * scale;
    x = center_x + rx * wing_side + fx * wing_forward;
    z = center_z + rz * wing_side + fz * wing_forward;
    hit_box_center(v, n, x, center_y + 1.0f * scale, z,
                   part_h, part_h, part_h);

    x = center_x - rx * wing_side + fx * wing_forward;
    z = center_z - rz * wing_side + fz * wing_forward;
    hit_box_center(v, n, x, center_y + 1.0f * scale, z,
                   part_h, part_h, part_h);
}

static void hit_draw_lines(float *verts, int n, const float *mvp) {
    glUniformMatrix4fv(g_hit_mvp, 1, GL_FALSE, mvp);
    glBindBuffer(GL_ARRAY_BUFFER, g_hit_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(float) * n * 3), verts, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * (GLsizei)sizeof(float), (const void *)0);
    glUniform4f(g_hit_col, 1.0f, 1.0f, 1.0f, 1.0f);
    glDrawArrays(GL_LINES, 0, n - 2);
}

static void hit_draw_entity(void *entity, const float *render_pos, float partial) {
    if (!g_cfg.hitbox_on || !entity || !render_pos) return;
    if (hit_seen_entity(entity)) return;
    if (!hit_resolve_symbols() || !hit_init_gl()) return;

    GLint depth_bits = 0;
    glGetIntegerv(GL_DEPTH_BITS, &depth_bits);
    if (depth_bits <= 0) return;

    /* Exact Entity::bb storage in 1.1.5 is at +0x104. */
    const NcAabb6 *aw = (const NcAabb6 *)((const unsigned char *)entity + 0x104);
    NcAabb6 b = *aw;

    /* Keep the AABB in the same local coordinate space used by the entity
     * renderer.  Entity::bb is stored in world coordinates; subtract the
     * entity's current origin, then apply the exact interpolated render_pos
     * supplied to EntityRenderDispatcher::render().  This avoids the old
     * interp/camera-offset cancellation that could produce a second, floating
     * hitbox on moving entities. */
    const float *cp = entity_getPos(entity);
    if (!cp) return;
    b.minx -= cp[0]; b.maxx -= cp[0];
    b.miny -= cp[1]; b.maxy -= cp[1];
    b.minz -= cp[2]; b.maxz -= cp[2];

    NcVec2 rot = {0,0};
    entity_getInterpolatedRotation(&rot, entity, partial);

    float verts[1800];
    int n = 0;
    bool dragon = hit_is_dragon(entity);
    if (dragon) {
        /* The dragon uses its multipart boxes instead of the coarse overall AABB. */
        hit_add_dragon_boxes(verts, &n, entity, *(const NcAabb6 *)aw, cp, rot.y);
    } else {
        hit_aabb_edges(verts, &n, b);      /* 24 vertices */
    }
    int box_vertex_count = n;
    hit_look_line(verts, &n, b, rot.x, rot.y);
    if (n < box_vertex_count + 2) return;

    if (*g_projection_slot == 0 || *g_view_slot == 0) return;
    float proj[16], view[16], model[16], vm[16], mvp[16];
    hit_copy_matrix(*g_projection_slot, proj);
    hit_copy_matrix(*g_view_slot, view);
    hit_translate(model, render_pos[0], render_pos[1], render_pos[2]);
    hit_mul(vm, view, model);
    hit_mul(mvp, proj, vm);

    /* The vertices above are already in camera-relative coordinates. */
    GLint old_prog = 0, old_array = 0, old_active_tex = GL_TEXTURE0, old_tex2d = 0;
    GLint old_depth_func = GL_LEQUAL, old_blend_src_rgb = GL_ONE, old_blend_dst_rgb = GL_ZERO;
    GLint old_blend_src_a = GL_ONE, old_blend_dst_a = GL_ZERO;
    GLint old_cull_face = GL_BACK;
    GLint old_scissor[4] = {0,0,0,0};
    GLint old_viewport[4] = {0,0,0,0};
    GLint old_elem = 0;
    GLboolean old_depth = glIsEnabled(GL_DEPTH_TEST);
    GLboolean old_depth_mask = GL_TRUE;
    GLboolean old_blend = glIsEnabled(GL_BLEND);
    GLboolean old_cull = glIsEnabled(GL_CULL_FACE);
    GLboolean old_scissor_en = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean old_color[4] = {GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE};

    glGetIntegerv(GL_CURRENT_PROGRAM, &old_prog);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &old_array);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &old_elem);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &old_active_tex);
    glGetIntegerv(GL_DEPTH_FUNC, &old_depth_func);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &old_depth_mask);
    glGetIntegerv(GL_BLEND_SRC_RGB, &old_blend_src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &old_blend_dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &old_blend_src_a);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &old_blend_dst_a);
    glGetIntegerv(GL_CULL_FACE, &old_cull_face);
    glGetIntegerv(GL_SCISSOR_BOX, old_scissor);
    glGetIntegerv(GL_VIEWPORT, old_viewport);
    glGetBooleanv(GL_COLOR_WRITEMASK, old_color);

    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex2d);

    GLint old_attr_buf=0, old_attr_size=4, old_attr_type=GL_FLOAT, old_attr_stride=0, old_attr_norm=GL_FALSE;
    GLint old_attr_enabled=0;
    void *old_attr_ptr = 0;
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &old_attr_enabled);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &old_attr_buf);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &old_attr_size);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &old_attr_type);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &old_attr_stride);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &old_attr_norm);
    glGetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &old_attr_ptr);

    glUseProgram(g_hit_prog);
    glUniformMatrix4fv(g_hit_mvp, 1, GL_FALSE, mvp);
    glBindBuffer(GL_ARRAY_BUFFER, g_hit_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(float) * n * 3), verts, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * (GLsizei)sizeof(float), (const void *)0);

    /* Depth test ON, depth writes OFF. No blending: crisp white/red lines. */
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);

    int line_start = n - 2;
    glUniform4f(g_hit_col, 1.0f, 1.0f, 1.0f, 1.0f);
    glDrawArrays(GL_LINES, 0, line_start);
    glUniform4f(g_hit_col, 0.15f, 0.55f, 1.0f, 1.0f);
    glDrawArrays(GL_LINES, line_start, 2);

    /* Restore EVERYTHING we touched. */
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)old_attr_buf);
    if (old_attr_enabled) glEnableVertexAttribArray(0);
    else glDisableVertexAttribArray(0);
    glVertexAttribPointer(0, old_attr_size, (GLenum)old_attr_type,
                          (GLboolean)old_attr_norm, old_attr_stride, old_attr_ptr);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)old_array);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)old_elem);
    glUseProgram((GLuint)old_prog);
    glBindTexture(GL_TEXTURE_2D, (GLuint)old_tex2d);
    glActiveTexture((GLenum)old_active_tex);
    glDepthFunc((GLenum)old_depth_func);
    glDepthMask(old_depth_mask);
    if (old_depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glBlendFuncSeparate((GLenum)old_blend_src_rgb, (GLenum)old_blend_dst_rgb,
                        (GLenum)old_blend_src_a, (GLenum)old_blend_dst_a);
    if (old_blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    glCullFace((GLenum)old_cull_face);
    if (old_cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    glScissor(old_scissor[0], old_scissor[1], old_scissor[2], old_scissor[3]);
    if (old_scissor_en) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    glViewport(old_viewport[0], old_viewport[1], old_viewport[2], old_viewport[3]);
    glColorMask(old_color[0], old_color[1], old_color[2], old_color[3]);
}

static void hook_entity_render(void *self, void *entity, const void *pos, float yaw, float partial) {
    if (g_orig_entity_render)
        g_orig_entity_render(self, entity, pos, yaw, partial);
    if (g_cfg.hitbox_on && entity && pos)
        hit_draw_entity(entity, (const float *)pos, partial);
}

/* ------------------------------------------------------------------ local entity render optimizers
 * These are deliberately render-only: they never change entity state, inventory,
 * placement, packets, or combat input. They only avoid expensive local rendering.
 */
static bool opt_in_range(void *entity, float max_dist) {
    if (!entity || !g_local_player || max_dist <= 0.0f) return true;
    const float *p = entity_getPos(entity);
    if (!p) return true;
    float dx = p[0] - g_snap.x;
    float dy = p[1] - g_snap.y;
    float dz = p[2] - g_snap.z;
    return (dx * dx + dy * dy + dz * dz) <= max_dist * max_dist;
}

static void hook_xp_render(void *self, void *entity, const void *pos, float yaw, float partial) {
    if (g_cfg.xp_opt_on && !opt_in_range(entity, g_cfg.xp_opt_dist)) return;
    if (g_orig_xp_render) g_orig_xp_render(self, entity, pos, yaw, partial);
}

static void hook_crystal_render(void *self, void *entity, const void *pos, float yaw, float partial) {
    if (g_cfg.crystal_opt_on && !opt_in_range(entity, g_cfg.crystal_opt_dist)) return;
    if (g_orig_crystal_render) g_orig_crystal_render(self, entity, pos, yaw, partial);
}

static void hook_crystal_effects(void *self, void *entity, const void *pos, float yaw, float partial) {
    if (g_cfg.crystal_opt_on && g_cfg.crystal_opt_effects) return;
    if (g_orig_crystal_effects) g_orig_crystal_effects(self, entity, pos, yaw, partial);
}

/* FPS optimizer: change what the game's option getters answer */
static bool hook_fancy(void *s)   { if (g_cfg.perf_gfx)    return false; return g_orig_fancy   ? g_orig_fancy(s)   : true; }
static bool hook_skies(void *s)   { if (g_cfg.perf_skies)  return false; return g_orig_skies   ? g_orig_skies(s)   : true; }
static bool hook_light(void *s)   { if (g_cfg.perf_light)  return false; return g_orig_light   ? g_orig_light(s)   : true; }
static bool hook_bobview(void *s) { if (g_cfg.perf_bob)    return false; return g_orig_bobview ? g_orig_bobview(s) : true; }
static int  hook_view(void *s) {
    int v = g_orig_view ? g_orig_view(s) : 8;
    if (g_cfg.perf_view_on && v > g_cfg.perf_view) v = g_cfg.perf_view;
    return v;
}

/* ------------------------------------------------------------------ touch input */
typedef int32_t (*getEvent_fn)(AInputQueue *, AInputEvent **);
static getEvent_fn g_orig_getEvent = 0;

static void push_ev(void *, int type, float x, float y) {
    if (g_qn < 256) { g_q[g_qn].type = type; g_q[g_qn].x = x; g_q[g_qn].y = y; g_qn++; }
}

static int32_t hook_getEvent(AInputQueue *q, AInputEvent **out) {
    for (;;) {
        int32_t r = g_orig_getEvent(q, out);
        if (r < 0 || !out || !*out) return r;
        AInputEvent *ev = *out;
        if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_MOTION) {
            NcMotion m;
            int32_t raw = AMotionEvent_getAction(ev);
            m.action = raw & AMOTION_EVENT_ACTION_MASK;
            m.idx = (raw & 0xff00) >> 8;
            int cnt = (int)AMotionEvent_getPointerCount(ev);
            if (cnt > NC_MAX_PTR) cnt = NC_MAX_PTR;
            m.count = cnt;
            for (int i = 0; i < cnt; i++) {
                m.id[i] = AMotionEvent_getPointerId(ev, (size_t)i);
                m.x[i] = AMotionEvent_getX(ev, (size_t)i);
                m.y[i] = AMotionEvent_getY(ev, (size_t)i);
            }
            pthread_mutex_lock(&g_mu);
            int swallow = nc_touch_event(&g_touch, &m, push_ev, 0);
            pthread_mutex_unlock(&g_mu);
            if (swallow) {
                if (g_touch_logged < 6) { g_touch_logged++; nclog("touch taken: action=%d", m.action); }
                AInputQueue_finishEvent(q, ev, 1);
                continue;
            }
        }
        return r;
    }
}

/* ------------------------------------------------------------------ ImGui setup + theme */
static const ImVec4 ACCENT(0.50f, 0.42f, 1.00f, 1.00f);

static void apply_theme() {
    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowRounding = 10; s.FrameRounding = 6; s.GrabRounding = 6; s.TabRounding = 6;
    s.ScrollbarRounding = 6; s.ChildRounding = 6; s.PopupRounding = 6;
    s.WindowBorderSize = 0; s.FrameBorderSize = 0; s.ChildBorderSize = 0;
    s.WindowPadding = V(12, 10); s.FramePadding = V(10, 6);
    s.ItemSpacing = V(10, 10); s.ItemInnerSpacing = V(8, 6);
    s.ScrollbarSize = 12; s.GrabMinSize = 18;

    const ImVec4 accentDim(0.30f, 0.26f, 0.62f, 1.00f);
    ImVec4 *c = s.Colors;
    c[ImGuiCol_Text]             = ImVec4(0.92f, 0.92f, 0.96f, 1.00f);
    c[ImGuiCol_TextDisabled]     = ImVec4(0.52f, 0.52f, 0.64f, 1.00f);
    c[ImGuiCol_WindowBg]         = ImVec4(0.045f, 0.045f, 0.065f, 0.97f);
    c[ImGuiCol_ChildBg]          = ImVec4(0.075f, 0.075f, 0.105f, 1.00f);
    c[ImGuiCol_FrameBg]          = ImVec4(0.12f, 0.12f, 0.17f, 1.00f);
    c[ImGuiCol_FrameBgHovered]   = ImVec4(0.16f, 0.16f, 0.23f, 1.00f);
    c[ImGuiCol_FrameBgActive]    = ImVec4(0.19f, 0.18f, 0.30f, 1.00f);
    c[ImGuiCol_CheckMark]        = ACCENT;
    c[ImGuiCol_SliderGrab]       = ACCENT;
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.62f, 0.55f, 1.00f, 1.00f);
    c[ImGuiCol_Button]           = accentDim;
    c[ImGuiCol_ButtonHovered]    = ImVec4(0.38f, 0.33f, 0.75f, 1.00f);
    c[ImGuiCol_ButtonActive]     = ACCENT;
    c[ImGuiCol_Header]           = ImVec4(0.24f, 0.21f, 0.52f, 1.00f);
    c[ImGuiCol_HeaderHovered]    = ImVec4(0.30f, 0.26f, 0.62f, 1.00f);
    c[ImGuiCol_HeaderActive]     = ACCENT;
    c[ImGuiCol_Separator]        = ImVec4(0.20f, 0.20f, 0.30f, 1.00f);
    c[ImGuiCol_ScrollbarBg]      = ImVec4(0.03f, 0.03f, 0.05f, 0.60f);
    c[ImGuiCol_ScrollbarGrab]    = accentDim;
}

static void make_icons();

static bool init_imgui(int w, int h) {
    (void)w;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;
    io.LogFilename = NULL;

    g_base = (float)h / 540.0f;
    if (g_base < 1.4f) g_base = 1.4f;

    /* the game's own Minecraft font, baked at 8 px and only ever drawn at whole multiples */
    ImFontConfig fc;
    fc.FontDataOwnedByAtlas = false;
    fc.OversampleH = 1; fc.OversampleV = 1; fc.PixelSnapH = true;
    io.Fonts->AddFontFromMemoryTTF((void *)nc_font_ttf, (int)nc_font_ttf_size, 8.0f, &fc);

    ImGui::StyleColorsDark();
    apply_theme();
    ImGui::GetStyle().ScaleAllSizes(g_base);

    if (!ImGui_ImplOpenGL3_Init("#version 100")) { nclog("ImGui GL backend init FAILED"); return false; }
    make_icons();
    nclog("ImGui ready: screen height %d, layout scale %.2f, %d icons", h, g_base, NC_ICON_COUNT);
    return true;
}

static void make_icons() {
    GLint prev = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    for (int i = 0; i < NC_ICON_COUNT; i++) {
        GLuint t = 0; glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, nc_icon_data[i]);
        g_icon_tex[i] = t;
    }
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
}

static int menu_font_mult(float h) {
    if (g_cfg.ui_font > 0) return g_cfg.ui_font;
    int k = (int)floorf(h / 360.0f + 0.5f);
    return k < 2 ? 2 : (k > 5 ? 5 : k);
}

/* ------------------------------------------------------------------ HUD elements */
enum { E_FPS, E_ARMOR, E_ELYTRA, E_ARROW, E_SPEED, E_COORDS, E_ELYTRA_ANGLE, E_ZOOM, E_PERSP, E_DROP, E_N, E_COUNT };

static float fpx(float size) { return 8.0f * (float)size; }
static ImVec2 txt(const char *s, float size) { return ImGui::GetFont()->CalcTextSizeA(fpx(size), FLT_MAX, 0.0f, s); }
static void put_text(ImDrawList *dl, ImVec2 p, float size, ImU32 col, const char *s) {
    dl->AddText(ImGui::GetFont(), fpx(size), V(floorf(p.x), floorf(p.y)), col, s);
}
static void put_text_sh(ImDrawList *dl, ImVec2 p, float size, ImU32 col, const char *s, bool shadow) {
    if (shadow) put_text(dl, V(p.x + size, p.y + size), size, IM_COL32(0, 0, 0, (int)(((col >> 24) & 0xff) * 0.6f)), s);
    put_text(dl, p, size, col, s);
}
static float icon_k(float s) { int k = (int)(s / 1.5f + 0.5f); return k < 1 ? 1 : k; }   /* whole-number icon scale */
static bool draw_icon(ImDrawList *dl, int idx, ImVec2 p, float side, ImU32 tint) {
    if (idx < 0 || idx >= NC_ICON_COUNT || !g_icon_tex[idx]) return false;
    dl->AddImage((ImTextureID)(uintptr_t)g_icon_tex[idx], p, V(p.x + side, p.y + side), V(0, 0), V(1, 1), tint);
    return true;
}
static void box_col(ImDrawList *dl, ImVec2 p, ImVec2 s, float alpha, float size, int rgb) {
    dl->AddRectFilled(p, vadd(p, s), packed(rgb, alpha), 2.0f * size);
}
static void box(ImDrawList *dl, ImVec2 p, ImVec2 s, float alpha, float size) {
    box_col(dl, p, s, 0.75f * alpha, size, 0x08080E);
}
static float btn_side(float level) { return floorf(g_h * 0.035f * (float)level); }
static ImVec2 place(float fx, float fy, ImVec2 sz) { return V(floorf(fx * (g_w - sz.x)), floorf(fy * (g_h - sz.y))); }

/* ---- FPS ---- */
static ImVec2 size_fps() {
    ImVec2 t = txt("FPS: 000", g_cfg.fps_size);
    float pad = 2.0f * g_cfg.fps_size;
    return V(t.x + 2 * pad, t.y + 2 * pad);
}
static void draw_fps(ImDrawList *dl, ImVec2 p, float fps) {
    char b[24]; snprintf(b, sizeof b, "FPS: %d", (int)(fps + 0.5f));
    ImVec2 s = size_fps(); float pad = 2.0f * g_cfg.fps_size;
    if (g_cfg.fps_bg) box_col(dl, p, s, g_cfg.fps_bg_alpha, g_cfg.fps_size, g_cfg.fps_bg_col);
    put_text(dl, V(p.x + pad, p.y + pad), g_cfg.fps_size, packed(g_cfg.fps_col, g_cfg.fps_alpha), b);
}

/* ---- Armor HUD ---- */
struct ArmorRow { int present, id, dur, max, slot; };

static void armor_style(int id, ImU32 *col, float a) {
    int r = 150, g = 150, b = 165;
    if      (id >= 298 && id <= 301) { r = 160; g = 100; b = 60; }    /* leather */
    else if (id >= 302 && id <= 305) { r = 130; g = 135; b = 150; }   /* chain */
    else if (id >= 306 && id <= 309) { r = 205; g = 205; b = 215; }   /* iron */
    else if (id >= 310 && id <= 313) { r = 70;  g = 215; b = 215; }   /* diamond */
    else if (id >= 314 && id <= 317) { r = 235; g = 200; b = 60; }    /* gold */
    else if (id == 444)              { r = 150; g = 110; b = 230; }   /* elytra */
    *col = rgba(r, g, b, a);
}
static void armor_rows(ArmorRow rows[4], bool preview) {
    static const int sample_id[4] = { 310, 311, 312, 313 };
    static const int sample_dur[4] = { 300, 330, 210, 150 };
    for (int i = 0; i < 4; i++) {
        if (preview) { rows[i].present = 1; rows[i].id = sample_id[i]; rows[i].max = 363; rows[i].dur = sample_dur[i]; }
        else { rows[i].present = g_snap.present[i]; rows[i].id = g_snap.id[i]; rows[i].max = g_snap.max[i]; rows[i].dur = g_snap.dur[i]; }
        rows[i].slot = i;
    }
}
static void armor_metrics(ImVec2 *row, ImVec2 *total) {
    float s = g_cfg.armor_size;
    float icon = 16.0f * icon_k(s), gap = 2.0f * s;
    float tw = g_cfg.armor_num == 1 ? txt("999/999", s).x : (g_cfg.armor_num == 2 ? txt("100%", s).x : 0.0f);
    row->x = icon + (tw > 0 ? gap + tw : 0.0f);
    row->y = icon + (g_cfg.armor_bar ? 3.0f * s : 0.0f);
    float pad = 2.0f * s;
    if (g_cfg.armor_horiz) { total->x = 4 * row->x + 3 * gap + 2 * pad; total->y = row->y + 2 * pad; }
    else                   { total->x = row->x + 2 * pad;              total->y = 4 * row->y + 3 * gap + 2 * pad; }
}
static ImVec2 size_armor() { ImVec2 r, t; armor_metrics(&r, &t); return t; }

static void draw_armor(ImDrawList *dl, ImVec2 p, bool preview) {
    ArmorRow rows[4]; armor_rows(rows, preview);
    ImVec2 rs, total; armor_metrics(&rs, &total);
    float s = g_cfg.armor_size; float a = g_cfg.armor_alpha, pad = 2.0f * s, gap = 2.0f * s, icon = 16.0f * icon_k(s);
    static const char *letters[4] = { "H", "C", "L", "B" };
    if (g_cfg.armor_bg) box_col(dl, p, total, g_cfg.armor_bg_alpha, s, g_cfg.armor_bg_col);
    int shown = 0;
    for (int i = 0; i < 4; i++) {
        if (!rows[i].present) continue;
        ImVec2 o = g_cfg.armor_horiz ? V(pad + shown * (rs.x + gap), pad) : V(pad, pad + shown * (rs.y + gap));
        ImVec2 q = vadd(p, o);
        int id = rows[i].id, ix = (id >= 298 && id <= 317) ? id - 298 : (id == 444 ? NC_ICON_ELYTRA : -1);
        ImU32 tint = (ix >= 0 && ix <= 3) ? rgba(160, 101, 64, a) : rgba(255, 255, 255, a);     /* leather is tinted brown, like in the game */
        if (!draw_icon(dl, ix, q, icon, tint)) {                                                  /* unknown item: coloured square */
            ImU32 col; armor_style(id, &col, a);
            dl->AddRectFilled(q, V(q.x + icon, q.y + icon), col, 1.5f * s);
            put_text(dl, V(q.x + (icon - fpx(s)) * 0.5f, q.y + (icon - fpx(s)) * 0.5f), s, rgba(10, 10, 16, a), letters[rows[i].slot]);
        }
        float frac = rows[i].max > 0 ? clampf((float)rows[i].dur / (float)rows[i].max, 0.0f, 1.0f) : 1.0f;
        if (g_cfg.armor_num) {
            char b[16];
            if (g_cfg.armor_num == 1) snprintf(b, sizeof b, "%d/%d", rows[i].dur, rows[i].max);
            else                      snprintf(b, sizeof b, "%d%%", (int)(frac * 100.0f + 0.5f));
            put_text_sh(dl, V(q.x + icon + gap, q.y + (icon - fpx(s)) * 0.5f), s, packed(g_cfg.armor_col, a), b, !g_cfg.armor_bg);
        }
        if (g_cfg.armor_bar) {
            float by = q.y + icon + s;
            dl->AddRectFilled(V(q.x, by), V(q.x + rs.x, by + 2.0f * s), rgba(40, 40, 55, g_cfg.armor_bg_alpha));
            int rr, gg;                                   /* green -> yellow -> red as it wears out */
            if (frac > 0.5f) { float t = (frac - 0.5f) * 2.0f; rr = (int)(235.0f - 145.0f * t); gg = (int)(215.0f + 15.0f * t); }
            else             { float t = frac * 2.0f;          rr = 235;                        gg = (int)(70.0f + 145.0f * t); }
            dl->AddRectFilled(V(q.x, by), V(q.x + rs.x * frac, by + 2.0f * s), rgba(rr, gg, 60, a));
        }
        shown++;
    }
}

/* ---- Elytra indicator (outline icon, text, or both) ---- */
static ImVec2 size_elytra() {
    float s = g_cfg.elytra_size; float pad = 2.0f * s, icon = 16.0f * icon_k(s);
    ImVec2 t = txt("ELYTRA", s);
    if (g_cfg.elytra_style == 0) return V(icon + 2 * pad, icon + 2 * pad);
    if (g_cfg.elytra_style == 1) return V(t.x + 2 * pad, t.y + 2 * pad);
    return V(icon + 2 * s + t.x + 2 * pad, (icon > t.y ? icon : t.y) + 2 * pad);
}
static void draw_elytra(ImDrawList *dl, ImVec2 p) {
    float s = g_cfg.elytra_size; float a = g_cfg.elytra_alpha, pad = 2.0f * s, icon = 16.0f * icon_k(s);
    ImVec2 sz = size_elytra();
    ImU32 col = packed(g_cfg.elytra_col, a);
    if (g_cfg.elytra_style != 0) box_col(dl, p, sz, g_cfg.elytra_bg_alpha, s, g_cfg.elytra_bg_col);
    float tx = p.x + pad;
    if (g_cfg.elytra_style != 1) {
        draw_icon(dl, NC_ICON_ELYTRA_OUTLINE, V(p.x + pad, p.y + (sz.y - icon) * 0.5f), icon, rgba((g_cfg.elytra_col >> 16) & 0xff, (g_cfg.elytra_col >> 8) & 0xff, g_cfg.elytra_col & 0xff, a));
        tx += icon + 2.0f * s;
    }
    if (g_cfg.elytra_style != 0) put_text(dl, V(tx, p.y + (sz.y - fpx(s)) * 0.5f), s, col, "ELYTRA");
}

/* ---- Arrow HUD: bow icon + total arrows in your inventory ---- */
static ImVec2 size_arrow() {
    float s = g_cfg.arrow_size; float pad = 2.0f * s, icon = 16.0f * icon_k(s);
    ImVec2 t = txt("999", s);
    return V(icon + 2.0f * s + t.x + 2 * pad, (icon > t.y ? icon : t.y) + 2 * pad);
}
static void draw_arrow(ImDrawList *dl, ImVec2 p, int count) {
    float s = g_cfg.arrow_size; float a = g_cfg.arrow_alpha, pad = 2.0f * s, icon = 16.0f * icon_k(s);
    ImVec2 sz = size_arrow();
    if (g_cfg.arrow_bg) box_col(dl, p, sz, g_cfg.arrow_bg_alpha, s, g_cfg.arrow_bg_col);
    draw_icon(dl, NC_ICON_ARROW, V(p.x + pad, p.y + (sz.y - icon) * 0.5f), icon, rgba(255, 255, 255, a));
    char b[8];
    if (count < 0) snprintf(b, sizeof b, "-");
    else           snprintf(b, sizeof b, "%d", count);
    put_text_sh(dl, V(p.x + pad + icon + 2.0f * s, p.y + (sz.y - fpx(s)) * 0.5f), s, packed(g_cfg.arrow_col, a), b, !g_cfg.arrow_bg);
}


/* ---- Coordinates HUD ---- */
static ImVec2 size_coords() {
    int dec = g_cfg.coords_precision - 1;
    char b[96];
    snprintf(b, sizeof b, "%.*f %.*f %.*f", dec, g_snap.x, dec, g_snap.y, dec, g_snap.z);
    float s = g_cfg.coords_size, pad = 2.0f * s;
    ImVec2 t = txt(b, s);
    return V(t.x + 2.0f * pad, t.y + 2.0f * pad);
}
static void draw_coords(ImDrawList *dl, ImVec2 p) {
    int dec = g_cfg.coords_precision - 1;
    char b[96];
    snprintf(b, sizeof b, "%.*f %.*f %.*f", dec, g_snap.x, dec, g_snap.y, dec, g_snap.z);
    float s = g_cfg.coords_size, pad = 2.0f * s;
    ImVec2 sz = size_coords();
    if (g_cfg.coords_bg) box_col(dl, p, sz, g_cfg.coords_bg_alpha, s, g_cfg.coords_bg_col);
    put_text_sh(dl, V(p.x + pad, p.y + pad), s, packed(g_cfg.coords_col, g_cfg.coords_alpha), b, !g_cfg.coords_bg);
}

/* ---- Elytra angle: actual Entity rotation X (pitch) from the 1.1.5 game object ---- */
static ImVec2 size_elytra_angle() {
    float s = g_cfg.elytra_angle_size; float pad = 2.0f * s;
    ImVec2 t = txt("-90", s);
    return V(t.x + 2.0f * pad + s * 1.1f, t.y + 2.0f * pad);
}
static void draw_elytra_angle(ImDrawList *dl, ImVec2 p) {
    float s = g_cfg.elytra_angle_size; float a = g_cfg.elytra_angle_alpha, pad = 2.0f * s;
    ImVec2 sz = size_elytra_angle();
    if (g_cfg.elytra_angle_bg) box_col(dl, p, sz, g_cfg.elytra_angle_bg_alpha, s, g_cfg.elytra_angle_bg_col);

    char b[32]; snprintf(b, sizeof b, "%.0f", g_snap.elytra_angle);
    ImVec2 tp = V(p.x + pad, p.y + pad);
    put_text_sh(dl, tp, s, packed(g_cfg.elytra_angle_col, a), b, !g_cfg.elytra_angle_bg);

    /* nc_font is ASCII-only, so drawing UTF-8 '°' would become '?'.  Draw the
     * degree mark as a tiny circle instead; visually it is the same glyph and
     * works on every device/font shipped with Night Client 1.1.5. */
    ImVec2 tw = txt(b, s);
    float r = fpx(s) * 0.11f;
    ImVec2 c = V(tp.x + tw.x + r * 1.45f, tp.y + r * 0.95f);
    dl->AddCircle(c, r, packed(g_cfg.elytra_angle_col, a), 12, fmaxf(1.0f, s * 0.22f));
}

/* ---- Speed HUD: horizontal blocks per real second ---- */
static ImVec2 size_speed() {
    float z = g_cfg.speed_size; float pad = 2.0f * z;
    ImVec2 t = txt("Speed: 99.99 B/s", z);
    return V(t.x + 2.0f * pad, t.y + 2.0f * pad);
}
static void draw_speed(ImDrawList *dl, ImVec2 p, float bps) {
    float z = g_cfg.speed_size; float a = g_cfg.speed_alpha, pad = 2.0f * z;
    ImVec2 sz = size_speed();
    if (g_cfg.speed_bg) box_col(dl, p, sz, g_cfg.speed_bg_alpha, z, g_cfg.speed_bg_col);
    char b[32]; snprintf(b, sizeof b, "Speed: %.2f B/s", bps);
    put_text_sh(dl, V(p.x + pad, p.y + pad), z, packed(g_cfg.speed_col, a), b, !g_cfg.speed_bg);
}

/* ---- round/square buttons (N, Zoom, Perspective) with preset labels ---- */
static const char *const LBL_ZOOM[]  = { "Z", "ZOOM", "+", "Q", "O" };
static const char *const LBL_PERSP[] = { "F5", "P", "CAM", "3P", "V" };
static const char *const LBL_N[]     = { "N", "NC", "NIGHT" };
static const char *const LBL_DROP[]  = { "Q", "DROP", "V" };
#define NC_COUNT_OF(a) ((int)(sizeof(a) / sizeof((a)[0])))
static const char *pick(const char *const *list, int n, int idx) { return list[(idx < 0 || idx >= n) ? 0 : idx]; }
static const char *button_text(int e) {
    switch (e) {
        case E_ZOOM: return g_cfg.zoom_text;
        case E_PERSP: return g_cfg.persp_text;
        case E_DROP: return g_cfg.drop_text;
        default: return g_cfg.n_text;
    }
}

static int label_level(float side) { int lv = (int)floorf(side / 8.0f * 0.5f); return lv < 1 ? 1 : lv; }
static ImVec2 btn_size(const char *label, float level) {
    float side = btn_side(level), w = side;
    if (strlen(label) > 1) { float need = txt(label, label_level(side)).x + side * 0.6f; if (need > w) w = need; }
    return V(floorf(w), side);
}
static void draw_button(ImDrawList *dl, ImVec2 p, ImVec2 sz, const char *label, float bg_alpha, float text_alpha, bool round, bool active, int bg_col, int text_col) {
    ImU32 bg = active ? rgba((int)clampf(((bg_col >> 16) & 0xff) * 1.5f, 0, 255), (int)clampf(((bg_col >> 8) & 0xff) * 1.5f, 0, 255),
                             (int)clampf((bg_col & 0xff) * 1.5f, 0, 255), bg_alpha)
                      : packed(bg_col, bg_alpha);
    ImVec2 c = V(p.x + sz.x * 0.5f, p.y + sz.y * 0.5f);
    if (round && strlen(label) == 1) dl->AddCircleFilled(c, sz.y * 0.5f, bg);
    else                             dl->AddRectFilled(p, V(p.x + sz.x, p.y + sz.y), bg, sz.y * 0.18f);
    int lv = label_level(sz.y);
    ImVec2 t = txt(label, lv);
    put_text(dl, V(c.x - t.x * 0.5f, c.y - t.y * 0.5f), lv, packed(text_col, text_alpha), label);
}

/* draws one button in its own click window; returns true when it was tapped */
static bool button_at(const char *id, ImVec2 p, ImVec2 sz, const char *label, float bg_alpha, float text_alpha, bool round,
                      bool active_look, int base_col, int text_col, NcRect *rect) {
    ImGui::SetNextWindowPos(p);
    ImGui::SetNextWindowSize(sz);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, V(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::Begin(id, NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground);
    ImGui::SetCursorScreenPos(p);
    bool pressed = ImGui::InvisibleButton("##b", sz);
    bool active = ImGui::IsItemActive();
    draw_button(ImGui::GetWindowDrawList(), p, sz, label, bg_alpha, text_alpha, round, active || active_look, base_col, text_col);
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    rect->visible = 1; rect->x = p.x; rect->y = p.y; rect->w = sz.x; rect->h = sz.y;
    return pressed;
}

/* ---- per-element accessors used by "Move on screen" ---- */
static int *elem_on(int e) {
    switch (e) { case E_FPS: return &g_cfg.fps_on; case E_ARMOR: return &g_cfg.armor_on; case E_ELYTRA: return &g_cfg.elytra_on;
                 case E_ARROW: return &g_cfg.arrow_on; case E_SPEED: return &g_cfg.speed_on; case E_COORDS: return &g_cfg.coords_on; case E_ELYTRA_ANGLE: return &g_cfg.elytra_angle_on; case E_ZOOM: return &g_cfg.zoom_on; case E_PERSP: return &g_cfg.persp_on;
                 case E_DROP: return &g_cfg.drop_on; default: return 0; }
}
static float *elem_x(int e) {
    switch (e) { case E_FPS: return &g_cfg.fps_x; case E_ARMOR: return &g_cfg.armor_x; case E_ELYTRA: return &g_cfg.elytra_x;
                 case E_ARROW: return &g_cfg.arrow_x; case E_SPEED: return &g_cfg.speed_x; case E_COORDS: return &g_cfg.coords_x; case E_ELYTRA_ANGLE: return &g_cfg.elytra_angle_x; case E_ZOOM: return &g_cfg.zoom_x; case E_PERSP: return &g_cfg.persp_x;
                 case E_DROP: return &g_cfg.drop_x; default: return &g_cfg.n_x; }
}
static float *elem_y(int e) {
    switch (e) { case E_FPS: return &g_cfg.fps_y; case E_ARMOR: return &g_cfg.armor_y; case E_ELYTRA: return &g_cfg.elytra_y;
                 case E_ARROW: return &g_cfg.arrow_y; case E_SPEED: return &g_cfg.speed_y; case E_COORDS: return &g_cfg.coords_y; case E_ELYTRA_ANGLE: return &g_cfg.elytra_angle_y; case E_ZOOM: return &g_cfg.zoom_y; case E_PERSP: return &g_cfg.persp_y;
                 case E_DROP: return &g_cfg.drop_y; default: return &g_cfg.n_y; }
}
static ImVec2 elem_size(int e) {
    switch (e) {
        case E_FPS: return size_fps();
        case E_ARMOR: return size_armor();
        case E_ELYTRA: return size_elytra();
        case E_ARROW: return size_arrow();
        case E_SPEED: return size_speed();
        case E_COORDS: return size_coords();
        case E_ELYTRA_ANGLE: return size_elytra_angle();
        case E_DROP:  return btn_size(g_cfg.drop_text,  g_cfg.drop_btn);
        case E_ZOOM:  return btn_size(g_cfg.zoom_text,  g_cfg.zoom_btn);
        case E_PERSP: return btn_size(g_cfg.persp_text, g_cfg.persp_btn);
        default:      return btn_size(g_cfg.n_text,     g_cfg.n_btn);
    }
}

/* ------------------------------------------------------------------ "Move on screen" */
static void build_edit(float w, float h) {
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(V(0, 0));
    ImGui::SetNextWindowSize(V(w, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, V(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::Begin("##night_edit", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(V(0, 0), V(w, h), IM_COL32(0, 0, 0, 120));

    static const char *ids[E_COUNT] = { "fps", "armor", "elytra", "arrow", "speed", "coords", "elytra_angle", "zoom", "persp", "drop", "n" };

    for (int e = 0; e < E_COUNT; e++) {
        int *on = elem_on(e);
        if (on && !*on) continue;
        ImVec2 sz = elem_size(e), pos = place(*elem_x(e), *elem_y(e), sz);
        switch (e) {
            case E_FPS:    draw_fps(dl, pos, 60.0f); break;
            case E_ARMOR:  draw_armor(dl, pos, true); break;
            case E_ELYTRA: draw_elytra(dl, pos); break;
            case E_ARROW:  draw_arrow(dl, pos, g_snap.arrow_count < 0 ? -1 : g_snap.arrow_count); break;
            case E_SPEED:  draw_speed(dl, pos, 4.20f); break;
            case E_ELYTRA_ANGLE: draw_elytra_angle(dl, pos); break;
            case E_DROP:   draw_button(dl, pos, sz, g_cfg.drop_text,  g_cfg.drop_alpha, g_cfg.drop_text_alpha, false, false, g_cfg.drop_bg_col, g_cfg.drop_col); break;
            case E_ZOOM:   draw_button(dl, pos, sz, g_cfg.zoom_text,  g_cfg.zoom_alpha, g_cfg.zoom_text_alpha, false, false, g_cfg.zoom_bg_col, g_cfg.zoom_col); break;
            case E_PERSP:  draw_button(dl, pos, sz, g_cfg.persp_text, g_cfg.persp_alpha, g_cfg.persp_text_alpha, false, false, g_cfg.persp_bg_col, g_cfg.persp_col); break;
            default:       draw_button(dl, pos, sz, g_cfg.n_text,     g_cfg.n_alpha, g_cfg.n_text_alpha, true,  false, g_cfg.n_bg_col, g_cfg.n_col); break;
        }
        dl->AddRect(pos, vadd(pos, sz), IM_COL32(150, 130, 255, 255), 3.0f, 0, 2.0f);
        ImGui::SetCursorScreenPos(pos);
        ImGui::InvisibleButton(ids[e], sz);
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 0.0f)) {
            ImVec2 np = V(pos.x + io.MouseDelta.x, pos.y + io.MouseDelta.y);
            float rx = w - sz.x, ry = h - sz.y;
            if (rx > 1) *elem_x(e) = clampf(np.x / rx, 0.0f, 1.0f);
            if (ry > 1) *elem_y(e) = clampf(np.y / ry, 0.0f, 1.0f);
        }
    }

    const char *hint = "Drag anything to move it";
    ImVec2 ht = txt(hint, 3);
    put_text(dl, V((w - ht.x) * 0.5f, h * 0.06f), 3, IM_COL32(235, 235, 245, 255), hint);
    float bw = fpx(3) * 5.0f, bh = fpx(3) * 2.4f;
    ImGui::SetCursorScreenPos(V((w - bw) * 0.5f, h * 0.06f + fpx(3) * 1.8f));
    if (ImGui::Button("Done", V(bw, bh))) { g_edit = false; g_menu_open = true; kb_stop(); nclog("layout saved"); }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

/* ------------------------------------------------------------------ menu */
static void touch_scroll() {
    ImGuiIO &io = ImGui::GetIO();
    if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemActive() && ImGui::IsMouseDragging(0))
        ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
}
static void chk(const char *label, int *v) { bool b = *v != 0; if (ImGui::Checkbox(label, &b)) *v = b ? 1 : 0; }
static void sl_f(const char *label, float *v, float lo, float hi) { ImGui::SliderFloat(label, v, lo, hi, "%.2f"); }
static void sl_i(const char *label, int *v, int lo, int hi) { ImGui::SliderInt(label, v, lo, hi, "%d"); }
static void move_btn() {
    if (ImGui::Button("Move on screen")) { g_edit = true; g_menu_open = false; nclog("edit mode"); }
}
static void head(const char *title, int *on, const char *desc) {
    ImGui::TextColored(ACCENT, "%s", title);
    if (on) chk("Enabled", on);
    if (desc) ImGui::TextDisabled("%s", desc);
    ImGui::Separator();
}
static void button_text_edit(const char *label, char *text) {
    char id[48];
    snprintf(id, sizeof id, "%s##btntext", label);
    ImGui::InputText(id, text, 17, ImGuiInputTextFlags_CharsNoBlank);
    if (ImGui::IsItemActivated()) {
        int target = 0;
        if (!strcmp(label, "Button text")) {
            if (text == g_cfg.zoom_text) target = 1;
            else if (text == g_cfg.persp_text) target = 2;
            else if (text == g_cfg.drop_text) target = 3;
        } else if (!strcmp(label, "N button text")) target = 4;
        if (target && !g_kb_open) kb_start(target, text);
    }
}
static void hud_look(float *size, float *alpha, float *bg_alpha) {
    sl_f("Size", size, 0.5f, 12.0f);
    sl_f("Text / icon opacity", alpha, 0.05f, 1.0f);
    sl_f("Background opacity", bg_alpha, 0.0f, 1.0f);
}
static void hud_pos(float *x, float *y) {
    move_btn();
    ImGui::TextDisabled("Fine tune");
    sl_f("X", x, 0.0f, 1.0f);
    sl_f("Y", y, 0.0f, 1.0f);
}

static void panel_autosprint() {
    head("Autosprint", &g_cfg.autosprint, "Sprint automatically while you move forward.");
}
static void panel_fps() {
    head("FPS counter", &g_cfg.fps_on, "Shows your frame rate.");
    chk("Also show in menus (outside a world)", &g_cfg.fps_menus);
    chk("Dark background", &g_cfg.fps_bg);
    color_picker("Text color", &g_cfg.fps_col);
    color_picker("Background color", &g_cfg.fps_bg_col);
    hud_look(&g_cfg.fps_size, &g_cfg.fps_alpha, &g_cfg.fps_bg_alpha);
    hud_pos(&g_cfg.fps_x, &g_cfg.fps_y);
}
static void panel_armor() {
    head("Armor HUD", &g_cfg.armor_on, "Your armor with its real icons and durability. Only shows in a world.");
    chk("Bar", &g_cfg.armor_bar);
    chk("Dark background", &g_cfg.armor_bg);
    chk("Lay out sideways", &g_cfg.armor_horiz);
    color_picker("Text color", &g_cfg.armor_col);
    if (ImGui::RadioButton("No text", g_cfg.armor_num == 0)) g_cfg.armor_num = 0;
    ImGui::SameLine();
    if (ImGui::RadioButton("Durability", g_cfg.armor_num == 1)) g_cfg.armor_num = 1;
    ImGui::SameLine();
    if (ImGui::RadioButton("Percent", g_cfg.armor_num == 2)) g_cfg.armor_num = 2;
    color_picker("Background color", &g_cfg.armor_bg_col);
    hud_look(&g_cfg.armor_size, &g_cfg.armor_alpha, &g_cfg.armor_bg_alpha);
    hud_pos(&g_cfg.armor_x, &g_cfg.armor_y);
}
static void panel_elytra() {
    head("Elytra indicator", &g_cfg.elytra_on, "An outline elytra icon on screen while you are gliding.");
    if (ImGui::RadioButton("Icon", g_cfg.elytra_style == 0)) g_cfg.elytra_style = 0;
    ImGui::SameLine();
    if (ImGui::RadioButton("Text", g_cfg.elytra_style == 1)) g_cfg.elytra_style = 1;
    ImGui::SameLine();
    if (ImGui::RadioButton("Both", g_cfg.elytra_style == 2)) g_cfg.elytra_style = 2;
    color_picker("Text / icon color", &g_cfg.elytra_col);
    color_picker("Background color", &g_cfg.elytra_bg_col);
    hud_look(&g_cfg.elytra_size, &g_cfg.elytra_alpha, &g_cfg.elytra_bg_alpha);
    hud_pos(&g_cfg.elytra_x, &g_cfg.elytra_y);
}
static void panel_arrow() {
    head("Arrow HUD", &g_cfg.arrow_on, "Shows while you hold a bow, with the total arrows in your inventory.");
    chk("Dark background", &g_cfg.arrow_bg);
    color_picker("Number color", &g_cfg.arrow_col);
    color_picker("Background color", &g_cfg.arrow_bg_col);
    hud_look(&g_cfg.arrow_size, &g_cfg.arrow_alpha, &g_cfg.arrow_bg_alpha);
    hud_pos(&g_cfg.arrow_x, &g_cfg.arrow_y);
}
static void panel_speed() {
    head("Speed indicator", &g_cfg.speed_on, "Shows your real horizontal movement speed in blocks per second.");
    chk("Dark background", &g_cfg.speed_bg);
    color_picker("Text color", &g_cfg.speed_col);
    color_picker("Background color", &g_cfg.speed_bg_col);
    hud_look(&g_cfg.speed_size, &g_cfg.speed_alpha, &g_cfg.speed_bg_alpha);
    hud_pos(&g_cfg.speed_x, &g_cfg.speed_y);
}
static void panel_xp_optimizer() {
    head("XP optimizer", &g_cfg.xp_opt_on, "Local-only XP orb rendering optimization. It does not change XP pickup or gameplay.");
    sl_f("Render distance", &g_cfg.xp_opt_dist, 4.0f, 64.0f);
    ImGui::TextDisabled("XP orbs farther than this are skipped by the local renderer.");
}
static void panel_crystal_optimizer() {
    head("Crystal optimizer", &g_cfg.crystal_opt_on, "Local-only End Crystal rendering optimization. It does not place, aim, or attack.");
    sl_f("Render distance", &g_cfg.crystal_opt_dist, 4.0f, 64.0f);
    chk("Disable crystal beam/effects", &g_cfg.crystal_opt_effects);
    ImGui::TextDisabled("Only the local crystal renderer is changed for performance.");
}
static void panel_coords() {
    head("Coordinates", &g_cfg.coords_on, "Shows your XYZ position in the HUD.");
    chk("Dark background", &g_cfg.coords_bg);
    color_picker("Text color", &g_cfg.coords_col);
    color_picker("Background color", &g_cfg.coords_bg_col);
    hud_look(&g_cfg.coords_size, &g_cfg.coords_alpha, &g_cfg.coords_bg_alpha);
    sl_i("Precision", &g_cfg.coords_precision, 1, 4);
    hud_pos(&g_cfg.coords_x, &g_cfg.coords_y);
}
static void panel_elytra_angle() {
    head("Elytra angle", &g_cfg.elytra_angle_on, "Shows your actual flight pitch while gliding.");
    chk("Dark background", &g_cfg.elytra_angle_bg);
    color_picker("Text color", &g_cfg.elytra_angle_col);
    color_picker("Background color", &g_cfg.elytra_angle_bg_col);
    hud_look(&g_cfg.elytra_angle_size, &g_cfg.elytra_angle_alpha, &g_cfg.elytra_angle_bg_alpha);
    hud_pos(&g_cfg.elytra_angle_x, &g_cfg.elytra_angle_y);
}
static void panel_nohurt() {
    head("No hurt cam", &g_cfg.nohurt, "Stops the screen from tilting when you take damage.");
}
static void panel_zoom() {
    head("Zoom", &g_cfg.zoom_on, "A Z button in the world. Tap it to zoom in, tap again to zoom out.");
    sl_f("Zoom level", &g_cfg.zoom_level, 1.5f, 12.0f);
    button_text_edit("Button text", g_cfg.zoom_text);
    color_picker("Text color", &g_cfg.zoom_col);
    color_picker("Background color", &g_cfg.zoom_bg_col);
    chk("Also show on the pause screen", &g_cfg.zoom_pause);
    sl_f("Button size", &g_cfg.zoom_btn, 0.5f, 12.0f);
    sl_f("Background opacity", &g_cfg.zoom_alpha, 0.0f, 1.0f);
    sl_f("Text opacity", &g_cfg.zoom_text_alpha, 0.0f, 1.0f);
    hud_pos(&g_cfg.zoom_x, &g_cfg.zoom_y);
}
static void panel_persp() {
    head("Perspective button", &g_cfg.persp_on, "A button in the world that switches first/third person.");
    button_text_edit("Button text", g_cfg.persp_text);
    color_picker("Text color", &g_cfg.persp_col);
    color_picker("Background color", &g_cfg.persp_bg_col);
    chk("Also show on the pause screen", &g_cfg.persp_pause);
    sl_f("Button size", &g_cfg.persp_btn, 0.5f, 12.0f);
    sl_f("Background opacity", &g_cfg.persp_alpha, 0.0f, 1.0f);
    sl_f("Text opacity", &g_cfg.persp_text_alpha, 0.0f, 1.0f);
    hud_pos(&g_cfg.persp_x, &g_cfg.persp_y);
    ImGui::TextDisabled("Works after you have touched the screen in a world once.");
}
static void panel_hitbox() {
    head("Hitboxes", &g_cfg.hitbox_on, "Turns on the game's own developer bounding-box renderer.");
    ImGui::TextDisabled("This shows every entity's and block's box, drawn by the game itself, so it renders");
    ImGui::TextDisabled("correctly through everything the game already handles (distance, walls, etc).");
    ImGui::TextDisabled("There is no separate colour for a thrown ender pearl yet - tell me what it looks");
    ImGui::TextDisabled("like once you can see it and I will try to single it out next.");
}
static void panel_perf() {
    head("FPS optimizer", 0, "Lower some graphics settings for more FPS. Each one is separate.");
    chk("Fast graphics (opaque leaves)", &g_cfg.perf_gfx);
    chk("Flat lighting", &g_cfg.perf_light);
    chk("No fancy skies", &g_cfg.perf_skies);
    chk("No view bobbing", &g_cfg.perf_bob);
    chk("Limit view distance", &g_cfg.perf_view_on);
    if (g_cfg.perf_view_on) sl_i("Max chunks", &g_cfg.perf_view, 2, 16);
    ImGui::TextDisabled("Some changes show after a moment or when you re-enter the world.");
}
static void panel_drop() {
    head("Quick drop", &g_cfg.drop_on, "A button that drops the item you're holding, one tap.");
    button_text_edit("Button text", g_cfg.drop_text);
    color_picker("Text color", &g_cfg.drop_col);
    color_picker("Background color", &g_cfg.drop_bg_col);
    chk("Also show on the pause screen", &g_cfg.drop_pause);
    sl_f("Button size", &g_cfg.drop_btn, 0.5f, 12.0f);
    sl_f("Background opacity", &g_cfg.drop_alpha, 0.0f, 1.0f);
    sl_f("Text opacity", &g_cfg.drop_text_alpha, 0.0f, 1.0f);
    hud_pos(&g_cfg.drop_x, &g_cfg.drop_y);
    ImGui::TextDisabled("Works after you have touched the screen in a world once.");
}
static void panel_client() {
    head("Client", 0, "Menu and N button.");
    sl_i("Menu text size (0 = auto)", &g_cfg.ui_font, 0, 6);
    button_text_edit("N button text", g_cfg.n_text);
    color_picker("Text color", &g_cfg.n_col);
    color_picker("Background color", &g_cfg.n_bg_col);
    sl_f("N button size", &g_cfg.n_btn, 0.5f, 12.0f);
    sl_f("Background opacity", &g_cfg.n_alpha, 0.0f, 1.0f);
    sl_f("Text opacity", &g_cfg.n_text_alpha, 0.0f, 1.0f);
    ImGui::TextDisabled("The N button shows on the Settings screen and the pause menu.");
    move_btn();
    ImGui::TextDisabled("N button position");
    sl_f("N X", &g_cfg.n_x, 0.0f, 1.0f);
    sl_f("N Y", &g_cfg.n_y, 0.0f, 1.0f);
    chk("Show N button everywhere (debug)", &g_cfg.n_always);
    ImGui::Spacing();
    ImGui::TextDisabled("Night Client " NC_VERSION);
    ImGui::TextDisabled("Settings: games/com.mojang/NightClient/config.txt");
}

struct Mod { const char *name; int *on; void (*panel)(); };
static const Mod g_mods[] = {
    { "Autosprint",         &g_cfg.autosprint, panel_autosprint },
    { "FPS counter",        &g_cfg.fps_on,     panel_fps },
    { "Armor HUD",          &g_cfg.armor_on,   panel_armor },
    { "Elytra indicator",   &g_cfg.elytra_on,  panel_elytra },
    { "Arrow HUD",          &g_cfg.arrow_on,   panel_arrow },
    { "Speed indicator",    &g_cfg.speed_on,   panel_speed },
    { "Coordinates",        &g_cfg.coords_on,  panel_coords },
    { "XP optimizer",        &g_cfg.xp_opt_on, panel_xp_optimizer },
    { "Crystal optimizer",   &g_cfg.crystal_opt_on, panel_crystal_optimizer },
    { "Elytra angle",       &g_cfg.elytra_angle_on, panel_elytra_angle },
    { "Quick drop",         &g_cfg.drop_on,    panel_drop },
    { "No hurt cam",        &g_cfg.nohurt,     panel_nohurt },
    { "Zoom",               &g_cfg.zoom_on,    panel_zoom },
    { "Perspective button", &g_cfg.persp_on,   panel_persp },
    { "Hitboxes",           &g_cfg.hitbox_on,  panel_hitbox },
    { "FPS optimizer",      0,                 panel_perf },
    { "Client",             0,                 panel_client },
};
#define NC_NMODS ((int)(sizeof(g_mods) / sizeof(g_mods[0])))

static void build_menu(float w, float h, NcRect *win_rect) {
    ImGui::SetNextWindowPos(V(w * 0.5f, h * 0.5f), ImGuiCond_Always, V(0.5f, 0.5f));
    ImGui::SetNextWindowSize(V(w * 0.84f, h * 0.88f), ImGuiCond_Always);
    ImGui::Begin("##night_menu", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
    win_rect->visible = 1; win_rect->x = p.x; win_rect->y = p.y; win_rect->w = s.x; win_rect->h = s.y;

    /* header: title + X (top right) */
    ImGui::TextColored(ACCENT, "NIGHT CLIENT");
    ImGui::SameLine();
    ImGui::TextDisabled("%s  MCPE 1.1.5", NC_VERSION);
    float xw = ImGui::GetFrameHeight() * 1.5f;
    ImGui::SameLine(ImGui::GetWindowWidth() - xw - ImGui::GetStyle().WindowPadding.x);
    if (ImGui::Button("X", V(xw, 0))) { g_menu_open = false; nclog("menu closed"); }
    ImGui::Separator();

    /* left: mod list, right: config of the selected mod (like your sketch) */
    ImVec2 body = ImGui::GetContentRegionAvail();
    float side_w = body.x * 0.30f;
    ImGui::BeginChild("##side", V(side_w, body.y), false, 0);
    touch_scroll();
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, V(0.0f, 0.5f));
    float rowh = ImGui::GetFrameHeight() * 1.7f;
    for (int i = 0; i < NC_NMODS; i++) {
        ImVec2 rp = ImGui::GetCursorScreenPos();
        float rw = ImGui::GetContentRegionAvail().x;
        if (ImGui::Selectable(g_mods[i].name, g_sel == i, 0, V(rw, rowh))) g_sel = i;
        if (g_mods[i].on) {
            float r = rowh * 0.14f;
            ImGui::GetWindowDrawList()->AddCircleFilled(V(rp.x + rw - r * 2.5f, rp.y + rowh * 0.5f), r,
                                                        *g_mods[i].on ? IM_COL32(90, 230, 130, 255) : IM_COL32(90, 90, 110, 255));
        }
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##panel", V(0, body.y), false, 0);
    touch_scroll();
    if (g_sel < 0 || g_sel >= NC_NMODS) g_sel = 0;
    g_mods[g_sel].panel();
    ImGui::EndChild();
    ImGui::End();
}

/* ------------------------------------------------------------------ per-frame entry (eglSwapBuffers) */
static void do_perspective() {
    if (g_cic && g_ci) cic_toggle3rd(g_cic, g_ci);
    else nclog("perspective: game objects not captured yet (touch the screen in a world first)");
}

static void nc_frame(EGLDisplay d, EGLSurface s) {
    if (g_imgui_failed) return;
    kb_poll();

    EGLint w = 0, h = 0;
    eglQuerySurface(d, s, EGL_WIDTH, &w);
    eglQuerySurface(d, s, EGL_HEIGHT, &h);
    if (w <= 0 || h <= 0) return;
    g_w = (float)w; g_h = (float)h;

    g_frames++;
    if (g_frames == 1) nclog("first frame reached our hook: %dx%d", (int)w, (int)h);

    double now = now_s();
    float fps = nc_fps_frame(&g_fps, now);

    bool in_settings = (g_settings_this != 0) || g_cfg.n_always;
    bool in_pause    = (g_pause_this != 0);
    bool menu_reach  = in_settings || in_pause;                 /* N button appears in either */
    bool in_world = (now - g_tick_time) < 0.6;
    bool play_hud = (now - g_play_time) < 0.3;
    if (!menu_reach) { g_menu_open = false; g_edit = false; kb_stop(); }

    bool any_armor = g_snap.present[0] || g_snap.present[1] || g_snap.present[2] || g_snap.present[3];
    bool fps_vis    = g_cfg.fps_on && (g_cfg.fps_menus || in_world);
    bool armor_vis  = g_cfg.armor_on && in_world && any_armor && !g_menu_open && !g_edit;
    bool elytra_vis = g_cfg.elytra_on && in_world && g_snap.gliding && !g_menu_open && !g_edit;
    bool arrow_vis  = g_cfg.arrow_on && in_world && g_snap.holding_bow && !g_menu_open && !g_edit;
    bool speed_vis  = g_cfg.speed_on && in_world && !g_menu_open && !g_edit;
    bool coords_vis = g_cfg.coords_on && in_world && !g_menu_open && !g_edit;
    bool elytra_angle_vis = g_cfg.elytra_angle_on && in_world && g_snap.gliding && g_snap.elytra_angle_valid && !g_menu_open && !g_edit;
    bool hud_btns   = play_hud && !in_settings && !g_menu_open && !g_edit;         /* the world itself */
    bool zoom_vis   = g_cfg.zoom_on && (hud_btns || (in_pause && g_cfg.zoom_pause && !g_menu_open && !g_edit));
    bool persp_vis  = g_cfg.persp_on && (hud_btns || (in_pause && g_cfg.persp_pause && !g_menu_open && !g_edit));
    bool drop_vis   = g_cfg.drop_on && (hud_btns || (in_pause && g_cfg.drop_pause && !g_menu_open && !g_edit));
    if (!zoom_vis) g_zoom_active = 0;

    bool need = fps_vis || armor_vis || elytra_vis || arrow_vis || speed_vis || coords_vis || elytra_angle_vis || zoom_vis || persp_vis || drop_vis || menu_reach || g_menu_open || g_edit;
    if (g_frames % 900 == 0 && g_beats < 6) {
        g_beats++;
        nclog("heartbeat: frames=%d settings=%d pause=%d world=%d play=%d menu=%d fps=%.0f", g_frames, g_settings_this != 0,
              g_pause_this != 0, (int)in_world, (int)play_hud, (int)g_menu_open, fps);
    }
    if (need && !g_drawing_logged) { g_drawing_logged = true; nclog("drawing started"); }

    if (!need) {
        pthread_mutex_lock(&g_mu);
        NcTouch keep_cap = g_touch;                    /* keep the finger id; hide every rect */
        nc_touch_init(&g_touch); g_touch.cap_id = keep_cap.cap_id;
        g_qn = 0;
        pthread_mutex_unlock(&g_mu);
        return;
    }

    if (!g_imgui_ready) {
        if (!init_imgui(w, h)) { g_imgui_failed = true; return; }
        g_imgui_ready = true;
    }
    ImGuiIO &io = ImGui::GetIO();

    /* touches collected by the input hook */
    struct { int type; float x, y; } evs[256];
    int n;
    pthread_mutex_lock(&g_mu);
    n = g_qn; memcpy(evs, g_q, sizeof(evs[0]) * n); g_qn = 0;
    pthread_mutex_unlock(&g_mu);
    if (n) io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
    for (int i = 0; i < n; i++) {
        io.AddMousePosEvent(evs[i].x, evs[i].y);
        if (evs[i].type == NC_EV_DOWN) io.AddMouseButtonEvent(0, true);
        if (evs[i].type == NC_EV_UP) { io.AddMouseButtonEvent(0, false); io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); }
    }

    static double last = 0.0;
    float dt = last > 0.0 ? (float)(now - last) : (1.0f / 60.0f);
    last = now;
    if (dt <= 0.0f) dt = 1.0f / 60.0f;
    if (dt > 0.25f) dt = 0.25f;

    ImGui_ImplOpenGL3_NewFrame();
    if (!g_tex_done) {           /* crisp pixel font: no smoothing when the texture is enlarged */
        g_tex_done = true;
        GLint prev = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
        glBindTexture(GL_TEXTURE_2D, (GLuint)(uintptr_t)io.Fonts->TexID);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
    }
    io.DisplaySize = V((float)w, (float)h);
    io.DeltaTime = dt;
    io.FontGlobalScale = (float)menu_font_mult((float)h);
    ImGui::NewFrame();

    NcRect hud[NC_MAX_HUD]; memset(hud, 0, sizeof hud);
    NcRect nrect; memset(&nrect, 0, sizeof nrect);
    NcRect wrect; memset(&wrect, 0, sizeof wrect);
    ImDrawList *fg = ImGui::GetForegroundDrawList();

    if (g_edit) {
        build_edit((float)w, (float)h);
        wrect.visible = 1; wrect.x = 0; wrect.y = 0; wrect.w = (float)w; wrect.h = (float)h;   /* whole screen is ours */
    } else {
        if (fps_vis)    draw_fps(fg, place(g_cfg.fps_x, g_cfg.fps_y, size_fps()), fps);
        if (armor_vis)  draw_armor(fg, place(g_cfg.armor_x, g_cfg.armor_y, size_armor()), false);
        if (elytra_vis) draw_elytra(fg, place(g_cfg.elytra_x, g_cfg.elytra_y, size_elytra()));
        if (arrow_vis)  draw_arrow(fg, place(g_cfg.arrow_x, g_cfg.arrow_y, size_arrow()), g_snap.arrow_count);
        if (speed_vis)  draw_speed(fg, place(g_cfg.speed_x, g_cfg.speed_y, size_speed()), g_snap.speed_bps);
        if (coords_vis) draw_coords(fg, place(g_cfg.coords_x, g_cfg.coords_y, size_coords()));
        if (elytra_angle_vis) draw_elytra_angle(fg, place(g_cfg.elytra_angle_x, g_cfg.elytra_angle_y, size_elytra_angle()));

        if (zoom_vis) {
            ImVec2 sz = elem_size(E_ZOOM);
            if (button_at("##night_zoom", place(g_cfg.zoom_x, g_cfg.zoom_y, sz), sz, g_cfg.zoom_text,
                          g_cfg.zoom_alpha, g_cfg.zoom_text_alpha, false, g_zoom_active != 0, g_cfg.zoom_bg_col, g_cfg.zoom_col, &hud[0]))
                g_zoom_active = g_zoom_active ? 0 : 1;
        }
        if (persp_vis) {
            ImVec2 sz = elem_size(E_PERSP);
            if (button_at("##night_persp", place(g_cfg.persp_x, g_cfg.persp_y, sz), sz, g_cfg.persp_text,
                          g_cfg.persp_alpha, g_cfg.persp_text_alpha, false, false, g_cfg.persp_bg_col, g_cfg.persp_col, &hud[1]))
                do_perspective();
        }
        if (drop_vis) {
            ImVec2 sz = elem_size(E_DROP);
            if (button_at("##night_drop", place(g_cfg.drop_x, g_cfg.drop_y, sz), sz, g_cfg.drop_text,
                          g_cfg.drop_alpha, g_cfg.drop_text_alpha, false, false, g_cfg.drop_bg_col, g_cfg.drop_col, &hud[2]))
                { if (g_cic && g_ci) cic_drop(g_cic, g_ci); else nclog("drop: game objects not captured yet"); }
        }
        if (menu_reach && !g_menu_open) {
            ImVec2 sz = elem_size(E_N);
            if (button_at("##night_n", place(g_cfg.n_x, g_cfg.n_y, sz), sz, g_cfg.n_text,
                          g_cfg.n_alpha, g_cfg.n_text_alpha, true, false, g_cfg.n_bg_col, g_cfg.n_col, &nrect)) {
                g_menu_open = true; nclog("menu opened");
            }
        }
        if (g_menu_open) build_menu((float)w, (float)h, &wrect);
    }

    if (!nc_cfg_equal(&g_cfg, &g_saved) && !ImGui::IsMouseDown(0)) {
        if (nc_cfg_save(&g_cfg, NC_CFG)) g_saved = g_cfg;
    }

    ImGui::Render();

    GLint prev_fbo = 0;
    GLboolean prev_mask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetBooleanv(GL_COLOR_WRITEMASK, prev_mask);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);                 /* always draw into the real screen */
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glColorMask(prev_mask[0], prev_mask[1], prev_mask[2], prev_mask[3]);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    GLenum gl_err = glGetError();
    if (gl_err != GL_NO_ERROR && !g_gl_err_logged) { g_gl_err_logged = true; nclog("GL error 0x%x after drawing", (unsigned)gl_err); }

    pthread_mutex_lock(&g_mu);
    g_touch.n = nrect; g_touch.win = wrect;
    for (int i = 0; i < NC_MAX_HUD; i++) g_touch.hud[i] = hud[i];
    pthread_mutex_unlock(&g_mu);
}

typedef EGLBoolean (*swap_fn)(EGLDisplay, EGLSurface);
static swap_fn g_orig_swap = 0;
static EGLBoolean hook_swap(EGLDisplay d, EGLSurface s) {
    nc_frame(d, s);
    g_hit_seen_n = 0;
    return g_orig_swap(d, s);
}

/* ------------------------------------------------------------------ startup */
static void *installer(void *) {
    int swap_done = 0, input_done = 0;
    for (int i = 0; i < 40 && !(swap_done && input_done); i++) {
        sleep(1);
        if (!swap_done) {
            int n = nc_got_hook("libminecraftpe.so", "eglSwapBuffers", (void *)hook_swap, (void **)&g_orig_swap);
            if (n > 0 || i == 0) nclog("hook eglSwapBuffers: %d slot(s)", n);
            swap_done = n > 0;
        }
        if (!input_done) {
            int n = nc_got_hook("libminecraftpe.so", "AInputQueue_getEvent", (void *)hook_getEvent, (void **)&g_orig_getEvent);
            if (n > 0 || i == 0) nclog("hook AInputQueue_getEvent: %d slot(s)", n);
            input_done = n > 0;
        }
    }
    nclog("installer finished: draw=%d input=%d", swap_done, input_done);
    return 0;
}

static void reg(const char *what, const char *sym, void *hook, void **orig) {
    tml_registerHook(sym, hook, orig);
    nclog("registered hook: %s", what);
}

__attribute__((constructor))
static void nc_init(void) {
    mkdir("/sdcard/games/com.mojang/NightClient", 0777);
    FILE *f = fopen(NC_LOG, "w");
    if (f) { fputs("Night Client " NC_VERSION " loaded\n", f); fclose(f); }

    nc_touch_init(&g_touch);
    nc_cfg_defaults(&g_cfg);
    int had = nc_cfg_load(&g_cfg, NC_CFG);
    if (!had) nc_cfg_save(&g_cfg, NC_CFG);
    g_saved = g_cfg;
    nclog("config %s", had ? "loaded" : "created");

    /* always on: autosprint + data snapshot, settings-screen detection, gameplay-screen detection */
    reg("player tick", "_ZN16MoveInputHandler4tickER11LocalPlayer", (void *)hook_tick, (void **)&g_orig_tick);
    reg("settings open", "_ZN24SettingsScreenController6onOpenEv", (void *)hook_settings_open, (void **)&g_orig_onOpen);
    reg("settings close", "_ZN24SettingsScreenControllerD1Ev", (void *)hook_settings_dtor, (void **)&g_orig_dtor);
    reg("gameplay screen", "_ZN16InGamePlayScreen10applyInputEf", (void *)hook_apply, (void **)&g_orig_apply);
    reg("pause tick", "_ZN21PauseScreenController4tickEv", (void *)hook_pause_tick, (void **)&g_orig_pausetick);
    reg("pause close", "_ZN21PauseScreenControllerD1Ev", (void *)hook_pause_dtor, (void **)&g_orig_pausedtor);

    /* per mod: can be switched off in config.txt (hook_x=0) if one of them ever crashes the game */
    if (g_cfg.hook_hurt)  reg("no hurt cam", "_ZN19LevelRendererPlayer7bobHurtER6Matrixf", (void *)hook_bobhurt, (void **)&g_orig_bob);
    if (g_cfg.hook_fov)   reg("zoom", "_ZN19LevelRendererPlayer6getFovEfb", (void *)hook_fov, (void **)&g_orig_fov);
    if (g_cfg.hook_persp) reg("perspective",
        "_ZN20ClientInputCallbacks21handlePointerLocationER14ClientInstanceRK24PointerLocationEventData11FocusImpact",
        (void *)hook_ptr, (void **)&g_orig_ptr);
    if (g_cfg.hook_xp) {
        reg("XP orb render optimizer", "_ZN21ExperienceOrbRenderer6renderER6EntityRK4Vec3ff", (void *)hook_xp_render, (void **)&g_orig_xp_render);
    }
    if (g_cfg.hook_crystal) {
        reg("End Crystal render optimizer", "_ZN20EnderCrystalRenderer6renderER6EntityRK4Vec3ff", (void *)hook_crystal_render, (void **)&g_orig_crystal_render);
        reg("End Crystal effects optimizer", "_ZN20EnderCrystalRenderer13renderEffectsER6EntityRK4Vec3ff", (void *)hook_crystal_effects, (void **)&g_orig_crystal_effects);
    }
    if (g_cfg.hook_hitbox) {
        reg("entity render hitboxes", "_ZN22EntityRenderDispatcher6renderER6EntityRK4Vec3ff", (void *)hook_entity_render, (void **)&g_orig_entity_render);
    }
    if (g_cfg.hook_perf) {
        reg("fast graphics", "_ZNK7Options16getFancyGraphicsEv", (void *)hook_fancy, (void **)&g_orig_fancy);
        reg("fancy skies", "_ZNK7Options13getFancySkiesEv", (void *)hook_skies, (void **)&g_orig_skies);
        reg("smooth lighting", "_ZNK7Options17getSmoothLightingEv", (void *)hook_light, (void **)&g_orig_light);
        reg("view bobbing", "_ZNK7Options10getBobViewEv", (void *)hook_bobview, (void **)&g_orig_bobview);
        reg("view distance", "_ZNK7Options21getViewDistanceChunksEv", (void *)hook_view, (void **)&g_orig_view);
    }

    pthread_t t;
    if (pthread_create(&t, 0, installer, 0) == 0) pthread_detach(t);
}

extern "C" __attribute__((visibility("default"))) void tml_init(void) {}
