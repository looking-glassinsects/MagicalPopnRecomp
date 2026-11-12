/* F1 overlay menu. A centred pink panel over the dimmed game: cover-style title, one list per
 * page, the selected row on a blue bar, option values shown as "< value >", a note about the
 * selected row and a key reminder underneath.
 *   Up/Down select, Left/Right change, Enter/Space activate, Esc/Backspace back (closes on
 *   the main page), F1 closes from anywhere. Mouse: hover selects, left click changes or
 *   activates, right click changes the other way.
 * Drawn with GDI into a 32-bit DIB that menu_draw also gives its (premultiplied) alpha. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "menu.h"
#include "settings.h"
#include "platform.h"
#include "hw.h"
#include "pad.h"

/* ---------------------------------------------------------------- strings */
enum {
    S_TITLE_MAIN, S_TITLE_VIDEO, S_TITLE_AUDIO, S_TITLE_KEYS,
    S_RESUME, S_SAVE, S_LOAD, S_SLOT, S_CONTINUE, S_PG_VIDEO, S_PG_AUDIO, S_PG_KEYS,
    S_SCREENSHOT, S_LANGUAGE, S_RESET, S_QUIT, S_BACK,
    S_MOTION, S_WINSIZE, S_DISPLAY, S_WINDOWED, S_FULLSCREEN, S_VSYNC, S_LOWLAT, S_WIDESCREEN, S_PIXEL,
    S_PIX_SQUARE, S_PIX_TV, S_PIX_STRETCH, S_INTEGER, S_SMOOTH, S_SCANLINES, S_SPRLIMIT, S_SHOWFPS,
    S_VOLUME, S_MUTE, S_MUTE_BG, S_PAUSE_BG,
    S_FFSPEED, S_MAX, S_KEYS_DEFAULT,
    S_ON, S_OFF, S_EMPTY, S_PRESS_KEY, S_PRESS_AGAIN_QUIT, S_PRESS_AGAIN_RESET,
    S_HINT_MAIN, S_HINT_SUB, S_HINT_CAPTURE, S_FOOT_MAIN, S_FOOT_SUB, S_KEYS_WERE_RESET,
    S_BTN_UP, S_BTN_DOWN, S_BTN_LEFT, S_BTN_RIGHT, S_BTN_A, S_BTN_B, S_BTN_X, S_BTN_Y, S_BTN_L, S_BTN_R,
    S_BTN_START, S_BTN_SELECT,
    S_PAD, S_PAD_SETUP, S_PAD_NONE, S_PAD_DONE, S_PAD_NOPAD, S_PAD_HINT,
    S_PAD_B, S_PAD_A, S_PAD_Y, S_PAD_X, S_PAD_FF, S_PAD_MENU, S_UPDATES, S_NOSLOW, S_COUNT
};

static const char *STR[S_COUNT][LANG_COUNT] = {
    {"Settings", "設定"},
    {"Video", "映像"}, {"Sound", "音声"}, {"Controls", "操作"},
    {"Resume", "再開"},
    {"Save state", "ステートセーブ"},
    {"Load state", "ステートロード"},
    {"Save slot", "セーブスロット"},
    {"Continue on launch", "起動時に続きから"},
    {"Video", "映像"},
    {"Sound", "音声"},
    {"Controls", "操作"},
    {"Screenshot", "スクリーンショット"},
    {"Language", "言語"},
    {"Reset game", "リセット"},
    {"Quit game", "ゲームを終了"},
    {"Back", "戻る"},
    {"Motion smoothing", "モーション補間"},
    {"Window size", "ウィンドウサイズ"},
    {"Display mode", "表示モード"},
    {"Windowed", "ウィンドウ"}, {"Fullscreen", "フルスクリーン"},
    {"VSync", "垂直同期"},
    {"Low latency", "低遅延モード"},
    {"Widescreen", "ワイド画面"},
    {"Pixel shape", "ピクセル形状"},
    {"Square", "正方形"}, {"TV (8:7)", "テレビ (8:7)"}, {"Stretch", "引き伸ばし"},
    {"Integer scaling", "整数倍スケーリング"},
    {"Smoothing", "スムージング"},
    {"Scanlines", "走査線"},
    {"Sprite limit", "スプライト制限"},
    {"Show FPS", "FPS表示"},
    {"Volume", "音量"},
    {"Mute", "ミュート"},
    {"Mute in background", "非アクティブ時にミュート"},
    {"Pause in background", "非アクティブ時に一時停止"},
    {"Fast-forward speed", "早送り速度"},
    {"Max", "最大"},
    {"Reset to defaults", "初期設定に戻す"},
    {"On", "オン"}, {"Off", "オフ"}, {"empty", "空き"},
    {"press a key...", "キーを押してください..."},
    {"press again to quit", "もう一度押すと終了"},
    {"press again to reset", "もう一度押すとリセット"},
    {"Up/Down select, Left/Right change. Saved automatically.", "上下で選択、左右で変更。設定は自動で保存されます。"},
    {"Up/Down select, Left/Right change, Enter activate.", "上下で選択、左右で変更、Enterで決定。"},
    {"Press the new key. Esc cancels.", "新しいキーを押してください。Escでキャンセル。"},
    {"F1 / Esc: close     F5 / F9: save / load     F11: fullscreen",
     "F1 / Esc: 閉じる     F5 / F9: セーブ / ロード     F11: フルスクリーン"},
    {"Esc / Backspace: back     F1: close     F11: fullscreen",
     "Esc / Backspace: 戻る     F1: 閉じる     F11: フルスクリーン"},
    {"Controls reset to defaults.", "操作を初期設定に戻しました。"},
    {"D-pad up", "十字キー 上"}, {"D-pad down", "十字キー 下"}, {"D-pad left", "十字キー 左"},
    {"D-pad right", "十字キー 右"},
    {"A", "A"}, {"B", "B"}, {"X", "X"}, {"Y", "Y"}, {"L", "L"}, {"R", "R"},
    {"Start", "スタート"}, {"Select", "セレクト"},
    {"Controller", "コントローラー"},
    {"Set up controller", "コントローラーを設定"},
    {"none connected", "未接続"},
    {"Controller set up.", "コントローラーを設定しました。"},
    {"No controller found. Connect one and try again.", "コントローラーが見つかりません。接続してからもう一度。"},
    {"Press %s on the controller.   Esc: skip   Backspace: stop",
     "コントローラーで %s を押してください。 Esc: スキップ  Backspace: 中止"},
    {"B (bottom)", "B（下）"}, {"A (right)", "A（右）"}, {"Y (left)", "Y（左）"}, {"X (top)", "X（上）"},
    {"Fast-forward (hold)", "早送り（長押し）"},
    {"Open the menu", "メニューを開く"},
    {"Check for updates", "アップデートの確認"},
    {"Remove slowdown", "処理落ちをなくす"},
};

static const char *TSTR[T_COUNT][LANG_COUNT] = {
    {"Screenshot saved", "スクリーンショットを保存しました"},
    {"F1  menu", "F1  メニュー"},
    {"Fast-forward", "早送り"},
    {"Paused", "一時停止中"},
    {"Magical Pop'n", "マジカルポップン"},
    {"Game reset", "リセットしました"},
    {"Saved to slot %d", "スロット%dにセーブしました"},
    {"Loaded slot %d", "スロット%dをロードしました"},
    {"Slot %d is empty", "スロット%dは空です"},
    {"Continuing where you left off", "前回の続きから再開しました"},
    {"Slot %d", "スロット%d"},
    {"empty", "空き"},
    {"Could not save slot %d", "スロット%dにセーブできませんでした"},
    {"Controller connected: %s", "コントローラー接続: %s"},
    {"Version %s of Magical Pop'n Recomp is available.\n\nUpdate now? The new version is downloaded (about 90 MB), "
     "the game is rebuilt (a minute or two) and then starts again. Your saves and settings are kept.",
     "Magical Pop'n Recomp のバージョン %s が利用できます。\n\n今すぐアップデートしますか？ 新しいバージョンをダウンロード（約90MB）して"
     "ゲームをビルドし直し（1～2分）、自動で再起動します。セーブと設定はそのまま残ります。"},
    {"Downloading the update   %d%%", "アップデートをダウンロード中   %d%%"},
    {"The update could not be downloaded. Please check your internet connection, or get the new version from the project's GitHub page.",
     "アップデートをダウンロードできませんでした。インターネット接続を確認するか、GitHubのページから新しいバージョンを入手してください。"},
};

static const char *S(int id) { return STR[id][g_set.language]; }
const char *tr(int id) { return TSTR[id][g_set.language]; }

/* ---------------------------------------------------------------- items */
enum { PG_MAIN, PG_VIDEO, PG_AUDIO, PG_KEYS, PG_COUNT };
enum {
    I_RESUME, I_SAVE, I_LOAD, I_SLOT, I_CONTINUE, I_NOSLOW, I_PG_VIDEO, I_PG_AUDIO, I_PG_KEYS, I_LANGUAGE, I_UPDATES,
    I_SCREENSHOT, I_RESET, I_QUIT, I_BACK,
    I_SCALE, I_FULLSCREEN, I_VSYNC, I_LOWLAT, I_MOTION, I_WIDESCREEN, I_PIXEL, I_INTEGER, I_SMOOTH, I_SCANLINES, I_SPRLIMIT, I_SHOWFPS,
    I_VOLUME, I_MUTE, I_MUTE_BG, I_PAUSE_BG,
    I_KEY0, I_KEY_LAST = I_KEY0 + BTN_COUNT - 1, I_FFSPEED, I_KEYS_DEFAULT, I_PADNAME, I_PADSETUP,
};

/* a note shown under the list for the selected row (NULL: the page's default hint) */
static const char *NOTE[][LANG_COUNT] = {
    [I_RESUME] = {"Back to the game.", "ゲームに戻ります。"},
    [I_SAVE] = {"Save your exact place in the selected slot (F5).", "選択中のスロットに現在の状態を保存します (F5)。"},
    [I_LOAD] = {"Return to the state saved in the selected slot (F9).", "選択中のスロットの状態に戻ります (F9)。"},
    [I_SLOT] = {"Nine slots. F6 / F7 also switch slots in game.", "スロットは9つ。ゲーム中は F6 / F7 でも切り替えられます。"},
    [I_CONTINUE] = {"Quit any time and start again right where you were.", "終了しても次回はその場所から再開します。"},
    [I_NOSLOW] = {"On: busy scenes run at full speed. Off: they slow down like the original game.",
                  "オン：重い場面も通常の速さで動きます。オフ：オリジナルと同じように遅くなります。"},
    [I_PG_VIDEO] = {"Window, fullscreen, widescreen, vsync and picture options.", "ウィンドウ、フルスクリーン、ワイド画面、垂直同期など。"},
    [I_PG_AUDIO] = {"Volume and background behaviour.", "音量と非アクティブ時の動作。"},
    [I_PG_KEYS] = {"Change keyboard keys and fast-forward speed.", "キー割り当てと早送り速度。"},
    [I_LANGUAGE] = {"Menu language. The game's own text comes from your ROM.", "メニューの言語。ゲーム内の文章はROMによります。"},
    [I_UPDATES] = {"When the game starts, look for a new version and offer to install it.",
                   "ゲームの起動時に新しいバージョンを確認し、あればアップデートできます。"},
    [I_SCREENSHOT] = {"Saved to the screenshots folder (F12).", "screenshots フォルダに保存します (F12)。"},
    [I_RESET] = {"Restart the game from the beginning.", "ゲームを最初からやり直します。"},
    [I_QUIT] = {"Your place is kept when Continue on launch is On.", "「起動時に続きから」がオンなら続きから再開できます。"},
    [I_SCALE] = {"Size of the window in windowed mode.", "ウィンドウモード時の大きさ。"},
    [I_FULLSCREEN] = {"F11 or Alt+Enter also switch.", "F11 または Alt+Enter でも切り替えられます。"},
    [I_VSYNC] = {"Wait for the monitor refresh. Off can tear. Motion smoothing always uses it.", "モニターのリフレッシュに同期します。オフだと画面が裂けることがあります。モーション補間中は常に使います。"},
    [I_LOWLAT] = {"Show each frame the moment it's drawn and read input late.", "描画直後に表示し、入力を直前に読み取ります。"},
    [I_MOTION] = {"120: an in-between picture after every game frame (screens above 75 Hz). 60 is off. Uses VSync; ~1.5 frames of delay.",
                  "120：ゲームの各フレームの間に中間フレームを描き足します（75Hzを超える画面）。60はオフ。垂直同期を使い、約1.5フレーム遅延します。"},
    [I_WIDESCREEN] = {"Shows more of the level at the sides. Not stretched.", "左右により広く表示します。引き伸ばしではありません。"},
    [I_PIXEL] = {"Square pixels, the shape a TV showed, or fill the window.", "正方形、テレビの比率、またはウィンドウいっぱい。"},
    [I_INTEGER] = {"Whole-number scaling only: perfectly even pixels.", "整数倍のみで拡大し、ドットを均一にします。"},
    [I_SMOOTH] = {"Soften the pixels when scaling.", "拡大時にドットをなめらかにします。"},
    [I_SCANLINES] = {"Dark lines between rows, like a CRT.", "ブラウン管のような走査線を表示します。"},
    [I_SPRLIMIT] = {"On: the SNES's per-line sprite limit, which can cut sprites off. Off: all are drawn.",
                    "オン：SNESの1ラインのスプライト制限で、欠けることがあります。オフ：すべて表示。"},
    [I_SHOWFPS] = {"Frame rate and speed in the corner.", "フレームレートと速度を表示します。"},
    [I_VOLUME] = {"Master volume.", "全体の音量。"},
    [I_MUTE] = {"Silence all sound.", "すべての音を消します。"},
    [I_MUTE_BG] = {"Silence the game while another window is active.", "他のウィンドウがアクティブな間は音を消します。"},
    [I_PAUSE_BG] = {"Pause the game while another window is active.", "他のウィンドウがアクティブな間は一時停止します。"},
    [I_FFSPEED] = {"Hold Tab or the right trigger to fast-forward.", "Tab または右トリガーを押している間、早送りします。"},
    [I_KEYS_DEFAULT] = {"Keys: arrows, Z X A S, Q W, Enter, Right Shift. Controllers: their usual layout.",
                        "キー：矢印、Z X A S、Q W、Enter、右Shift。コントローラー：標準の配置。"},
    [I_PADNAME] = {"Any controller works. Buttons go by position: bottom B, right A, left Y, top X.",
                   "どのコントローラーも使えます。ボタンは位置で対応：下B、右A、左Y、上X。"},
    [I_PADSETUP] = {"Choose what each button on your controller does, one at a time.",
                    "コントローラーのボタンを1つずつ割り当てます。"},
    [I_PADSETUP + 1] = {NULL, NULL},
};

static int is_option(int id) {
    return id == I_SLOT || id == I_CONTINUE || id == I_NOSLOW || id == I_LANGUAGE || id == I_UPDATES || (id >= I_SCALE && id <= I_PAUSE_BG) || id == I_FFSPEED;
}
static int is_subpage(int id) { return id == I_PG_VIDEO || id == I_PG_AUDIO || id == I_PG_KEYS; }

static const int KEY_ORDER[BTN_COUNT] = {BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_A, BTN_B, BTN_X, BTN_Y,
                                         BTN_L, BTN_R, BTN_START, BTN_SELECT};
static const int KEY_LABEL[BTN_COUNT] = {S_BTN_UP, S_BTN_DOWN, S_BTN_LEFT, S_BTN_RIGHT, S_BTN_A, S_BTN_B, S_BTN_X,
                                         S_BTN_Y, S_BTN_L, S_BTN_R, S_BTN_START, S_BTN_SELECT};

static int g_open, g_page, g_sel, g_binding = -1, g_armed = -1;
static int g_page_sel[PG_COUNT];
static int g_status = -1;                      /* S_* shown in gold instead of the note, until the selection moves */

/* Controls > Set up controller: one step per button, in this order (-1 = not running) */
static int g_padstep = -1;
static const int PAD_STEPS[][2] = {
    {VP_UP, S_BTN_UP}, {VP_DOWN, S_BTN_DOWN}, {VP_LEFT, S_BTN_LEFT}, {VP_RIGHT, S_BTN_RIGHT},
    {VP_SOUTH, S_PAD_B}, {VP_EAST, S_PAD_A}, {VP_WEST, S_PAD_Y}, {VP_NORTH, S_PAD_X},
    {VP_L, S_BTN_L}, {VP_R, S_BTN_R}, {VP_SELECT, S_BTN_SELECT}, {VP_START, S_BTN_START},
    {VP_FF, S_PAD_FF}, {VP_MENU, S_PAD_MENU},
};
#define PAD_NSTEPS ((int)(sizeof PAD_STEPS / sizeof PAD_STEPS[0]))

static void pad_step_next(void) {
    if (++g_padstep >= PAD_NSTEPS) { pad_setup_end(1); g_padstep = -1; g_status = S_PAD_DONE; }
}
static void pad_setup_stop(void) { if (g_padstep >= 0) { pad_setup_end(0); g_padstep = -1; } }

int menu_pad_setup_active(void) { return g_open && g_padstep >= 0; }

void menu_tick(void) {
    if (g_open && g_padstep >= 0 && pad_setup_poll(PAD_STEPS[g_padstep][0])) pad_step_next();
}

/* row rectangles from the last draw, for the mouse */
#define MAX_ROWS 32
static RECT g_row_rect[MAX_ROWS];
static int g_row_count, g_mouse_x = -1, g_mouse_y = -1;

static int page_items(int page, int *out) {
    int n = 0;
    switch (page) {
    case PG_MAIN:
        out[n++] = I_RESUME; out[n++] = I_SAVE; out[n++] = I_LOAD; out[n++] = I_SLOT; out[n++] = I_CONTINUE; out[n++] = I_NOSLOW;
        out[n++] = I_PG_VIDEO; out[n++] = I_PG_AUDIO; out[n++] = I_PG_KEYS; out[n++] = I_LANGUAGE; out[n++] = I_UPDATES;
        out[n++] = I_SCREENSHOT; out[n++] = I_RESET; out[n++] = I_QUIT; break;
    case PG_VIDEO:
        out[n++] = I_SCALE; out[n++] = I_FULLSCREEN; out[n++] = I_VSYNC; out[n++] = I_LOWLAT; out[n++] = I_MOTION; out[n++] = I_WIDESCREEN;
        out[n++] = I_PIXEL; out[n++] = I_INTEGER; out[n++] = I_SMOOTH; out[n++] = I_SCANLINES; out[n++] = I_SPRLIMIT; out[n++] = I_SHOWFPS;
        out[n++] = I_BACK; break;
    case PG_AUDIO:
        out[n++] = I_VOLUME; out[n++] = I_MUTE; out[n++] = I_MUTE_BG; out[n++] = I_PAUSE_BG; out[n++] = I_BACK; break;
    case PG_KEYS:
        out[n++] = I_PADNAME; out[n++] = I_PADSETUP;
        for (int i = 0; i < BTN_COUNT; i++) out[n++] = I_KEY0 + i;
        out[n++] = I_FFSPEED; out[n++] = I_KEYS_DEFAULT; out[n++] = I_BACK; break;
    }
    return n;
}

static void key_name(int vk, char *out, int size) {
    if (!vk) { snprintf(out, size, "(none)"); return; }
    UINT sc = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    switch (vk) {
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_PRIOR: case VK_NEXT: case VK_END:
    case VK_HOME: case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK: case VK_RCONTROL: case VK_RMENU:
        sc |= 0x100; break;
    }
    wchar_t w[64];
    if (!GetKeyNameTextW((LONG)(sc << 16), w, 64)) swprintf(w, 64, L"0x%02X", vk);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, size, NULL, NULL);
}

static const char *label_of(int id) {
    switch (id) {
    case I_RESUME: return S(S_RESUME);
    case I_SAVE: return S(S_SAVE);
    case I_LOAD: return S(S_LOAD);
    case I_SLOT: return S(S_SLOT);
    case I_CONTINUE: return S(S_CONTINUE);
    case I_NOSLOW: return S(S_NOSLOW);
    case I_PG_VIDEO: return S(S_PG_VIDEO);
    case I_PG_AUDIO: return S(S_PG_AUDIO);
    case I_PG_KEYS: return S(S_PG_KEYS);
    case I_LANGUAGE: return S(S_LANGUAGE);
    case I_UPDATES: return S(S_UPDATES);
    case I_SCREENSHOT: return S(S_SCREENSHOT);
    case I_RESET: return S(S_RESET);
    case I_QUIT: return S(S_QUIT);
    case I_BACK: return S(S_BACK);
    case I_SCALE: return S(S_WINSIZE);
    case I_FULLSCREEN: return S(S_DISPLAY);
    case I_VSYNC: return S(S_VSYNC);
    case I_LOWLAT: return S(S_LOWLAT);
    case I_MOTION: return S(S_MOTION);
    case I_WIDESCREEN: return S(S_WIDESCREEN);
    case I_PIXEL: return S(S_PIXEL);
    case I_INTEGER: return S(S_INTEGER);
    case I_SMOOTH: return S(S_SMOOTH);
    case I_SCANLINES: return S(S_SCANLINES);
    case I_SPRLIMIT: return S(S_SPRLIMIT);
    case I_SHOWFPS: return S(S_SHOWFPS);
    case I_VOLUME: return S(S_VOLUME);
    case I_MUTE: return S(S_MUTE);
    case I_MUTE_BG: return S(S_MUTE_BG);
    case I_PAUSE_BG: return S(S_PAUSE_BG);
    case I_FFSPEED: return S(S_FFSPEED);
    case I_KEYS_DEFAULT: return S(S_KEYS_DEFAULT);
    case I_PADNAME: return S(S_PAD);
    case I_PADSETUP: return S(S_PAD_SETUP);
    }
    if (id >= I_KEY0 && id <= I_KEY_LAST) return S(KEY_LABEL[id - I_KEY0]);
    return "";
}

/* value text of an option row (or a key binding) */
static void value_of(int id, char *v, int size) {
    const char *onoff[2] = {S(S_OFF), S(S_ON)};
    v[0] = 0;
    switch (id) {
    case I_SLOT: {
        char info[64]; state_slot_info(g_set.slot, info, sizeof info);
        snprintf(v, size, "%d   %s", g_set.slot, info[0] ? info : S(S_EMPTY)); break;
    }
    case I_CONTINUE: snprintf(v, size, "%s", onoff[g_set.resume]); break;
    case I_NOSLOW: snprintf(v, size, "%s", onoff[g_set.no_slowdown]); break;
    case I_LANGUAGE: snprintf(v, size, "%s", g_set.language == LANG_JA ? "日本語" : "English"); break;
    case I_UPDATES: snprintf(v, size, "%s", onoff[g_set.check_updates]); break;
    case I_SCALE: snprintf(v, size, "%dx", g_set.scale); break;
    case I_FULLSCREEN: snprintf(v, size, "%s", S(g_set.fullscreen ? S_FULLSCREEN : S_WINDOWED)); break;
    case I_VSYNC: snprintf(v, size, "%s", onoff[g_set.vsync]); break;
    case I_LOWLAT: snprintf(v, size, "%s", onoff[g_set.low_latency]); break;
    case I_MOTION:
        if (g_set.motion_smooth > 60) snprintf(v, size, "%d fps", g_set.motion_smooth);
        else snprintf(v, size, "60 fps (%s)", S(S_OFF));
        break;
    case I_WIDESCREEN: {
        static const char *ws[WS_COUNT] = {NULL, "16:9", "16:10", "21:9"};
        snprintf(v, size, "%s", g_set.widescreen ? ws[g_set.widescreen] : S(S_OFF)); break;
    }
    case I_PIXEL:
        snprintf(v, size, "%s", S(g_set.aspect == ASPECT_PIXEL ? S_PIX_SQUARE : g_set.aspect == ASPECT_TV ? S_PIX_TV : S_PIX_STRETCH));
        break;
    case I_INTEGER: snprintf(v, size, "%s", onoff[g_set.integer_scale]); break;
    case I_SMOOTH: snprintf(v, size, "%s", onoff[g_set.smooth]); break;
    case I_SCANLINES: snprintf(v, size, "%s", onoff[g_set.scanlines]); break;
    case I_SPRLIMIT: snprintf(v, size, "%s", onoff[g_set.sprite_limit]); break;
    case I_SHOWFPS: snprintf(v, size, "%s", onoff[g_set.show_fps]); break;
    case I_VOLUME: snprintf(v, size, "%d%%", g_set.volume); break;
    case I_MUTE: snprintf(v, size, "%s", onoff[g_set.mute]); break;
    case I_MUTE_BG: snprintf(v, size, "%s", onoff[g_set.mute_unfocused]); break;
    case I_PAUSE_BG: snprintf(v, size, "%s", onoff[g_set.pause_unfocused]); break;
    case I_FFSPEED: snprintf(v, size, "%s", g_set.ff_speed == FF_2X ? "2x" : g_set.ff_speed == FF_4X ? "4x" : S(S_MAX)); break;
    default:
        if (id >= I_KEY0 && id <= I_KEY_LAST) key_name(g_set.keys[KEY_ORDER[id - I_KEY0]], v, size);
    }
}

void menu_open(void) {
    g_open = 1; g_page = PG_MAIN; g_sel = 0; g_binding = -1; g_armed = -1; g_status = -1;
    memset(g_page_sel, 0, sizeof g_page_sel);
    plat_clear_keys();
}
void menu_close(void) { pad_setup_stop(); g_open = 0; g_binding = -1; g_armed = -1; }
int menu_is_open(void) { return g_open; }

static int cycle(int v, int n, int d) { return (v + d + n) % n; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void change(int id, int d) {
    switch (id) {
    case I_SLOT: g_set.slot = cycle(g_set.slot - 1, 9, d) + 1; break;
    case I_CONTINUE: g_set.resume = !g_set.resume; break;
    case I_NOSLOW: g_set.no_slowdown = !g_set.no_slowdown; break;
    case I_LANGUAGE: g_set.language = cycle(g_set.language, LANG_COUNT, d); break;
    case I_UPDATES: g_set.check_updates = !g_set.check_updates; break;
    case I_SCALE: g_set.scale = clampi(g_set.scale + d, 1, 6); if (g_set.fullscreen) g_set.fullscreen = 0; plat_apply_display(); break;
    case I_FULLSCREEN: g_set.fullscreen = !g_set.fullscreen; plat_apply_display(); break;
    case I_VSYNC: g_set.vsync = !g_set.vsync; break;
    case I_LOWLAT: g_set.low_latency = !g_set.low_latency; break;
    case I_MOTION: {
        (void)d;
        g_set.motion_smooth = g_set.motion_smooth > 60 ? 60 : 120; break;
    }
    case I_WIDESCREEN: g_set.widescreen = cycle(g_set.widescreen, WS_COUNT, d); plat_apply_display(); break;
    case I_PIXEL: g_set.aspect = cycle(g_set.aspect, ASPECT_COUNT, d); plat_apply_display(); break;
    case I_INTEGER: g_set.integer_scale = !g_set.integer_scale; break;
    case I_SMOOTH: g_set.smooth = !g_set.smooth; break;
    case I_SCANLINES: g_set.scanlines = !g_set.scanlines; break;
    case I_SPRLIMIT: g_set.sprite_limit = !g_set.sprite_limit; hw_set_sprite_limit(g_set.sprite_limit); break;
    case I_SHOWFPS: g_set.show_fps = !g_set.show_fps; break;
    case I_VOLUME: g_set.volume = clampi(g_set.volume + d * 10, 0, 100); break;
    case I_MUTE: g_set.mute = !g_set.mute; break;
    case I_MUTE_BG: g_set.mute_unfocused = !g_set.mute_unfocused; break;
    case I_PAUSE_BG: g_set.pause_unfocused = !g_set.pause_unfocused; break;
    case I_FFSPEED: g_set.ff_speed = cycle(g_set.ff_speed, FF_COUNT, d); break;
    }
}

static void select_row(int i) { if (i != g_sel) { g_armed = -1; g_status = -1; } g_sel = i; }

static void go_page(int page) {
    pad_setup_stop();
    g_page_sel[g_page] = g_sel;
    g_page = page; g_sel = page == PG_MAIN ? g_page_sel[PG_MAIN] : 0;
    g_armed = -1; g_status = -1; g_binding = -1;
}

/* Enter / left click (d = +1) or right click (d = -1) on a row */
static int activate(int id, int d) {
    if (is_option(id)) { change(id, d); return MA_NONE; }
    if (d < 0) return MA_NONE;
    switch (id) {
    case I_RESUME: menu_close(); return MA_RESUME;
    case I_SAVE: menu_close(); return MA_SAVE;
    case I_LOAD: menu_close(); return MA_LOAD;
    case I_SCREENSHOT: menu_close(); return MA_SCREENSHOT;
    case I_PG_VIDEO: go_page(PG_VIDEO); return MA_NONE;
    case I_PG_AUDIO: go_page(PG_AUDIO); return MA_NONE;
    case I_PG_KEYS: go_page(PG_KEYS); return MA_NONE;
    case I_BACK: go_page(PG_MAIN); return MA_NONE;
    case I_RESET:
        if (g_armed == I_RESET) { menu_close(); return MA_RESET; }
        g_armed = I_RESET; return MA_NONE;
    case I_QUIT:
        if (g_armed == I_QUIT) return MA_QUIT;
        g_armed = I_QUIT; return MA_NONE;
    case I_KEYS_DEFAULT: settings_reset_keys(&g_set); pad_reset_layout(); g_status = S_KEYS_WERE_RESET; return MA_NONE;
    case I_PADSETUP:
        if (pad_setup_begin()) g_padstep = 0;
        else g_status = S_PAD_NOPAD;
        return MA_NONE;
    }
    if (id >= I_KEY0 && id <= I_KEY_LAST) g_binding = KEY_ORDER[id - I_KEY0];
    return MA_NONE;
}

static int row_at(int x, int y) {
    for (int i = 0; i < g_row_count; i++)
        if (x >= g_row_rect[i].left && x < g_row_rect[i].right && y >= g_row_rect[i].top && y < g_row_rect[i].bottom)
            return i;
    return -1;
}

void menu_mouse_move(int x, int y) {
    if (!g_open || (x == g_mouse_x && y == g_mouse_y)) return;
    g_mouse_x = x; g_mouse_y = y;
    if (g_binding >= 0 || g_padstep >= 0) return;
    int r = row_at(x, y);
    if (r >= 0) select_row(r);
}

int menu_handle_key(int vk) {
    int items[MAX_ROWS], n = page_items(g_page, items);
    if (g_sel >= n) g_sel = n - 1;
    if (vk == MENU_VK_CLICK_L || vk == MENU_VK_CLICK_R) {
        if (g_binding >= 0 || g_padstep >= 0) return MA_NONE;
        int r = row_at(g_mouse_x, g_mouse_y);
        if (r < 0 || r >= n) return MA_NONE;
        select_row(r);
        return activate(items[r], vk == MENU_VK_CLICK_L ? +1 : -1);
    }
    if (vk == PAD_VK_TAB_PREV || vk == PAD_VK_TAB_NEXT) return MA_NONE;
    int pad = (vk & PAD_VK_BASE) != 0;
    vk &= 0xFFFF;
    if (g_binding >= 0) {
        if (pad) return MA_NONE;
        if (vk != VK_ESCAPE) {
            for (int i = 0; i < BTN_COUNT; i++) if (g_set.keys[i] == vk) g_set.keys[i] = 0;   /* keep keys unique */
            g_set.keys[g_binding] = vk;
        }
        g_binding = -1;
        return MA_NONE;
    }
    if (g_padstep >= 0) {                     /* controller setup: Esc skips a button, Backspace stops */
        if (pad) return MA_NONE;
        if (vk == VK_ESCAPE) pad_step_next();
        else if (vk == VK_BACK) pad_setup_stop();
        else if (vk == VK_F1) { menu_close(); return MA_RESUME; }
        return MA_NONE;
    }
    switch (vk) {
    case VK_UP: select_row((g_sel + n - 1) % n); return MA_NONE;
    case VK_DOWN: select_row((g_sel + 1) % n); return MA_NONE;
    case VK_LEFT: case VK_RIGHT:
        if (is_option(items[g_sel])) change(items[g_sel], vk == VK_LEFT ? -1 : 1);
        else if (vk == VK_RIGHT && is_subpage(items[g_sel])) return activate(items[g_sel], +1);
        else if (vk == VK_LEFT && g_page != PG_MAIN) go_page(PG_MAIN);
        return MA_NONE;
    case VK_ESCAPE: case VK_BACK:
        if (g_page != PG_MAIN) { go_page(PG_MAIN); return MA_NONE; }
        menu_close(); return MA_RESUME;
    case VK_F1:
        menu_close(); return MA_RESUME;
    case VK_RETURN: case VK_SPACE:
        return activate(items[g_sel], +1);
    }
    return MA_NONE;
}

/* ---------------------------------------------------------------- drawing
 * Themed after the Super Famicom cover: a pink sticker-like panel with a thick navy outline,
 * the title in the cover's multicoloured outlined lettering, the selected row on a
 * blue pill (the heroine's cape), all in the embedded "Mochiy Pop P One" font. The panel's
 * shape and translucency are written straight into the 32-bit DIB (premultiplied alpha). */
#define C_PANEL   RGB(248, 178, 211)
#define C_BORDER  RGB(52, 34, 96)
#define C_TEXT    RGB(74, 38, 104)
#define C_VALUE   RGB(196, 34, 120)
#define C_NOTE    RGB(112, 62, 134)
#define C_FOOT    RGB(150, 92, 160)
#define C_BAR     RGB(58, 92, 214)
#define C_WHITE   RGB(255, 255, 255)
#define C_ALERT   RGB(214, 24, 84)
#define C_ALERT_ON_BAR RGB(255, 228, 84)
#define PANEL_ALPHA 218      /* the pink itself; text, outlines and the bar stay solid */

static int g_font_state;     /* 0 = not tried, 1 = embedded font loaded, -1 = unavailable */

static const wchar_t *font_face(void) {
    if (!g_font_state) {
        g_font_state = -1;
        HRSRC r = FindResourceW(NULL, L"MENUFONT", (LPCWSTR)RT_RCDATA);
        HGLOBAL hg = r ? LoadResource(NULL, r) : NULL;
        void *data = hg ? LockResource(hg) : NULL;
        DWORD n = 0;
        if (data && AddFontMemResourceEx(data, SizeofResource(NULL, r), NULL, &n) && n) g_font_state = 1;
    }
    if (g_font_state > 0) return L"Mochiy Pop P One";
    return g_set.language == LANG_JA ? L"Yu Gothic UI" : L"Segoe UI";
}

static HFONT make_font(int px) {
    return CreateFontW(-px, 0, 0, 0, g_font_state > 0 ? FW_NORMAL : FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                       ANTIALIASED_QUALITY, 0, font_face());
}
void *menu_make_font(int px) { return make_font(px); }

static int utf8_to_w(const char *s, wchar_t *w, int cap) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, w, cap);
    return n > 0 ? n - 1 : 0;
}

static void wtext(HDC dc, const char *utf8, int x, int y, COLORREF c) {
    wchar_t w[256]; int n = utf8_to_w(utf8, w, 256);
    SetTextColor(dc, c);
    TextOutW(dc, x, y, w, n);
}

static int text_top(HDC dc, int center) {
    TEXTMETRICW tm; GetTextMetricsW(dc, &tm);
    GLYPHMETRICS gm; static const MAT2 I = {{0, 1}, {0, 0}, {0, 0}, {0, 1}};
    int cap = (tm.tmAscent - tm.tmInternalLeading) * 7 / 10;
    if (GetGlyphOutlineW(dc, L'H', GGO_METRICS, &gm, 0, NULL, &I) != GDI_ERROR) cap = gm.gmptGlyphOrigin.y;
    return center + cap / 2 - tm.tmAscent;
}

static int twidth(HDC dc, const char *utf8) {
    wchar_t w[256]; SIZE sz; int n = utf8_to_w(utf8, w, 256);
    GetTextExtentPoint32W(dc, w, n, &sz);
    return sz.cx;
}

/* text with a thick outline, one colour per character when `cols` is given */
static int outlined(HDC dc, const wchar_t *w, int n, int x, int y, int o, COLORREF outline,
                    const COLORREF *cols, int ncols, COLORREF fillc) {
    static const float C[16][2] = {{1,0},{.92f,.38f},{.71f,.71f},{.38f,.92f},{0,1},{-.38f,.92f},{-.71f,.71f},{-.92f,.38f},
                                   {-1,0},{-.92f,-.38f},{-.71f,-.71f},{-.38f,-.92f},{0,-1},{.38f,-.92f},{.71f,-.71f},{.92f,-.38f}};
    int cx = x, ci = 0;
    for (int i = 0; i < n; i++) {
        SIZE sz; GetTextExtentPoint32W(dc, &w[i], 1, &sz);
        SetTextColor(dc, outline);
        for (int a = 0; a < 16; a++)
            TextOutW(dc, cx + (int)lroundf(C[a][0] * o), y + (int)lroundf(C[a][1] * o), &w[i], 1);
        SetTextColor(dc, cols ? cols[ci % ncols] : fillc);
        TextOutW(dc, cx, y, &w[i], 1);
        if (w[i] != L' ' && w[i] != L'\'') ci++;
        cx += sz.cx;
    }
    return cx - x;
}

/* ---- pixel helpers on the DIB (BGRA); alpha is computed at the end */
typedef struct { uint8_t *px; int w, h; } Canvas;

static float rrect_dist(float x, float y, float x0, float y0, float x1, float y1, float r) {
    float cx = (x0 + x1) * .5f, cy = (y0 + y1) * .5f, hx = (x1 - x0) * .5f - r, hy = (y1 - y0) * .5f - r;
    float dx = fabsf(x - cx) - hx, dy = fabsf(y - cy) - hy;
    float ox = dx > 0 ? dx : 0, oy = dy > 0 ? dy : 0;
    float inside = dx > dy ? dx : dy; if (inside > 0) inside = 0;
    return sqrtf(ox * ox + oy * oy) + inside - r;        /* < 0 inside */
}

static void blend_px(Canvas *c, int x, int y, COLORREF col, float a) {
    if (x < 0 || y < 0 || x >= c->w || y >= c->h || a <= 0) return;
    if (a > 1) a = 1;
    uint8_t *p = c->px + ((size_t)y * c->w + x) * 4;
    p[0] = (uint8_t)(p[0] + (GetBValue(col) - p[0]) * a);
    p[1] = (uint8_t)(p[1] + (GetGValue(col) - p[1]) * a);
    p[2] = (uint8_t)(p[2] + (GetRValue(col) - p[2]) * a);
}

/* filled rounded rectangle, optionally with an outline of width bw, anti-aliased */
static void rrect(Canvas *c, float x0, float y0, float x1, float y1, float r, COLORREF fillc, float bw, COLORREF bc) {
    for (int y = (int)y0 - 1; y <= (int)y1 + 1; y++)
        for (int x = (int)x0 - 1; x <= (int)x1 + 1; x++) {
            float d = rrect_dist(x + .5f, y + .5f, x0, y0, x1, y1, r);
            float cov = .5f - d;                                 /* coverage of the whole shape */
            if (cov <= 0) continue;
            if (bw > 0) {
                blend_px(c, x, y, bc, cov);
                blend_px(c, x, y, fillc, .5f - (d + bw));       /* coverage of the inner fill */
            } else blend_px(c, x, y, fillc, cov);
        }
}

/* one line in a row of height rh at (x, top), shrunk until it fits `avail` pixels */
static void fit_text(HDC dc, const char *utf8, int x, int top, int rh, int avail, int px, COLORREF c) {
    HGDIOBJ old = GetCurrentObject(dc, OBJ_FONT);
    HFONT f = NULL;
    for (;; px--) {
        if (f) { SelectObject(dc, old); DeleteObject(f); }
        f = make_font(px);
        SelectObject(dc, f);
        if (twidth(dc, utf8) <= avail || px <= 7) break;
    }
    wtext(dc, utf8, x, top + text_top(dc, rh / 2), c);
    SelectObject(dc, old); DeleteObject(f);
}

static RECT g_panel;
void menu_panel_rect(void *rect) { *(RECT *)rect = g_panel; }

void menu_draw(void *hdcv, void *bits, int w, int h) {
    HDC dc = (HDC)hdcv;
    Canvas cv = {(uint8_t *)bits, w, h};
    int items[MAX_ROWS], n = page_items(g_page, items);
    SetBkMode(dc, TRANSPARENT);
    font_face();

    /* size: 26 px text on a 900 px tall screen, scaled with the window, shrunk until it fits */
    float scale = h / 900.0f; if (scale < 0.6f) scale = 0.6f;
    int fh = (int)(26 * scale + 0.5f);
    HFONT font = NULL;
    int em = 0, lh = 0, pw = 0, ph = 0, title_h = 0;
    for (;; fh--) {
        if (font) DeleteObject(font);
        font = make_font(fh);
        SelectObject(dc, font);
        em = twidth(dc, "M");
        lh = fh * 150 / 100;
        title_h = fh * 3 + fh / 4;
        pw = em * 38;
        ph = title_h + (int)((n + 2.9f) * lh);
        if ((pw <= w - 8 && ph <= h - 8) || fh <= 9) break;
    }
    if (pw > w - 4) pw = w - 4;
    int pad = fh * 13 / 10, bw = fh / 6 > 2 ? fh / 6 : 2, radius = fh;
    int px = (w - pw) / 2, py = (h - ph) / 2; if (py < 2) py = 2;
    int value_x = px + pad + em * 15;
    int inset = bw + pad / 2;             /* selection bar inside the outline */

    /* panel */
    rrect(&cv, (float)px, (float)py, (float)(px + pw), (float)(py + ph), (float)radius, C_PANEL, (float)bw, C_BORDER);

    /* title: the game's name in the cover's lettering, and the page name on the right */
    {
        int tf = fh * 19 / 10, o = tf / 8 > 2 ? tf / 8 : 2;
        HFONT big = make_font(tf);
        SelectObject(dc, big);
        static const COLORREF LOGO[] = {RGB(238, 58, 74), RGB(255, 196, 30), RGB(64, 186, 84), RGB(56, 118, 232), RGB(255, 132, 36)};
        const wchar_t *logo = g_set.language == LANG_JA ? L"\x30DE\x30B8\x30AB\x30EB\x30DD\x30C3\x30D7\x30F3" : L"Magical Pop'n";
        int tcy = py + bw + (title_h - bw) / 2;
        int ty = text_top(dc, tcy);
        outlined(dc, logo, (int)wcslen(logo), px + pad + o, ty, o, C_BORDER, LOGO, 5, 0);
        SelectObject(dc, font);
        DeleteObject(big);
        static const int TITLE[PG_COUNT] = {S_TITLE_MAIN, S_TITLE_VIDEO, S_TITLE_AUDIO, S_TITLE_KEYS};
        const char *pt = S(TITLE[g_page]);
        wtext(dc, pt, px + pw - pad - twidth(dc, pt), text_top(dc, tcy), C_TEXT);
        rrect(&cv, (float)(px + inset), (float)(py + title_h - 1), (float)(px + pw - inset), (float)(py + title_h + 1), 1.f,
              RGB(214, 140, 182), 0, 0);
    }

    int y = py + title_h + lh / 4;
    int text_dy = text_top(dc, lh / 2);
    g_row_count = n;
    for (int i = 0; i < n; i++, y += lh) {
        int id = items[i], sel = i == g_sel;
        int rx = px + inset, rw = pw - 2 * inset;
        g_row_rect[i].left = rx; g_row_rect[i].top = y; g_row_rect[i].right = rx + rw; g_row_rect[i].bottom = y + lh;
        if (sel) rrect(&cv, (float)rx, y + 2.f, (float)(rx + rw), (float)(y + lh - 2), (lh - 4) * .5f, C_BAR, 0, 0);
        wtext(dc, label_of(id), px + pad, y + text_dy, sel ? C_WHITE : C_TEXT);
        char v[160];
        COLORREF vc = sel ? C_WHITE : C_VALUE;
        if (id >= I_KEY0 && id <= I_KEY_LAST && g_binding == KEY_ORDER[id - I_KEY0]) {
            wtext(dc, S(S_PRESS_KEY), value_x, y + text_dy, sel ? C_ALERT_ON_BAR : C_ALERT);
        } else if (is_option(id) || (id >= I_KEY0 && id <= I_KEY_LAST)) {
            value_of(id, v, sizeof v);
            wtext(dc, v, value_x, y + text_dy, vc);
        } else if (id == I_PADNAME) {
            const char *nm = pad_last_name();
            wtext(dc, nm[0] ? nm : S(S_PAD_NONE), value_x, y + text_dy, nm[0] ? vc : (sel ? C_WHITE : C_FOOT));
        } else if (id == I_PADSETUP && g_padstep >= 0) {
            snprintf(v, sizeof v, "%d / %d   %s", g_padstep + 1, PAD_NSTEPS, S(PAD_STEPS[g_padstep][1]));
            wtext(dc, v, value_x, y + text_dy, sel ? C_ALERT_ON_BAR : C_ALERT);
        } else if ((id == I_QUIT || id == I_RESET) && g_armed == id) {
            wtext(dc, S(id == I_QUIT ? S_PRESS_AGAIN_QUIT : S_PRESS_AGAIN_RESET), value_x, y + text_dy,
                  sel ? C_ALERT_ON_BAR : C_ALERT);
        } else {
            const char *key = id == I_SAVE ? "F5" : id == I_LOAD ? "F9" : id == I_SCREENSHOT ? "F12" : NULL;
            if (key) wtext(dc, key, value_x, y + text_dy, sel ? C_WHITE : C_FOOT);
        }
    }

    /* note + key reminder, smaller */
    HFONT small = make_font(fh * 3 / 4 > 9 ? fh * 3 / 4 : 9);
    SelectObject(dc, small);
    const char *note = NULL;
    COLORREF note_c = C_NOTE;
    char padnote[256];
    if (g_binding >= 0) { note = S(S_HINT_CAPTURE); note_c = C_ALERT; }
    else if (g_padstep >= 0) {
        snprintf(padnote, sizeof padnote, S(S_PAD_HINT), S(PAD_STEPS[g_padstep][1]));
        note = padnote; note_c = C_ALERT;
    }
    else if (g_status >= 0) { note = S(g_status); note_c = C_ALERT; }
    else {
        int id = items[g_sel];
        if (id < (int)(sizeof NOTE / sizeof NOTE[0])) note = NOTE[id][g_set.language];
        if (!note) note = S(g_page == PG_MAIN ? S_HINT_MAIN : S_HINT_SUB);
    }
    y += lh / 4;
    rrect(&cv, (float)(px + inset), (float)(y - 1), (float)(px + pw - inset), (float)(y + 1), 1.f, RGB(214, 140, 182), 0, 0);
    int sl = lh * 8 / 10;
    fit_text(dc, note, px + pad, y + sl / 4, sl, pw - 2 * pad, fh * 3 / 4, note_c);
    fit_text(dc, S(g_page == PG_MAIN ? S_FOOT_MAIN : S_FOOT_SUB), px + pad, y + sl / 4 + sl, sl, pw - 2 * pad, fh * 3 / 4, C_FOOT);
    SelectObject(dc, GetStockObject(SYSTEM_FONT));
    DeleteObject(font); DeleteObject(small);
    GdiFlush();

    /* alpha: the panel's anti-aliased shape; premultiply (only the panel's box is touched:
     * the caller keeps everything else transparent) */
    g_panel.left = px - 1 < 0 ? 0 : px - 1; g_panel.top = py - 1 < 0 ? 0 : py - 1;
    g_panel.right = px + pw + 2 > w ? w : px + pw + 2; g_panel.bottom = py + ph + 2 > h ? h : py + ph + 2;
    for (int yy = g_panel.top; yy < g_panel.bottom; yy++)
        for (int xx = g_panel.left; xx < g_panel.right; xx++) {
            uint8_t *p = cv.px + ((size_t)yy * w + xx) * 4;
            float cov = 0;
            if (xx >= px - 1 && xx <= px + pw + 1 && yy >= py - 1 && yy <= py + ph + 1) {
                cov = .5f - rrect_dist(xx + .5f, yy + .5f, (float)px, (float)py, (float)(px + pw), (float)(py + ph), (float)radius);
                if (cov > 1) cov = 1;
                if (cov < 0) cov = 0;
            }
            int bg = p[2] == GetRValue(C_PANEL) && p[1] == GetGValue(C_PANEL) && p[0] == GetBValue(C_PANEL);
            int a = (int)(cov * (bg ? PANEL_ALPHA : 255) + .5f);
            p[0] = (uint8_t)(p[0] * a / 255); p[1] = (uint8_t)(p[1] * a / 255); p[2] = (uint8_t)(p[2] * a / 255);
            p[3] = (uint8_t)a;
        }
}
