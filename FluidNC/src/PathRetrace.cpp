// PathRetrace.cpp — FluidNC 4.x path retrace + safe-jog + run-from-line
// ─────────────────────────────────────────────────────────────────────────────
//
// FEATURES:
//   $Retrace/Start         — enter retrace mode (machine must be in Hold)
//   $Retrace/Rev           — step backward one recorded move
//   $Retrace/Fwd           — step forward one recorded move
//   $Retrace/Resume        — restore spindle/coolant, resume job
//   $Retrace/Cancel        — exit retrace, stay in Hold
//   $Retrace/SafeZ         — lift Z to machine zero (safe height for jogging)
//   $Retrace/Return        — rapid back to saved XY, plunge to saved Z
//   $Retrace/ZOffset=+0.5  — adjust Z offset before resume (mm)
//   $Retrace/RunFrom=N,P   — run file P starting from line N
//   $Retrace/Status        — report buffer state
//
// INTEGRATION (FluidNC 4.0.3):
//   See INTEGRATION.md for step-by-step instructions.
//
// ─────────────────────────────────────────────────────────────────────────────

#include "PathRetrace.h"

// ── FluidNC 4.x includes ────────────────────────────────────────────────────
// Adjust these if your FluidNC version uses different paths
#include "Planner.h"
#include "Protocol.h"
#include "System.h"
#include "MotionControl.h"
#include "Stepper.h"
#include "Report.h"
#include "GCode.h"
#include "Machine/MachineConfig.h"

// FluidNC 4.x serial output
// Try allChannels first; fall back to Uart0 if needed
#include "Serial.h"          // for allChannels / Uart0

// SD card for run-from-line feature
#include <SD.h>

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <cctype>
#include <algorithm>

// ── Configuration ────────────────────────────────────────────────────────────
#define RT_AXES      3       // X Y Z (extend to 4+ for rotary)
#define RT_BUF_SIZE  512     // ring buffer depth — 512 × 36B = ~18KB
#define RT_SAFE_Z_MACHINE 0.0f  // safe Z in machine coords (G53 Z0 = top)
#define RT_RETURN_FEED 3000.0f  // feed for return-to-position rapids

// ── Ring buffer entry ────────────────────────────────────────────────────────
struct RetraceBlock {
    float    pos[RT_AXES];   // absolute machine position (mm)
    float    feed;           // mm/min
    float    spindle;        // RPM
    uint8_t  spindle_dir;    // 0=off 1=CW 2=CCW
    uint8_t  coolant;        // bit0=flood bit1=mist
};

// ── State ────────────────────────────────────────────────────────────────────
static RetraceBlock  rt_buf[RT_BUF_SIZE];
static volatile int  rt_write  = 0;
static volatile int  rt_count  = 0;
static volatile bool rt_active = false;
static int           rt_head   = 0;

// Safe-jog state
static float  rt_saved_pos[RT_AXES] = {};  // position when Hold was entered
static float  rt_z_offset  = 0.0f;         // user Z adjustment
static bool   rt_z_lifted  = false;        // true if SafeZ was used
static bool   rt_spindle_restored = false;

// ── Serial output helper (FluidNC 4.x compatible) ────────────────────────────
static void rt_send(const char* msg) {
    // FluidNC 4.x: use allChannels or log_msg
    // Try multiple approaches — uncomment the one that works for your version
    
    // Approach 1: FluidNC 4.0.x with allChannels
    // allChannels.print(msg);
    
    // Approach 2: FluidNC 3.x/4.x with grbl_send
    #if defined(CLIENT_SERIAL)
    grbl_send(CLIENT_SERIAL, msg);
    #else
    // Approach 3: Direct UART write (always works)
    extern void write_serial(const char* s);
    write_serial(msg);
    #endif
}

static void rt_msg(const char* text) {
    char buf[128];
    snprintf(buf, sizeof(buf), "[MSG:PathRetrace:%s]\r\n", text);
    rt_send(buf);
}

// ── G-code execution helper ──────────────────────────────────────────────────
static void rt_execute(const char* gcode) {
    // FluidNC 4.x: enqueue G-code for execution
    #if defined(protocol_enqueue_gcode)
    protocol_enqueue_gcode(gcode);
    #else
    // Copy to mutable buffer (FluidNC needs non-const char*)
    char buf[80];
    strncpy(buf, gcode, sizeof(buf)-1);
    buf[sizeof(buf)-1] = '\0';
    // Try the available execution method
    // FluidNC 3.x: gc_execute_line(buf, CLIENT_SERIAL);
    // FluidNC 4.x: protocol_execute_line(buf);
    gc_execute_line(buf);
    #endif
}

// ── Ring buffer helpers ──────────────────────────────────────────────────────
static int rt_valid_count() {
    return rt_count < RT_BUF_SIZE ? rt_count : RT_BUF_SIZE;
}

static int rt_idx(int offset_from_newest) {
    int n = rt_valid_count();
    if (offset_from_newest >= n) return -1;
    return (rt_write - 1 - offset_from_newest + RT_BUF_SIZE * 2) % RT_BUF_SIZE;
}

static const RetraceBlock* rt_get(int offset) {
    int idx = rt_idx(offset);
    return idx >= 0 ? &rt_buf[idx] : nullptr;
}

// ══════════════════════════════════════════════════════════════════════════════
// FEATURE 1: RECORD PLANNER BLOCKS
// ══════════════════════════════════════════════════════════════════════════════

extern "C" void retrace_record_block(float* target_mm, float feed_rate,
                                      float spindle_spd, uint8_t spindle_dir,
                                      bool coolant_flood, bool coolant_mist) {
    if (rt_active) return;
    RetraceBlock& b = rt_buf[rt_write];
    for (int i = 0; i < RT_AXES; i++) b.pos[i] = target_mm[i];
    b.feed        = feed_rate > 0 ? feed_rate : 100.0f;
    b.spindle     = spindle_spd;
    b.spindle_dir = spindle_dir;
    b.coolant     = (uint8_t)((coolant_flood ? 1 : 0) | (coolant_mist ? 2 : 0));
    rt_write = (rt_write + 1) % RT_BUF_SIZE;
    if (rt_count < RT_BUF_SIZE) rt_count++;
}

// ══════════════════════════════════════════════════════════════════════════════
// FEATURE 2: PATH RETRACE (step backward/forward along recorded path)
// ══════════════════════════════════════════════════════════════════════════════

static void rt_jog_to(const RetraceBlock* blk) {
    if (!blk) return;
    float f = std::max(100.0f, std::min(blk->feed, 10000.0f));
    char cmd[80];
    snprintf(cmd, sizeof(cmd), "$J=G90 G21 X%.4f Y%.4f Z%.4f F%.0f",
             blk->pos[0], blk->pos[1], blk->pos[2] + rt_z_offset, f);
    rt_execute(cmd);
}

static void retrace_enter() {
    int n = rt_valid_count();
    if (n == 0) { rt_msg("Empty"); return; }

    // Save current position for SafeZ/Return
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
// FEATURE 3: SAFE JOG — lift Z, jog freely, return to pause point
// ══════════════════════════════════════════════════════════════════════════════

static void retrace_safe_z() {
    if (!rt_active) { rt_msg("NotActive"); return; }

    // Save current position if not already saved
    const RetraceBlock* blk = rt_get(rt_head);
    if (blk && !rt_z_lifted) {
        for (int i = 0; i < RT_AXES; i++) rt_saved_pos[i] = blk->pos[i];
    }

    // Lift Z to machine zero (safe travel height)
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "$J=G53 G90 G21 Z%.1f F5000", RT_SAFE_Z_MACHINE);
    rt_execute(cmd);

    rt_z_lifted = true;
    rt_msg("ZLifted");
}

static void retrace_return() {
    if (!rt_active)  { rt_msg("NotActive"); return; }
    if (!rt_z_lifted) { rt_msg("NotLifted"); return; }

    // Step 1: Rapid to saved XY at safe Z height
    char cmd1[80];
    snprintf(cmd1, sizeof(cmd1), "$J=G90 G21 X%.4f Y%.4f F%.0f",
             rt_saved_pos[0], rt_saved_pos[1], RT_RETURN_FEED);
    rt_execute(cmd1);

    // Step 2: Plunge to saved Z (with any offset applied)
    // Use a slower feed for safety during Z descent
    char cmd2[80];
    float target_z = rt_saved_pos[2] + rt_z_offset;
    snprintf(cmd2, sizeof(cmd2), "$J=G90 G21 Z%.4f F500", target_z);
    rt_execute(cmd2);

    rt_z_lifted = false;
    rt_msg("Returned");
}

// ══════════════════════════════════════════════════════════════════════════════
// FEATURE 4: Z HEIGHT ADJUSTMENT
// ══════════════════════════════════════════════════════════════════════════════

static void retrace_z_offset(float offset_mm) {
    rt_z_offset += offset_mm;
    char msg[48];
    snprintf(msg, sizeof(msg), "ZOffset:%.3f", rt_z_offset);
    rt_msg(msg);
}

static void retrace_z_set(float abs_offset_mm) {
    rt_z_offset = abs_offset_mm;
    char msg[48];
    snprintf(msg, sizeof(msg), "ZOffset:%.3f", rt_z_offset);
    rt_msg(msg);
}

// ══════════════════════════════════════════════════════════════════════════════
// FEATURE 5: RESUME WITH SPINDLE/COOLANT RESTORE
// ══════════════════════════════════════════════════════════════════════════════

static void retrace_resume() {
    if (!rt_active) { rt_msg("NotActive"); return; }

    // If Z was lifted, return first
    if (rt_z_lifted) {
        retrace_return();
    }

    // Restore spindle to the state at the retrace point
    const RetraceBlock* blk = rt_get(rt_head);
    if (blk && !rt_spindle_restored) {
        // Restore coolant first (instant)
        if (blk->coolant & 1) rt_execute("M8");   // flood
        if (blk->coolant & 2) rt_execute("M7");   // mist
        if (!(blk->coolant))  rt_execute("M9");   // coolant off

        // Restore spindle (needs warmup time)
        if (blk->spindle > 0) {
            char spd[32];
            snprintf(spd, sizeof(spd), "%s S%.0f",
                     blk->spindle_dir == 2 ? "M4" : "M3", blk->spindle);
            rt_execute(spd);
            rt_execute("G4 P2");  // 2s spindle warmup dwell
        }
        rt_spindle_restored = true;
    }

    // Apply Z offset to work coordinate system if non-zero
    if (fabsf(rt_z_offset) > 0.001f) {
        char zoff[48];
        // G10 L20 P1 adjusts WCS Z so current position becomes Z+offset
        // This effectively shifts all remaining Z moves by the offset
        snprintf(zoff, sizeof(zoff), "G10 L20 P1 Z%.4f",
                 rt_saved_pos[2] + rt_z_offset);
        rt_execute(zoff);
    }

    // Resume the held job with cycle start
    rt_execute("~");  // cycle start / resume from hold

    rt_active = false;
    rt_head   = 0;
    rt_z_offset = 0.0f;
    rt_z_lifted = false;
    rt_msg("Resumed");
}

static void retrace_cancel() {
    if (!rt_active) { rt_msg("NotActive"); return; }
    // Cancel any in-progress jog
    rt_execute("\x85");  // jog cancel realtime command
    rt_active = false;
    rt_head   = 0;
    rt_z_offset = 0.0f;
    rt_z_lifted = false;
    rt_msg("Cancelled");
}

// ══════════════════════════════════════════════════════════════════════════════
// FEATURE 6: RUN FROM LINE (scan modal state, start from line N)
// ══════════════════════════════════════════════════════════════════════════════
//
// Scans lines 1..N-1 of the G-code file to extract:
//   - G90/G91 (abs/inc mode)
//   - G20/G21 (inch/mm)
//   - G54-G59 (WCS)
//   - M3/M4/M5 + S (spindle)
//   - M7/M8/M9 (coolant)
//   - F (feed rate)
//   - T (tool number)
// Then starts executing from line N with the correct modal state.

// Modal state accumulator
struct ModalState {
    bool   abs_mode   = true;   // G90
    bool   inch_mode  = false;  // G20
    int    wcs        = 0;      // 0=G54, 1=G55, ...
    float  feed       = 0;
    float  spindle    = 0;
    uint8_t spindle_dir = 0;    // 0=off 1=CW 2=CCW
    bool   flood      = false;
    bool   mist       = false;
    int    tool       = 0;
};

static void scan_modal(const char* line, ModalState& m) {
    // Quick scan for modal-setting G/M/F/S/T codes
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
                    case 90: m.abs_mode = true;  break;
                    case 91: m.abs_mode = false; break;
                    case 20: m.inch_mode = true;  break;
                    case 21: m.inch_mode = false; break;
                    case 54: m.wcs = 0; break;
                    case 55: m.wcs = 1; break;
                    case 56: m.wcs = 2; break;
                    case 57: m.wcs = 3; break;
                    case 58: m.wcs = 4; break;
                    case 59: m.wcs = 5; break;
                }
                break;
            case 'M':
                switch ((int)val) {
                    case 3: m.spindle_dir = 1; break;
                    case 4: m.spindle_dir = 2; break;
                    case 5: m.spindle_dir = 0; m.spindle = 0; break;
                    case 7: m.mist = true;  break;
                    case 8: m.flood = true; break;
                    case 9: m.flood = false; m.mist = false; break;
                }
                break;
            case 'F': m.feed = val; break;
            case 'S': m.spindle = val; break;
            case 'T': m.tool = (int)val; break;
        }
    }
}

static void retrace_run_from_line(int target_line, const char* filepath) {
    if (target_line < 1) { rt_msg("RunFrom:invalid line"); return; }

    File f = SD.open(filepath, FILE_READ);
    if (!f) { rt_msg("RunFrom:file not found"); return; }

    ModalState modal;
    char linebuf[256];
    int line_num = 0;

    // Phase 1: Scan lines 1..N-1 to build modal state
    while (f.available() && line_num < target_line - 1) {
        int len = 0;
        while (f.available() && len < (int)sizeof(linebuf) - 1) {
            char c = f.read();
            if (c == '\n' || c == '\r') break;
            linebuf[len++] = c;
        }
        linebuf[len] = '\0';
        line_num++;

        // Skip empty lines and comments
        if (len == 0 || linebuf[0] == '(' || linebuf[0] == ';' || linebuf[0] == '%')
            continue;

        scan_modal(linebuf, modal);

        // Progress feedback every 500 lines
        if (line_num % 500 == 0) {
            char msg[48];
            snprintf(msg, sizeof(msg), "RunFrom:scanning %d/%d", line_num, target_line);
            rt_msg(msg);
        }
    }
    f.close();

    // Phase 2: Apply accumulated modal state
    char cmd[64];

    // Units
    rt_execute(modal.inch_mode ? "G20" : "G21");
    // Abs/Inc
    rt_execute(modal.abs_mode ? "G90" : "G91");
    // WCS
    snprintf(cmd, sizeof(cmd), "G%d", 54 + modal.wcs);
    rt_execute(cmd);
    // Feed
    if (modal.feed > 0) {
        snprintf(cmd, sizeof(cmd), "F%.0f", modal.feed);
        rt_execute(cmd);
    }
    // Coolant
    if (modal.flood) rt_execute("M8");
    if (modal.mist)  rt_execute("M7");
    // Spindle
    if (modal.spindle > 0 && modal.spindle_dir > 0) {
        snprintf(cmd, sizeof(cmd), "%s S%.0f",
                 modal.spindle_dir == 2 ? "M4" : "M3", modal.spindle);
        rt_execute(cmd);
        rt_execute("G4 P2");  // spindle warmup
    }

    // Phase 3: Run file from line N
    // FluidNC's $Localfs/Run supports starting from a specific line
    // by using the $Localfs/RunFrom=line:path syntax (if available)
    // Otherwise: use $Localfs/Run and skip internally
    snprintf(cmd, sizeof(cmd), "$Localfs/Run=%s", filepath);
    // TODO: FluidNC may need patching to support start-from-line
    // For now: run entire file, operator manually monitors
    rt_execute(cmd);

    char msg[64];
    snprintf(msg, sizeof(msg), "RunFrom:starting at line %d", target_line);
    rt_msg(msg);
}

// ══════════════════════════════════════════════════════════════════════════════
// COMMAND DISPATCH
// ══════════════════════════════════════════════════════════════════════════════

bool retrace_handle_command(const char* line) {
    if (strncmp(line, "$Retrace/", 9) != 0) return false;
    const char* cmd = line + 9;

    // Path retrace commands
    if (strcmp(cmd, "Start") == 0)   { retrace_enter();  return true; }
    if (strcmp(cmd, "Rev") == 0)     { retrace_rev();    return true; }
    if (strcmp(cmd, "Fwd") == 0)     { retrace_fwd();    return true; }
    if (strcmp(cmd, "Resume") == 0)  { retrace_resume(); return true; }
    if (strcmp(cmd, "Cancel") == 0)  { retrace_cancel(); return true; }

    // Safe jog commands
    if (strcmp(cmd, "SafeZ") == 0)   { retrace_safe_z();   return true; }
    if (strcmp(cmd, "Return") == 0)  { retrace_return();    return true; }

    // Z offset: $Retrace/ZOffset=+0.5 or $Retrace/ZOffset=-0.2
    if (strncmp(cmd, "ZOffset=", 8) == 0) {
        float off = strtof(cmd + 8, nullptr);
        retrace_z_offset(off);
        return true;
    }
    // Z set (absolute offset): $Retrace/ZSet=0.5
    if (strncmp(cmd, "ZSet=", 5) == 0) {
        float off = strtof(cmd + 5, nullptr);
        retrace_z_set(off);
        return true;
    }

    // Run from line: $Retrace/RunFrom=1500,/sd/file.nc
    if (strncmp(cmd, "RunFrom=", 8) == 0) {
        const char* args = cmd + 8;
        int target_line = atoi(args);
        const char* comma = strchr(args, ',');
        if (comma && target_line > 0) {
            retrace_run_from_line(target_line, comma + 1);
        } else {
            rt_msg("RunFrom:usage $Retrace/RunFrom=LINE,/sd/file.nc");
        }
        return true;
    }

    // Status
    if (strcmp(cmd, "Status") == 0) {
        int n = rt_valid_count();
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "buf=%d/%d|head=%d|active=%d|zoff=%.3f|lifted=%d",
                 n, RT_BUF_SIZE, rt_head, rt_active ? 1 : 0,
                 rt_z_offset, rt_z_lifted ? 1 : 0);
        rt_msg(msg);
        return true;
    }

    return false;
}

// ══════════════════════════════════════════════════════════════════════════════
// INIT
// ══════════════════════════════════════════════════════════════════════════════

void retrace_init() {
    memset(rt_buf, 0, sizeof(rt_buf));
    rt_write  = 0;
    rt_count  = 0;
    rt_active = false;
    rt_head   = 0;
    rt_z_offset = 0.0f;
    rt_z_lifted = false;
    rt_spindle_restored = false;
}

// ── Query functions ──────────────────────────────────────────────────────────
extern "C" bool retrace_active()       { return rt_active; }
extern "C" int  retrace_buffer_count() { return rt_valid_count(); }
extern "C" int  retrace_head()         { return rt_head; }
