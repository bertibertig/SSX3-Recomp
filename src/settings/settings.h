/*
 * Player settings: one INI file, read at start-up, written by the settings
 * screen (settings_tui.c, `ssx3_recomp --settings`). Design after
 * Amped2-Recomp's src/settings (same toolkit): defaults, then the file, then
 * the environment for one run.
 *
 * File: $SSX3_SETTINGS, else $XDG_CONFIG_HOME/ssx3-recomp/settings.ini
 * (~/.config/... without XDG_CONFIG_HOME), on Windows
 * %APPDATA%\ssx3-recomp\settings.ini. Without one, the old launcher's
 * run.conf in the same folder is taken over once.
 *
 * The rest of the program keeps reading its environment variables
 * (SSX3_RES_SCALE, SSX3_WINDOW, ...; src/render/vk_backend.c, main.c):
 * ssx3_settings_apply() exports each setting under that name unless the
 * environment already has it, so a variable set for one run still wins.
 */
#ifndef SSX3_SETTINGS_H
#define SSX3_SETTINGS_H

enum { SSX3_RENDER_VULKAN, SSX3_RENDER_CPU, SSX3_RENDER_COUNT };
enum { SSX3_LANG_AUTO, SSX3_LANG_EN, SSX3_LANG_FR, SSX3_LANG_DE, SSX3_LANG_ES,
       SSX3_LANG_COUNT };
enum { SSX3_ASPECT_4_3, SSX3_ASPECT_16_9, SSX3_ASPECT_16_10, SSX3_ASPECT_21_9,
       SSX3_ASPECT_AUTO, SSX3_ASPECT_COUNT };
/* Frame rate: the game's own 60 frames, or interpolated up to a cap. */
enum { SSX3_FPS_60, SSX3_FPS_DISPLAY, SSX3_FPS_120, SSX3_FPS_144, SSX3_FPS_165,
       SSX3_FPS_240, SSX3_FPS_UNCAPPED, SSX3_FPS_COUNT };
enum { SSX3_WIN_WINDOWED, SSX3_WIN_BORDERLESS, SSX3_WIN_FULLSCREEN, SSX3_WIN_COUNT };

#define SSX3_SCALE_MAX 5

typedef struct {
    int  renderer;           /* SSX3_RENDER_* */
    int  language;           /* SSX3_LANG_*: AUTO = the system's, if the disc has it */
    int  resolution_scale;   /* 1 .. SSX3_SCALE_MAX, x 640x480 */
    int  aspect_ratio;       /* SSX3_ASPECT_* */
    int  frame_rate;         /* SSX3_FPS_* */
    int  vsync;              /* 0 / 1 */
    int  window_mode;        /* SSX3_WIN_* */
    char monitor[96];        /* "primary", a display number or part of a name */
    int  skip_logos;         /* EA BIG + THX logos */
    int  skip_intro;         /* the intro movie */
    int  keyboard;           /* keyboard as controller 1 */
} Ssx3Settings;

extern Ssx3Settings g_ssx3_settings;

/* Defaults, then the file (or the old run.conf). Call once, early. */
void ssx3_settings_load(void);

/* Write the current values. 0 on success. */
int ssx3_settings_save(void);

/* The file in use. */
const char *ssx3_settings_path(void);

/* Export the settings as the environment variables the program reads, each
 * only where the environment does not set it already. */
void ssx3_settings_apply(void);

/* One line on stderr: what this run uses. */
void ssx3_settings_log(void);

/* Names as written to the file. */
const char *ssx3_language_code(int lang);          /* "auto", "en", "fr", "de", "es" */
const char *ssx3_language_name(int lang);          /* "English", "Deutsch", ... */
const char *ssx3_aspect_name(int aspect);           /* "4:3", ..., "auto" */
double      ssx3_aspect_value(int aspect);          /* w/h; auto: 16/9 for previews */

/* Whether the disc has that language's text (game_files/data/locale). */
int ssx3_language_on_disc(int lang);
/* The system's language as SSX3_LANG_* (EN when it is none of the four). */
int ssx3_language_system(void);
/* The language this run uses: the setting, or for AUTO the system's when the
 * disc has it, else English. Never AUTO. */
int ssx3_language_resolve(int lang);

/* For the XGetLanguage override (recomp_manual.c): the XC_LANGUAGE value of
 * SSX3_LANGUAGE (en 1, de 3, fr 4, es 5), 0 = leave the title's query alone. */
int ssx3_language_xbox(void);

/* The interactive settings screen in the terminal. Returns 1 when the player
 * chose to start the game, 0 to quit. */
int ssx3_settings_tui(void);

#endif /* SSX3_SETTINGS_H */
