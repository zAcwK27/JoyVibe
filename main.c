#include <stdio.h>
#include <string.h>

#include <switch.h>

/* User-facing aliases for split Joy-Con npad slots (libnx HidNpadIdType). */
#define HID_PAD_ID_PLAYER1   HidNpadIdType_No1
#define HID_PAD_ID_PLAYER2   HidNpadIdType_No2
#define HID_PAD_ID_HANDHELD  HidNpadIdType_Handheld

#define STEP_PERCENT 5
#define FREQ_MIN_HZ  10.0f
#define FREQ_MAX_HZ  500.0f
#define BAR_WIDTH    40
#define FLASH_FRAMES 12

#define LEFT_INPUT_MASK  ( \
    HidNpadButton_Up | HidNpadButton_Down | HidNpadButton_Left | HidNpadButton_Right | \
    HidNpadButton_L | HidNpadButton_ZL | HidNpadButton_LeftSL | HidNpadButton_LeftSR)

#define RIGHT_INPUT_MASK ( \
    HidNpadButton_A | HidNpadButton_B | HidNpadButton_X | HidNpadButton_Y | \
    HidNpadButton_R | HidNpadButton_ZR | HidNpadButton_RightSL | HidNpadButton_RightSR)

typedef enum {
    PARAM_AMP_LOW = 0,
    PARAM_AMP_HIGH,
    PARAM_FREQ_LOW,
    PARAM_FREQ_HIGH,
    PARAM_COUNT
} ParamId;

typedef struct {
    int amp_low;
    int amp_high;
    int freq_low;
    int freq_high;
} VibeParams;

typedef struct {
    VibeParams params;
    u64 prev_buttons;
    bool connected;
    HidNpadIdType active_id;
    int flash_timer[PARAM_COUNT];
} JoyConSide;

typedef struct {
    HidVibrationDeviceHandle handheld[2];
    HidVibrationDeviceHandle player1[2];
    HidVibrationDeviceHandle split_left;
    HidVibrationDeviceHandle split_right;
    bool handheld_ok;
    bool player1_ok;
    bool split_left_ok;
    bool split_right_ok;
} VibrationDevices;

static JoyConSide g_left;
static JoyConSide g_right;
static VibrationDevices g_vibe;
static PadState g_pad;

static bool g_hiddbg_ok = false;
static bool g_vibe_session_ok = false;
static u32 g_last_vibe_error = 0;
static u64 g_debug_left_down = 0;
static u64 g_debug_right_down = 0;

static void clamp_params(VibeParams *p) {
    if (p->amp_low < 0) p->amp_low = 0;
    if (p->amp_low > 100) p->amp_low = 100;
    if (p->amp_high < 0) p->amp_high = 0;
    if (p->amp_high > 100) p->amp_high = 100;
    if (p->freq_low < 0) p->freq_low = 0;
    if (p->freq_low > 100) p->freq_low = 100;
    if (p->freq_high < 0) p->freq_high = 0;
    if (p->freq_high > 100) p->freq_high = 100;
}

static void adjust_param(int *value, int delta) {
    *value += delta;
    if (*value < 0) *value = 0;
    if (*value > 100) *value = 100;
}

static void flash_param(JoyConSide *side, ParamId param) {
    side->flash_timer[param] = FLASH_FRAMES;
}

static void tick_flash_timers(JoyConSide *side) {
    for (int i = 0; i < PARAM_COUNT; i++) {
        if (side->flash_timer[i] > 0) {
            side->flash_timer[i]--;
        }
    }
}

static float percent_to_amp(int percent) {
    return (float)percent / 100.0f;
}

static float percent_to_freq_hz(int percent) {
    return FREQ_MIN_HZ + ((float)percent / 100.0f) * (FREQ_MAX_HZ - FREQ_MIN_HZ);
}

static HidVibrationValue make_stop_value(void) {
    HidVibrationValue v;
    memset(&v, 0, sizeof(v));
    v.freq_low = 160.0f;
    v.freq_high = 320.0f;
    return v;
}

static HidVibrationValue params_to_vibration(const VibeParams *p) {
    HidVibrationValue v;

    if (p->amp_low == 0 && p->amp_high == 0) {
        return make_stop_value();
    }

    memset(&v, 0, sizeof(v));
    v.amp_low = percent_to_amp(p->amp_low);
    v.amp_high = percent_to_amp(p->amp_high);
    v.freq_low = percent_to_freq_hz(p->freq_low);
    v.freq_high = percent_to_freq_hz(p->freq_high);

    if (v.amp_low > 0.0f && v.freq_low < FREQ_MIN_HZ) {
        v.freq_low = FREQ_MIN_HZ;
    }
    if (v.amp_high > 0.0f && v.freq_high < FREQ_MIN_HZ) {
        v.freq_high = FREQ_MIN_HZ;
    }

    return v;
}

static bool params_is_active(const VibeParams *p) {
    return p->amp_low > 0 || p->amp_high > 0;
}

static Result hiddbg_set_vibration(HidVibrationDeviceHandle handle, const HidVibrationValue *value) {
    return hidSendVibrationValue(handle, value);
}

static Result send_vibration_checked(HidVibrationDeviceHandle handle, const HidVibrationValue *value) {
    Result rc = hiddbg_set_vibration(handle, value);
    if (R_FAILED(rc)) {
        g_last_vibe_error = rc;
    }
    return rc;
}

static void process_left_buttons(JoyConSide *side, u64 down) {
    VibeParams *p = &side->params;

    if (down & HidNpadButton_Up) {
        adjust_param(&p->amp_low, STEP_PERCENT);
        flash_param(side, PARAM_AMP_LOW);
    }
    if (down & HidNpadButton_Down) {
        adjust_param(&p->amp_high, STEP_PERCENT);
        flash_param(side, PARAM_AMP_HIGH);
    }
    if (down & HidNpadButton_Left) {
        adjust_param(&p->freq_low, STEP_PERCENT);
        flash_param(side, PARAM_FREQ_LOW);
    }
    if (down & HidNpadButton_Right) {
        adjust_param(&p->freq_high, STEP_PERCENT);
        flash_param(side, PARAM_FREQ_HIGH);
    }
    if (down & HidNpadButton_LeftSR) {
        adjust_param(&p->amp_high, -STEP_PERCENT);
        flash_param(side, PARAM_AMP_HIGH);
    }
    if (down & HidNpadButton_LeftSL) {
        adjust_param(&p->amp_low, -STEP_PERCENT);
        flash_param(side, PARAM_AMP_LOW);
    }
    if (down & HidNpadButton_L) {
        adjust_param(&p->freq_low, -STEP_PERCENT);
        flash_param(side, PARAM_FREQ_LOW);
    }
    if (down & HidNpadButton_ZL) {
        adjust_param(&p->freq_high, -STEP_PERCENT);
        flash_param(side, PARAM_FREQ_HIGH);
    }

    clamp_params(p);
}

static void process_right_buttons(JoyConSide *side, u64 down) {
    VibeParams *p = &side->params;

    if (down & HidNpadButton_X) {
        adjust_param(&p->amp_low, STEP_PERCENT);
        flash_param(side, PARAM_AMP_LOW);
    }
    if (down & HidNpadButton_B) {
        adjust_param(&p->amp_high, STEP_PERCENT);
        flash_param(side, PARAM_AMP_HIGH);
    }
    if (down & HidNpadButton_Y) {
        adjust_param(&p->freq_low, STEP_PERCENT);
        flash_param(side, PARAM_FREQ_LOW);
    }
    if (down & HidNpadButton_A) {
        adjust_param(&p->freq_high, STEP_PERCENT);
        flash_param(side, PARAM_FREQ_HIGH);
    }
    if (down & HidNpadButton_RightSL) {
        adjust_param(&p->amp_high, -STEP_PERCENT);
        flash_param(side, PARAM_AMP_HIGH);
    }
    if (down & HidNpadButton_RightSR) {
        adjust_param(&p->amp_low, -STEP_PERCENT);
        flash_param(side, PARAM_AMP_LOW);
    }
    if (down & HidNpadButton_R) {
        adjust_param(&p->freq_low, -STEP_PERCENT);
        flash_param(side, PARAM_FREQ_LOW);
    }
    if (down & HidNpadButton_ZR) {
        adjust_param(&p->freq_high, -STEP_PERCENT);
        flash_param(side, PARAM_FREQ_HIGH);
    }

    clamp_params(p);
}

static u64 buttons_edge(u64 current, u64 previous) {
    return current & ~previous;
}

/* Read Joy-Con Left state (HID_PAD_ID_JOYCON_LEFT equivalent). */
static bool read_joycon_left(HidNpadIdType id, u64 *out_buttons) {
    u32 style = hidGetNpadStyleSet(id);
    HidNpadJoyLeftState state;

    if (!(style & HidNpadStyleTag_NpadJoyLeft)) {
        return false;
    }

    if (hidGetNpadStatesJoyLeft(id, &state, 1) > 0) {
        *out_buttons = state.buttons;
        return true;
    }

    return false;
}

/* Read Joy-Con Right state (HID_PAD_ID_JOYCON_RIGHT equivalent). */
static bool read_joycon_right(HidNpadIdType id, u64 *out_buttons) {
    u32 style = hidGetNpadStyleSet(id);
    HidNpadJoyRightState state;

    if (!(style & HidNpadStyleTag_NpadJoyRight)) {
        return false;
    }

    if (hidGetNpadStatesJoyRight(id, &state, 1) > 0) {
        *out_buttons = state.buttons;
        return true;
    }

    return false;
}

static bool scan_left_joycon(u64 *out_buttons, HidNpadIdType *out_id) {
    static const HidNpadIdType ids[] = {
        HID_PAD_ID_PLAYER1,
        HID_PAD_ID_PLAYER2,
        HID_PAD_ID_HANDHELD,
    };

    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        if (read_joycon_left(ids[i], out_buttons)) {
            *out_id = ids[i];
            return true;
        }
    }

    return false;
}

static bool scan_right_joycon(u64 *out_buttons, HidNpadIdType *out_id) {
    static const HidNpadIdType ids[] = {
        HID_PAD_ID_PLAYER1,
        HID_PAD_ID_PLAYER2,
        HID_PAD_ID_HANDHELD,
    };

    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        if (read_joycon_right(ids[i], out_buttons)) {
            *out_id = ids[i];
            return true;
        }
    }

    return false;
}

static void read_split_inputs(u64 *out_left, u64 *out_right, bool *out_left_ok, bool *out_right_ok,
                              HidNpadIdType *out_left_id, HidNpadIdType *out_right_id) {
    u64 left_buttons = 0;
    u64 right_buttons = 0;
    bool left_ok = false;
    bool right_ok = false;
    HidNpadIdType left_id = HID_PAD_ID_PLAYER1;
    HidNpadIdType right_id = HID_PAD_ID_PLAYER1;

    padUpdate(&g_pad);

    const u64 pad_buttons = padGetButtons(&g_pad);

    left_ok = scan_left_joycon(&left_buttons, &left_id);
    right_ok = scan_right_joycon(&right_buttons, &right_id);

    if (pad_buttons & LEFT_INPUT_MASK) {
        if (left_ok) {
            left_buttons |= pad_buttons & LEFT_INPUT_MASK;
        } else {
            left_buttons = pad_buttons & LEFT_INPUT_MASK;
            left_ok = true;
            left_id = HID_PAD_ID_PLAYER1;
        }
    }

    if (pad_buttons & RIGHT_INPUT_MASK) {
        if (right_ok) {
            right_buttons |= pad_buttons & RIGHT_INPUT_MASK;
        } else {
            right_buttons = pad_buttons & RIGHT_INPUT_MASK;
            right_ok = true;
            right_id = HID_PAD_ID_PLAYER1;
        }
    }

    *out_left = left_buttons;
    *out_right = right_buttons;
    *out_left_ok = left_ok;
    *out_right_ok = right_ok;
    *out_left_id = left_id;
    *out_right_id = right_id;
}

static u64 read_global_buttons_down(void) {
    return padGetButtonsDown(&g_pad);
}

static void reset_all_params(void) {
    memset(&g_left.params, 0, sizeof(g_left.params));
    memset(&g_right.params, 0, sizeof(g_right.params));
}

static bool init_vibration_devices(void) {
    Result rc;

    memset(&g_vibe, 0, sizeof(g_vibe));

    rc = hidInitializeVibrationDevices(g_vibe.handheld, 2, HID_PAD_ID_HANDHELD,
                                       HidNpadStyleTag_NpadHandheld);
    g_vibe.handheld_ok = R_SUCCEEDED(rc);

    rc = hidInitializeVibrationDevices(g_vibe.player1, 2, HID_PAD_ID_PLAYER1,
                                       HidNpadStyleTag_NpadJoyDual);
    g_vibe.player1_ok = R_SUCCEEDED(rc);

    rc = hidInitializeVibrationDevices(&g_vibe.split_left, 1, HID_PAD_ID_PLAYER1,
                                       HidNpadStyleTag_NpadJoyLeft);
    g_vibe.split_left_ok = R_SUCCEEDED(rc);

    rc = hidInitializeVibrationDevices(&g_vibe.split_right, 1, HID_PAD_ID_PLAYER1,
                                       HidNpadStyleTag_NpadJoyRight);
    g_vibe.split_right_ok = R_SUCCEEDED(rc);

    if (!g_vibe.split_left_ok) {
        rc = hidInitializeVibrationDevices(&g_vibe.split_left, 1, HID_PAD_ID_PLAYER2,
                                           HidNpadStyleTag_NpadJoyLeft);
        g_vibe.split_left_ok = R_SUCCEEDED(rc);
    }

    if (!g_vibe.split_right_ok) {
        rc = hidInitializeVibrationDevices(&g_vibe.split_right, 1, HID_PAD_ID_PLAYER2,
                                           HidNpadStyleTag_NpadJoyRight);
        g_vibe.split_right_ok = R_SUCCEEDED(rc);
    }

    return g_vibe.handheld_ok || g_vibe.player1_ok || g_vibe.split_left_ok || g_vibe.split_right_ok;
}

static void send_side_vibration(bool use_handheld, bool is_left, const VibeParams *params) {
    HidVibrationDeviceHandle handle = {0};
    bool ready = false;
    HidVibrationValue value;

    value = params_to_vibration(params);

    if (use_handheld && g_vibe.handheld_ok) {
        handle = is_left ? g_vibe.handheld[0] : g_vibe.handheld[1];
        ready = true;
    } else if (g_vibe.split_left_ok && g_vibe.split_right_ok) {
        handle = is_left ? g_vibe.split_left : g_vibe.split_right;
        ready = true;
    } else if (g_vibe.player1_ok) {
        handle = is_left ? g_vibe.player1[0] : g_vibe.player1[1];
        ready = true;
    } else if (g_vibe.handheld_ok) {
        handle = is_left ? g_vibe.handheld[0] : g_vibe.handheld[1];
        ready = true;
    }

    if (ready && g_vibe_session_ok) {
        send_vibration_checked(handle, &value);
    }
}

static void send_vibrations(void) {
    if (!g_vibe_session_ok) {
        return;
    }

    const bool use_handheld = padIsHandheld(&g_pad);

    send_side_vibration(use_handheld, true, &g_left.params);
    send_side_vibration(use_handheld, false, &g_right.params);
}

static void stop_all_vibrations(void) {
    HidVibrationValue stop = make_stop_value();

    if (g_vibe.handheld_ok) {
        send_vibration_checked(g_vibe.handheld[0], &stop);
        send_vibration_checked(g_vibe.handheld[1], &stop);
    }
    if (g_vibe.player1_ok) {
        send_vibration_checked(g_vibe.player1[0], &stop);
        send_vibration_checked(g_vibe.player1[1], &stop);
    }
    if (g_vibe.split_left_ok) {
        send_vibration_checked(g_vibe.split_left, &stop);
    }
    if (g_vibe.split_right_ok) {
        send_vibration_checked(g_vibe.split_right, &stop);
    }
}

static void print_bar(int percent, bool highlight) {
    int filled = (BAR_WIDTH * percent) / 100;
    if (filled < 0) filled = 0;
    if (filled > BAR_WIDTH) filled = BAR_WIDTH;

    if (highlight) {
        printf(CONSOLE_YELLOW);
    }

    printf("[");
    for (int i = 0; i < BAR_WIDTH; i++) {
        putchar(i < filled ? '=' : '-');
    }
    printf("]");

    if (highlight) {
        printf(CONSOLE_RESET);
    }
}

static void print_param_line(JoyConSide *side, ParamId param, const char *label, int percent) {
    const bool highlight = side->flash_timer[param] > 0;

    if (highlight) {
        printf(CONSOLE_YELLOW);
    }

    printf("%-10s %3d%% ", label, percent);

    if (highlight) {
        printf(CONSOLE_RESET);
    }

    print_bar(percent, highlight);
    printf("\n");
}

static const char *npad_id_name(HidNpadIdType id) {
    switch (id) {
    case HidNpadIdType_No1: return "P1";
    case HidNpadIdType_No2: return "P2";
    case HidNpadIdType_Handheld: return "HH";
    default: return "--";
    }
}

static void draw_ui(void) {
    consoleClear();

    printf(CONSOLE_WHITE "                                   JoyVibe\n" CONSOLE_RESET);
    printf("\n");

    printf(CONSOLE_BLUE "LEFT JOY-CON  [%s %s]\n" CONSOLE_RESET,
           g_left.connected ? "linked" : "none",
           npad_id_name(g_left.active_id));
    print_param_line(&g_left, PARAM_AMP_LOW, "amp_low:", g_left.params.amp_low);
    print_param_line(&g_left, PARAM_AMP_HIGH, "amp_high:", g_left.params.amp_high);
    print_param_line(&g_left, PARAM_FREQ_LOW, "freq_low:", g_left.params.freq_low);
    print_param_line(&g_left, PARAM_FREQ_HIGH, "freq_high:", g_left.params.freq_high);
    printf("\n");

    printf(CONSOLE_RED "RIGHT JOY-CON [%s %s]\n" CONSOLE_RESET,
           g_right.connected ? "linked" : "none",
           npad_id_name(g_right.active_id));
    print_param_line(&g_right, PARAM_AMP_LOW, "amp_low:", g_right.params.amp_low);
    print_param_line(&g_right, PARAM_AMP_HIGH, "amp_high:", g_right.params.amp_high);
    print_param_line(&g_right, PARAM_FREQ_LOW, "freq_low:", g_right.params.freq_low);
    print_param_line(&g_right, PARAM_FREQ_HIGH, "freq_high:", g_right.params.freq_high);
    printf("\n");

    printf(CONSOLE_WHITE "CONTROLS GUIDE\n" CONSOLE_RESET);
    printf("\n");
    printf(CONSOLE_BLUE " Left Joy-Con:\n" CONSOLE_RESET);
    printf("Up:     +5%% amp_low    | SL: -5%% amp_low\n");
    printf("Down:   +5%% amp_high   | SR: -5%% amp_high\n");
    printf("Left:   +5%% freq_low   | L:  -5%% freq_low\n");
    printf("Right:  +5%% freq_high  | ZL: -5%% freq_high\n");
    printf("\n");
    printf(CONSOLE_RED " Right Joy-Con:\n" CONSOLE_RESET);
    printf("X:      +5%% amp_low    | SR: -5%% amp_low\n");
    printf("B:      +5%% amp_high   | SL: -5%% amp_high\n");
    printf("Y:      +5%% freq_low   | R:  -5%% freq_low\n");
    printf("A:      +5%% freq_high  | ZR: -5%% freq_high\n");

    if (g_last_vibe_error != 0) {
        printf("\n");
        printf(CONSOLE_YELLOW "Last vibration error: 0x%x\n" CONSOLE_RESET, g_last_vibe_error);
    }

    printf("\n");
    const bool active = params_is_active(&g_left.params) || params_is_active(&g_right.params);
    if (active) {
        printf(CONSOLE_GREEN "Vibration: Active\n" CONSOLE_RESET);
    } else {
        printf("Vibration: Stopped\n");
    }
    printf("\n");
    printf("+ exit   - reset all\n");
}

static bool init_app(void) {
    Result rc;
    const HidNpadIdType supported_ids[] = {
        HID_PAD_ID_PLAYER1,
        HID_PAD_ID_PLAYER2,
        HID_PAD_ID_HANDHELD,
    };

    PrintConsole *console = consoleInit(NULL);
    if (console == NULL) {
        return false;
    }

    consoleSetWindow(console, 1, 1, 78, 36);

    hidInitializeNpad();
    hidSetSupportedNpadIdType(supported_ids, sizeof(supported_ids) / sizeof(supported_ids[0]));

    padConfigureInput(8, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);

    rc = hiddbgInitialize();
    g_hiddbg_ok = R_SUCCEEDED(rc);

    rc = hidBeginPermitVibrationSession();
    g_vibe_session_ok = R_SUCCEEDED(rc);

    if (!init_vibration_devices()) {
        return false;
    }

    return true;
}

static void exit_app(void) {
    stop_all_vibrations();

    if (g_vibe_session_ok) {
        hidEndPermitVibrationSession();
    }

    if (g_hiddbg_ok) {
        hiddbgExit();
    }

    consoleExit(NULL);
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    if (!init_app()) {
        return 1;
    }

    while (appletMainLoop()) {
        u64 left_buttons = 0;
        u64 right_buttons = 0;
        bool left_ok = false;
        bool right_ok = false;
        HidNpadIdType left_id = HID_PAD_ID_PLAYER1;
        HidNpadIdType right_id = HID_PAD_ID_PLAYER1;

        read_split_inputs(&left_buttons, &right_buttons, &left_ok, &right_ok, &left_id, &right_id);

        g_left.connected = left_ok;
        g_right.connected = right_ok;
        g_left.active_id = left_id;
        g_right.active_id = right_id;

        const u64 left_down = left_ok ? buttons_edge(left_buttons, g_left.prev_buttons) : 0;
        const u64 right_down = right_ok ? buttons_edge(right_buttons, g_right.prev_buttons) : 0;

        g_debug_left_down = left_down;
        g_debug_right_down = right_down;

        const u64 global_down = read_global_buttons_down();

        if (global_down & HidNpadButton_Plus) {
            break;
        }
        if (global_down & HidNpadButton_Minus) {
            reset_all_params();
        }

        process_left_buttons(&g_left, left_down);
        process_right_buttons(&g_right, right_down);

        tick_flash_timers(&g_left);
        tick_flash_timers(&g_right);

        send_vibrations();
        draw_ui();
        
        g_left.prev_buttons = left_buttons;
        g_right.prev_buttons = right_buttons;

        consoleUpdate(NULL);
    }

    exit_app();
    return 0;
}