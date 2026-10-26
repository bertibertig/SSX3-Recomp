/*
 * Player settings: the INI file, the old launcher's run.conf, the system
 * language and the export into the environment. See settings.h.
 */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE                    /* setenv, strcasecmp */
#endif
#include "settings.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define strcasecmp _stricmp
#else
#include <strings.h>
#include <sys/stat.h>
#endif

Ssx3Settings g_ssx3_settings;
static int s_lang_env;                 /* SSX3_LANGUAGE came from the environment */

static const char *const k_lang_code[SSX3_LANG_COUNT] = { "auto", "en", "fr", "de", "es" };
static const char *const k_lang_name[SSX3_LANG_COUNT] = {
    "Automatic", "English", "Français", "Deutsch", "Español" };
/* The disc's text: data/locale/cmn<suffix>.loc and friends. */
static const char *const k_lang_file[SSX3_LANG_COUNT] = { "", "amer", "fren", "grmn", "span" };
/* XC_LANGUAGE values the title's XGetLanguage returns (XDK xbox.h). */
static const int k_lang_xbox[SSX3_LANG_COUNT] = { 0, 1, 4, 3, 5 };

static const char *const k_aspect[SSX3_ASPECT_COUNT] = { "4:3", "16:9", "16:10", "21:9", "auto" };
static const char *const k_fps[SSX3_FPS_COUNT] = {
    "60", "display", "120", "144", "165", "240", "uncapped" };
static const char *const k_window[SSX3_WIN_COUNT] = { "windowed", "borderless", "fullscreen" };
static const char *const k_renderer[SSX3_RENDER_COUNT] = { "vulkan", "cpu" };

const char *ssx3_language_code(int l) { return l >= 0 && l < SSX3_LANG_COUNT ? k_lang_code[l] : "auto"; }
const char *ssx3_language_name(int l) { return l >= 0 && l < SSX3_LANG_COUNT ? k_lang_name[l] : "?"; }
const char *ssx3_aspect_name(int a)   { return a >= 0 && a < SSX3_ASPECT_COUNT ? k_aspect[a] : "4:3"; }

double ssx3_aspect_value(int a)
{
    static const double v[SSX3_ASPECT_COUNT] = { 4.0 / 3, 16.0 / 9, 16.0 / 10, 21.0 / 9, 16.0 / 9 };
    return a >= 0 && a < SSX3_ASPECT_COUNT ? v[a] : 4.0 / 3;
}

static void defaults(Ssx3Settings *s)
{
    memset(s, 0, sizeof *s);
    s->renderer = SSX3_RENDER_VULKAN;
    s->language = SSX3_LANG_AUTO;
    s->resolution_scale = 1;
    s->aspect_ratio = SSX3_ASPECT_16_9;
    s->frame_rate = SSX3_FPS_DISPLAY;
    s->window_mode = SSX3_WIN_WINDOWED;
    snprintf(s->monitor, sizeof s->monitor, "primary");
    s->skip_logos = 1;
    s->keyboard = 1;
}

/* ---- languages ------------------------------------------------------------ */

int ssx3_language_on_disc(int lang)
{
    char path[96];
    FILE *f;

    if (lang <= SSX3_LANG_AUTO || lang >= SSX3_LANG_COUNT)
        return 0;
    snprintf(path, sizeof path, "game_files/data/locale/cmn%s.loc", k_lang_file[lang]);
    if (!(f = fopen(path, "rb")))
        return 0;
    fclose(f);
    return 1;
}

static int lang_from_iso(const char *s)
{
    int i;
    for (i = SSX3_LANG_EN; i < SSX3_LANG_COUNT; i++)
        if (s && tolower((unsigned char)s[0]) == k_lang_code[i][0]
                && tolower((unsigned char)s[1]) == k_lang_code[i][1])
            return i;
    return SSX3_LANG_AUTO;
}

int ssx3_language_system(void)
{
#ifdef _WIN32
    /* The display language (Settings > Time & language), not the format
     * region: that is the language the player reads. */
    switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
    case LANG_FRENCH:  return SSX3_LANG_FR;
    case LANG_GERMAN:  return SSX3_LANG_DE;
    case LANG_SPANISH: return SSX3_LANG_ES;
    default:           return SSX3_LANG_EN;
    }
#else
    /* POSIX: the message locale, LANGUAGE's first entry before it (GNU). */
    static const char *const vars[] = { "LANGUAGE", "LC_ALL", "LC_MESSAGES", "LANG" };
    size_t i;
    for (i = 0; i < sizeof vars / sizeof vars[0]; i++) {
        const char *v = getenv(vars[i]);
        int l;
        if (!v || !*v || !strcmp(v, "C") || !strcmp(v, "POSIX") || !strncmp(v, "C.", 2))
            continue;
        l = lang_from_iso(v);
        return l != SSX3_LANG_AUTO ? l : SSX3_LANG_EN;
    }
    return SSX3_LANG_EN;
#endif
}

int ssx3_language_resolve(int lang)
{
    if (lang > SSX3_LANG_AUTO && lang < SSX3_LANG_COUNT && ssx3_language_on_disc(lang))
        return lang;
    if (lang == SSX3_LANG_AUTO && ssx3_language_on_disc(ssx3_language_system()))
        return ssx3_language_system();
    return SSX3_LANG_EN;
}

/* For the XGetLanguage override (recomp_manual.c): the XC_LANGUAGE value for
 * SSX3_LANGUAGE (en/fr/de/es), 0 to leave the title's own query alone. */
int ssx3_language_xbox(void)
{
    static int v = -1;
    if (v < 0) {
        int l = lang_from_iso(getenv("SSX3_LANGUAGE"));
        v = l != SSX3_LANG_AUTO ? k_lang_xbox[l] : 0;
    }
    return v;
}

/* ---- the file ------------------------------------------------------------- */

static char s_path[512];
static char s_dir[512];

static void find_paths(void)
{
    const char *e = getenv("SSX3_SETTINGS"), *base;

    if (s_path[0])
        return;
    if (e && *e) {
        char *slash;
        snprintf(s_path, sizeof s_path, "%s", e);
        snprintf(s_dir, sizeof s_dir, "%s", e);
        slash = strrchr(s_dir, '/');
#ifdef _WIN32
        if (!slash || strrchr(s_dir, '\\') > slash)
            slash = strrchr(s_dir, '\\');
#endif
        if (slash)
            *slash = 0;
        else
            snprintf(s_dir, sizeof s_dir, ".");
        return;
    }
#ifdef _WIN32
    base = getenv("APPDATA");
    snprintf(s_dir, sizeof s_dir, "%s\\ssx3-recomp", base && *base ? base : ".");
    snprintf(s_path, sizeof s_path, "%s\\settings.ini", s_dir);
#else
    base = getenv("XDG_CONFIG_HOME");
    if (base && *base)
        snprintf(s_dir, sizeof s_dir, "%s/ssx3-recomp", base);
    else
        snprintf(s_dir, sizeof s_dir, "%s/.config/ssx3-recomp",
                 getenv("HOME") ? getenv("HOME") : ".");
    snprintf(s_path, sizeof s_path, "%s/settings.ini", s_dir);
#endif
}

const char *ssx3_settings_path(void)
{
    find_paths();
    return s_path;
}

static int pick(const char *v, const char *const *names, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (!strcasecmp(v, names[i]))
            return i;
    return -1;
}

static int boolean(const char *v)
{
    return !strcasecmp(v, "on") || !strcasecmp(v, "1") || !strcasecmp(v, "yes")
        || !strcasecmp(v, "true");
}

static char *trim(char *s)
{
    char *e;
    while (isspace((unsigned char)*s))
        s++;
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = 0;
    return s;
}

/* One key = value. `old`: the run.conf keys (RES_SCALE, FPS, ...). */
static void set_key(Ssx3Settings *s, const char *key, const char *v, int old)
{
    int i;

    if (!strcasecmp(key, old ? "RENDERER" : "renderer")) {
        if ((i = pick(v, k_renderer, SSX3_RENDER_COUNT)) >= 0) s->renderer = i;
    } else if (!old && !strcasecmp(key, "language")) {
        if ((i = pick(v, k_lang_code, SSX3_LANG_COUNT)) >= 0) s->language = i;
        else fprintf(stderr, "[settings] language = %s: use auto, en, fr, de or es\n", v);
    } else if (!strcasecmp(key, old ? "RES_SCALE" : "resolution_scale")) {
        i = atoi(v);
        if (i >= 1 && i <= SSX3_SCALE_MAX) s->resolution_scale = i;
    } else if (!strcasecmp(key, old ? "ASPECT" : "aspect_ratio")) {
        if ((i = pick(v, k_aspect, SSX3_ASPECT_COUNT)) >= 0) s->aspect_ratio = i;
    } else if (!strcasecmp(key, old ? "FPS" : "frame_rate")) {
        if ((i = pick(v, k_fps, SSX3_FPS_COUNT)) >= 0) s->frame_rate = i;
    } else if (!strcasecmp(key, old ? "VSYNC" : "vsync")) {
        s->vsync = boolean(v);
    } else if (!strcasecmp(key, old ? "WINDOW" : "window_mode")) {
        if ((i = pick(v, k_window, SSX3_WIN_COUNT)) >= 0) s->window_mode = i;
    } else if (!strcasecmp(key, old ? "MONITOR" : "monitor")) {
        if (*v) snprintf(s->monitor, sizeof s->monitor, "%s", v);
    } else if (!old && !strcasecmp(key, "skip_logos")) {
        s->skip_logos = boolean(v);
    } else if (!old && !strcasecmp(key, "skip_intro")) {
        s->skip_intro = boolean(v);
    } else if (!old && !strcasecmp(key, "keyboard")) {
        s->keyboard = boolean(v);
    }
}

static int read_file(Ssx3Settings *s, const char *path, int old)
{
    FILE *f = fopen(path, "r");
    char line[256];

    if (!f)
        return 0;
    while (fgets(line, sizeof line, f)) {
        char *l = trim(line), *eq;
        if (!*l || *l == '#' || *l == ';' || *l == '[')
            continue;
        if (!(eq = strchr(l, '=')))
            continue;
        *eq = 0;
        set_key(s, trim(l), trim(eq + 1), old);
    }
    fclose(f);
    return 1;
}

void ssx3_settings_load(void)
{
    char old[600];

    defaults(&g_ssx3_settings);
    find_paths();
    if (read_file(&g_ssx3_settings, s_path, 0))
        return;
    /* First start with this version: take over the old launch menu's choice
     * (scripts/_launch_menu.sh / run.ps1 saved it next to where we live). */
    snprintf(old, sizeof old, "%s%crun.conf", s_dir,
#ifdef _WIN32
             '\\'
#else
             '/'
#endif
             );
    if (read_file(&g_ssx3_settings, old, 1))
        fprintf(stderr, "[settings] took over %s (saved as %s on the next save)\n", old, s_path);
}

static int make_dirs(const char *dir)
{
    char buf[512];
    size_t i;

    snprintf(buf, sizeof buf, "%s", dir);
    for (i = 1; buf[i]; i++) {
        if (buf[i] != '/' && buf[i] != '\\')
            continue;
        if (i == 2 && buf[1] == ':')
            continue;                                  /* C:\ */
        buf[i] = 0;
#ifdef _WIN32
        _mkdir(buf);
#else
        mkdir(buf, 0755);
#endif
        buf[i] = dir[i];
    }
#ifdef _WIN32
    return _mkdir(buf) == 0 || errno == EEXIST ? 0 : -1;
#else
    return mkdir(buf, 0755) == 0 || errno == EEXIST ? 0 : -1;
#endif
}

int ssx3_settings_save(void)
{
    const Ssx3Settings *s = &g_ssx3_settings;
    FILE *f;

    find_paths();
    make_dirs(s_dir);
    if (!(f = fopen(s_path, "w")))
        return -1;
    fprintf(f,
            "# SSX 3 Recomp settings - written by `ssx3_recomp --settings` (run.sh / run.ps1 --settings).\n"
            "# An environment variable of the same meaning (SSX3_RES_SCALE, ...) overrides a line for one run.\n"
            "renderer = %s\n"
            "language = %s\n"
            "resolution_scale = %d\n"
            "aspect_ratio = %s\n"
            "frame_rate = %s\n"
            "vsync = %s\n"
            "window_mode = %s\n"
            "monitor = %s\n"
            "skip_logos = %s\n"
            "skip_intro = %s\n"
            "keyboard = %s\n",
            k_renderer[s->renderer], k_lang_code[s->language], s->resolution_scale,
            k_aspect[s->aspect_ratio], k_fps[s->frame_rate], s->vsync ? "on" : "off",
            k_window[s->window_mode], s->monitor, s->skip_logos ? "on" : "off",
            s->skip_intro ? "on" : "off", s->keyboard ? "on" : "off");
    return fclose(f) == 0 ? 0 : -1;
}

/* ---- into the environment ------------------------------------------------- */

static void env_default(const char *name, const char *value)
{
    const char *cur = getenv(name);
    if (cur && *cur)
        return;
#ifdef _WIN32
    {
        char buf[256];
        snprintf(buf, sizeof buf, "%s=%s", name, value);
        _putenv(buf);
    }
#else
    setenv(name, value, 1);
#endif
}

void ssx3_settings_apply(void)
{
    const Ssx3Settings *s = &g_ssx3_settings;
    char buf[32];

    env_default("SSX3_RENDERER", k_renderer[s->renderer]);
    if (!strcmp(getenv("SSX3_RENDERER"), "cpu"))
        env_default("RECOMP_FB_WINDOW", "1");      /* the CPU rasteriser's window */
    s_lang_env = getenv("SSX3_LANGUAGE") && *getenv("SSX3_LANGUAGE");
    env_default("SSX3_LANGUAGE", k_lang_code[ssx3_language_resolve(s->language)]);
    snprintf(buf, sizeof buf, "%d", s->resolution_scale);
    env_default("SSX3_RES_SCALE", buf);
    if (s->aspect_ratio != SSX3_ASPECT_4_3)
        env_default("SSX3_ASPECT", k_aspect[s->aspect_ratio]);
    switch (s->frame_rate) {
    case SSX3_FPS_60:       env_default("SSX3_INTERP", "0"); break;
    case SSX3_FPS_DISPLAY:  break;                  /* the back end's default */
    case SSX3_FPS_UNCAPPED: env_default("SSX3_FPS_LIMIT", "0"); break;
    default:                env_default("SSX3_FPS_LIMIT", k_fps[s->frame_rate]); break;
    }
    if (s->vsync)
        env_default("SSX3_VSYNC", "1");
    env_default("SSX3_WINDOW", k_window[s->window_mode]);
    if (strcasecmp(s->monitor, "primary"))
        env_default("SSX3_MONITOR", s->monitor);
    env_default("SSX3_SKIP_LOGOS", s->skip_logos ? "1" : "0");
    env_default("SSX3_SKIP_INTRO", s->skip_intro ? "1" : "0");
    env_default("RECOMP_KEYBOARD", s->keyboard ? "1" : "0");
}

void ssx3_settings_log(void)
{
    const char *e = getenv("SSX3_LANGUAGE");
    fprintf(stderr, "[settings] %s: %s, language %s%s, %sx, aspect %s, frames %s, vsync %s, %s, monitor %s\n",
            ssx3_settings_path(), getenv("SSX3_RENDERER") ? getenv("SSX3_RENDERER") : "?",
            e ? e : "?", s_lang_env ? " (environment)"
                         : g_ssx3_settings.language == SSX3_LANG_AUTO ? " (auto)" : "",
            getenv("SSX3_RES_SCALE") ? getenv("SSX3_RES_SCALE") : "1",
            getenv("SSX3_ASPECT") ? getenv("SSX3_ASPECT") : "4:3",
            getenv("SSX3_INTERP") && *getenv("SSX3_INTERP") == '0' ? "60"
            : getenv("SSX3_FPS_LIMIT") ? getenv("SSX3_FPS_LIMIT") : "display",
            getenv("SSX3_VSYNC") && *getenv("SSX3_VSYNC") == '1' ? "on" : "off",
            getenv("SSX3_WINDOW") ? getenv("SSX3_WINDOW") : "windowed",
            getenv("SSX3_MONITOR") ? getenv("SSX3_MONITOR") : "primary");
}
