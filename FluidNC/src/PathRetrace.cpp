// PathRetrace.cpp — FluidNC 4.0.3 path retrace + safe-jog + run-from-line
// ─────────────────────────────────────────────────────────────────────────────
#include "PathRetrace.h"

#include "Planner.h"
#include "Protocol.h"
#include "System.h"
#include "MotionControl.h"
#include "Stepper.h"
#include "Report.h"
#include "GCode.h"
#include "Machine/MachineConfig.h"
#include "Serial.h"            // allChannels
#include "RealtimeCmd.h"       // Cmd::JogCancel
#include "Settings.h"          // execute_line
#include "SpindleDatatypes.h"  // SpindleState
#include "Channel.h"

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <cctype>
#include <algorithm>
#include <string>

// ── Configuration ────────────────────────────────────────────────────────────
#define RT_AXES      3
#define RT_BUF_SIZE  512
#define RT_SAFE_Z    0.0f    // machine coordinate for safe Z (G53 Z0 = top)
#define RT_RETURN_F  3000.0f // mm/min for return rapids

// ── Ring buffer entry ────────────────────────────────────────────────────────
struct RetraceBlock {
    float    pos[RT_AXES];
    float    feed;
    float    spindle_rpm;
    uint8_t  spindle_dir;   // SpindleState as uint8_t (3=CW, 4=CCW, 5=off)
    uint8_t  coolant;       // bit0=Flood, bit1=Mist
};

// ── State ────────────────────────────────────────────────────────────────────
static RetraceBlock  rt_buf[RT_BUF_SIZE];
static volatile int  rt_write  = 0;
static volatile int  rt_count  = 0;
static volatile bool rt_active = false;
static int           rt_head   = 0;

static float  rt_saved_pos[RT_AXES] = {};
static float  rt_z_offset   = 0.0f;
static bool   rt_z_lifted   = false;
static bool   rt_spindle_restored = false;

// ── Serial output (FluidNC 4.0.3) ───────────────────────────────────────────
static void rt_send(const char* msg) {
    allChannels.print(msg);
}

static void rt_msg(const char* text) {
    char buf[128];
    snprintf(buf, sizeof(buf), "[MSG:PathRetrace:%s]\r\n", text);
    rt_send(buf);
}

// ── G-code execution (FluidNC 4.0.3) ────────────────────────────────────────
static void rt_gcode(const char* gcode) {
    // gc_execute_line expects a non-const char* in some FluidNC versions
    Error err = gc_execute_line(gcode);
    (void)err;
}

static void rt_dollar(const char* cmd) {
    // Execute a $ command via the settings handler
    // Use Uart0 channel as the response destination
    execute_line(cmd, allChannels, AuthenticationLevel::LEVEL_GUEST);
}

// ── Ring buffer helpers ──────────────────────────────────────────────────────
static int rt_valid_count() {
    return rt_count < RT_BUF_SIZE ? rt_count : RT_BUF_SIZE;
}

static int rt_idx(int offset) {
    int n = rt_valid_count();
    if (offset >= n) return -1;
    return (rt_write - 1 - offset + RT_BUF_SIZE * 2) % RT_BUF_SIZE;
}

static const RetraceBlock* rt_get(int offset) {
    int idx = rt_idx(offset);
    return idx >= 0 ? &rt_buf[idx] : nullptr;
}

// ══════════════════════════════════════════════════════════════════════════════
// RECORD PLANNER BLOCKS
// ══════════════════════════════════════════════════════════════════════════════

extern "C" void retrace_record_block(float* target_mm, float feed_rate,
                                      float spindle_spd, uint8_t spindle_dir,
                                      bool coolant_flood, bool coolant_mist) {
    if (rt_active) return;
    RetraceBlock& b = rt_buf[rt_write];
    for (int i = 0; i < RT_AXES; i++) b.pos[i] = target_mm[i];
    b.feed        = feed_rate > 0 ? feed_rate : 100.0f;
    b.spindle_rpm = spindle_spd;
    b.spindle_dir = spindle_dir;
    b.coolant     = (uint8_t)((coolant_flood ? 1 : 0) | (coolant_mist ? 2 : 0));
    rt_write = (rt_write + 1) % RT_BUF_SIZE;
    if (rt_count < RT_BUF_SIZE) rt_count++;
}

// ══════════════════════════════════════════════════════════════════════════════
// PATH RETRACE
// ══════════════════════════════════════════════════════════════════════════════

static void rt_jog_to(const RetraceBlock* blk) {
    if (!blk) return;
    float f = std::max(100.0f, std::min(blk->feed, 10000.0f));
    char cmd[80];
    snprintf(cmd, sizeof(cmd), "$J=G90 G21 X%.4f Y%.4f Z%.4f F%.0f",
             blk->pos[0], blk->pos[1], blk->pos[2] + rt_z_offset, f);
    rt_dollar(cmd);
}

static void retrace_enter() {
    int n = rt_valid_count();
    if (n == 0) { rt_msg("Empty"); return; }

    const RetraceBlock* latest = rt_get(0);
    if (latest) {
        for (int i = 0; i < RT_AXES; i++) rt_saved_pos[i] = latest->pos[i];
    }

    rt_active = true;
    rt_head   = 0;
    rt_z_offset = 0.0f;
    rt_z_lifted = false;
    rt_spindle_restored = false;

    char msg[48];
    snprintf(msg, sizeof(msg), "Active|%d/%d", rt_head, n);
    rt_msg(msg);
}

static void retrace_rev() {
    if (!rt_active) { rt_msg("NotActive"); return; }
    int n = rt_valid_count();
    if (rt_head >= n - 1) { rt_msg("AtStart"); return; }
    rt_head++;
    rt_jog_to(rt_get(rt_head));
    char msg[32]; snprintf(msg, sizeof(msg), "%d/%d", rt_head, n);
    rt_msg(msg);
}

static void retrace_fwd() {
    if (!rt_active) { rt_msg("NotActive"); return; }
    if (rt_head <= 0) { rt_msg("AtCurrent"); return; }
    rt_head--;
    rt_jog_to(rt_get(rt_head));
    int n = rt_valid_count();
    char msg[32]; snprintf(msg, sizeof(msg), "%d/%d", rt_head, n);
    rt_msg(msg);
}

// ══════════════════════════════════════════════════════════════════════════════
// SAFE JOG — lift Z, jog freely, return to pause point
// ══════════════════════════════════════════════════════════════════════════════

static void retrace_safe_z() {
    if (!rt_active) { rt_msg("NotActive"); return; }
    const RetraceBlock* blk = rt_get(rt_head);
    if (blk && !rt_z_lifted) {
        for (int i = 0; i < RT_AXES; i++) rt_saved_pos[i] = blk->pos[i];
    }
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "$J=G53 G90 G21 Z%.1f F5000", RT_SAFE_Z);
    rt_dollar(cmd);
    rt_z_lifted = true;
    rt_msg("ZLifted");
}

static void retrace_return() {
    if (!rt_active)   { rt_msg("NotActive"); return; }
    if (!rt_z_lifted) { rt_msg("NotLifted"); return; }
    // Rapid to saved XY
    char cmd1[80];
    snprintf(cmd1, sizeof(cmd1), "$J=G90 G21 X%.4f Y%.4f F%.0f",
             rt_saved_pos[0], rt_saved_pos[1], RT_RETURN_F);
    rt_dollar(cmd1);
    // Plunge to saved Z + offset
    char cmd2[80];
    snprintf(cmd2, sizeof(cmd2), "$J=G90 G21 Z%.4f F500",
             rt_saved_pos[2] + rt_z_offset);
    rt_dollar(cmd2);
    rt_z_lifted = false;
    rt_msg("Returned");
}

// ══════════════════════════════════════════════════════════════════════════════
// Z HEIGHT ADJUSTMENT
// ══════════════════════════════════════════════════════════════════════════════

static void retrace_z_offset(float offset_mm) {
    rt_z_offset += offset_mm;
    char msg[32]; snprintf(msg, sizeof(msg), "ZOffset:%.3f", rt_z_offset);
    rt_msg(msg);
}

static void retrace_z_set(float abs_mm) {
    rt_z_offset = abs_mm;
    char msg[32]; snprintf(msg, sizeof(msg), "ZOffset:%.3f", rt_z_offset);
    rt_msg(msg);
}

// ══════════════════════════════════════════════════════════════════════════════
// RESUME WITH SPINDLE/COOLANT RESTORE
// ══════════════════════════════════════════════════════════════════════════════

static void retrace_resume() {
    if (!rt_active) { rt_msg("NotActive"); return; }
    if (rt_z_lifted) retrace_return();

    const RetraceBlock* blk = rt_get(rt_head);
    if (blk && !rt_spindle_restored) {
        // Coolant
        if (blk->coolant & 1) rt_gcode("M8");
        if (blk->coolant & 2) rt_gcode("M7");
        if (!blk->coolant)    rt_gcode("M9");
        // Spindle
        if (blk->spindle_rpm > 0 && blk->spindle_dir != (uint8_t)SpindleState::Disable) {
            char spd[32];
            snprintf(spd, sizeof(spd), "%s S%.0f",
                     blk->spindle_dir == (uint8_t)SpindleState::Ccw ? "M4" : "M3",
                     blk->spindle_rpm);
            rt_gcode(spd);
            rt_gcode("G4 P2");  // 2s warmup
        }
        rt_spindle_restored = true;
    }

    // Apply Z offset to WCS
    if (fabsf(rt_z_offset) > 0.001f) {
        char zoff[48];
        snprintf(zoff, sizeof(zoff), "G10 L20 P1 Z%.4f",
                 rt_saved_pos[2] + rt_z_offset);
        rt_gcode(zoff);
    }

    // Resume from hold
    protocol_send_event(&cycleStartEvent);

    rt_active = false;
    rt_head   = 0;
    rt_z_offset = 0.0f;
    rt_z_lifted = false;
    rt_msg("Resumed");
}

static void retrace_cancel() {
    if (!rt_active) { rt_msg("NotActive"); return; }
    execute_realtime_command(Cmd::JogCancel, allChannels);
    rt_active = false;
    rt_head   = 0;
    rt_z_offset = 0.0f;
    rt_z_lifted = false;
    rt_msg("Cancelled");
}

// ══════════════════════════════════════════════════════════════════════════════
// RUN FROM LINE (scan modal state, start from line N)
// ══════════════════════════════════════════════════════════════════════════════

struct ModalState {
    bool   abs_mode   = true;
    bool   inch_mode  = false;
    int    wcs        = 0;
    float  feed       = 0;
    float  spindle    = 0;
    uint8_t spindle_dir = 0;
    bool   flood      = false;
    bool   mist       = false;
    int    tool       = 0;
};

static void scan_modal(const char* line, ModalState& m) {
    const char* p = line;
    while (*p) {
        while (*p && !isalpha(*p)) p++;
        if (!*p) break;
        char letter = toupper(*p++);
        char* end;
        float val = strtof(p, &end);
        if (end == p) { p++; continue; }
        p = end;
        switch (letter) {
            case 'G':
                switch ((int)val) {
                    case 90: m.abs_mode=true;  break; case 91: m.abs_mode=false; break;
                    case 20: m.inch_mode=true;  break; case 21: m.inch_mode=false; break;
                    case 54: m.wcs=0; break; case 55: m.wcs=1; break;
                    case 56: m.wcs=2; break; case 57: m.wcs=3; break;
                    case 58: m.wcs=4; break; case 59: m.wcs=5; break;
                } break;
            case 'M':
                switch ((int)val) {
                    case 3: m.spindle_dir=1; break; case 4: m.spindle_dir=2; break;
                    case 5: m.spindle_dir=0; m.spindle=0; break;
                    case 7: m.mist=true;  break; case 8: m.flood=true; break;
                    case 9: m.flood=false; m.mist=false; break;
                } break;
            case 'F': m.feed = val; break;
            case 'S': m.spindle = val; break;
            case 'T': m.tool = (int)val; break;
        }
    }
}

static void retrace_run_from_line(int target_line, const char* filepath) {
    if (target_line < 1) { rt_msg("RunFrom:invalid line"); return; }

    // Use FluidNC's file system - try localfs path
    FILE* f = fopen(filepath, "r");
    if (!f) {
        // Try with /sd prefix
        std::string sdpath = std::string("/sd") + filepath;
        f = fopen(sdpath.c_str(), "r");
    }
    if (!f) { rt_msg("RunFrom:file not found"); return; }

    ModalState modal;
    char linebuf[256];
    int line_num = 0;

    while (fgets(linebuf, sizeof(linebuf), f) && line_num < target_line - 1) {
        line_num++;
        if (linebuf[0] == '(' || linebuf[0] == ';' || linebuf[0] == '%' || linebuf[0] == '\n')
            continue;
        scan_modal(linebuf, modal);
        if (line_num % 500 == 0) {
            char msg[48];
            snprintf(msg, sizeof(msg), "RunFrom:scanning %d/%d", line_num, target_line);
            rt_msg(msg);
        }
    }
    fclose(f);

    // Apply modal state
    rt_gcode(modal.inch_mode ? "G20" : "G21");
    rt_gcode(modal.abs_mode  ? "G90" : "G91");
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "G%d", 54 + modal.wcs); rt_gcode(cmd);
    if (modal.feed > 0) { snprintf(cmd, sizeof(cmd), "F%.0f", modal.feed); rt_gcode(cmd); }
    if (modal.flood) rt_gcode("M8");
    if (modal.mist)  rt_gcode("M7");
    if (modal.spindle > 0 && modal.spindle_dir > 0) {
        snprintf(cmd, sizeof(cmd), "%s S%.0f",
                 modal.spindle_dir == 2 ? "M4" : "M3", modal.spindle);
        rt_gcode(cmd);
        rt_gcode("G4 P2");
    }

    // Start file (FluidNC runs it via $Localfs/Run)
    snprintf(cmd, sizeof(cmd), "$Localfs/Run=%s", filepath);
    rt_dollar(cmd);

    char msg[64];
    snprintf(msg, sizeof(msg), "RunFrom:started line %d", target_line);
    rt_msg(msg);
}

// ══════════════════════════════════════════════════════════════════════════════
// COMMAND DISPATCH
// ══════════════════════════════════════════════════════════════════════════════

bool retrace_handle_command(const char* line) {
    if (strncmp(line, "$Retrace/", 9) != 0) return false;
    const char* cmd = line + 9;

    if (strcmp(cmd, "Start") == 0)   { retrace_enter();  return true; }
    if (strcmp(cmd, "Rev") == 0)     { retrace_rev();    return true; }
    if (strcmp(cmd, "Fwd") == 0)     { retrace_fwd();    return true; }
    if (strcmp(cmd, "Resume") == 0)  { retrace_resume(); return true; }
    if (strcmp(cmd, "Cancel") == 0)  { retrace_cancel(); return true; }
    if (strcmp(cmd, "SafeZ") == 0)   { retrace_safe_z(); return true; }
    if (strcmp(cmd, "Return") == 0)  { retrace_return(); return true; }

    if (strncmp(cmd, "ZOffset=", 8) == 0) {
        retrace_z_offset(strtof(cmd + 8, nullptr)); return true;
    }
    if (strncmp(cmd, "ZSet=", 5) == 0) {
        retrace_z_set(strtof(cmd + 5, nullptr)); return true;
    }
    if (strncmp(cmd, "RunFrom=", 8) == 0) {
        const char* args = cmd + 8;
        int line_num = atoi(args);
        const char* comma = strchr(args, ',');
        if (comma && line_num > 0) {
            retrace_run_from_line(line_num, comma + 1);
        } else {
            rt_msg("RunFrom:usage $Retrace/RunFrom=LINE,/path/file.nc");
        }
        return true;
    }
    if (strcmp(cmd, "Status") == 0) {
        int n = rt_valid_count();
        char msg[96];
        snprintf(msg, sizeof(msg), "buf=%d/%d|head=%d|active=%d|zoff=%.3f|lifted=%d",
                 n, RT_BUF_SIZE, rt_head, rt_active ? 1 : 0, rt_z_offset, rt_z_lifted ? 1 : 0);
        rt_msg(msg);
        return true;
    }
    return false;
}

// ══════════════════════════════════════════════════════════════════════════════
// INIT + QUERY
// ══════════════════════════════════════════════════════════════════════════════

void retrace_init() {
    memset(rt_buf, 0, sizeof(rt_buf));
    rt_write = 0; rt_count = 0; rt_active = false; rt_head = 0;
    rt_z_offset = 0.0f; rt_z_lifted = false; rt_spindle_restored = false;
}

extern "C" bool retrace_active()       { return rt_active; }
extern "C" int  retrace_buffer_count() { return rt_valid_count(); }
extern "C" int  retrace_head()         { return rt_head; }
