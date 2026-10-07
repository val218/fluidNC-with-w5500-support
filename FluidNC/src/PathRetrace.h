// PathRetrace.h — FluidNC 4.x plugin: path rewind, safe-jog, run-from-line
// ─────────────────────────────────────────────────────────────────────────────
// Drop into FluidNC/FluidNC/src/ and add to CMakeLists.txt
//
// FEATURES:
//   1. PATH RETRACE — step backward/forward along recorded toolpath
//   2. SAFE JOG     — lift Z, jog anywhere, return to pause point, resume
//   3. Z ADJUST     — offset Z height before resuming
//   4. RUN FROM LINE — start job from any G-code line (scans modal state)
//
// Ring buffer records every planner block's XYZ + feed + spindle + coolant.
// Default 512 blocks × 36 bytes = 18KB.
#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// ── Recording (called from Planner) ──────────────────────────────────────────
void retrace_record_block(float* target_mm,
                          float   feed_rate,
                          float   spindle_spd,
                          uint8_t spindle_dir,
                          bool    coolant_flood,
                          bool    coolant_mist);

// ── Query functions ──────────────────────────────────────────────────────────
bool retrace_active();
int  retrace_buffer_count();
int  retrace_head();

#ifdef __cplusplus
}

// ── Command handler (C++ only) ───────────────────────────────────────────────
bool retrace_handle_command(const char* line);
void retrace_init();

#endif
