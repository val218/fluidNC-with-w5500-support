// VizGenerator.cpp — FluidNC plugin: toolpath viz file generator
// ─────────────────────────────────────────────────────────────────────────────
// Scans a G-code file on SD card. Extracts XY motion coordinates.
// Writes a compact .viz CSV file that the pendant can load via $File/ShowSome.
//
// Runs as a background FreeRTOS task on Core 1, priority 1 (below motion).
// Memory: ~4KB stack + 256B line buffer. No heap allocation.
// ─────────────────────────────────────────────────────────────────────────────

#include "VizGenerator.h"

// FluidNC includes — adjust to match your version
#include "Report.h"
#include "Machine/MachineConfig.h"
#include "System.h"

#include <SD.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cctype>
#include <algorithm>

// ── Configuration ────────────────────────────────────────────────────────────
#define VIZ_MAX_POINTS    8000   // max points in output
#define VIZ_SAMPLE_EVERY  3      // sample every N motion lines
#define VIZ_MIN_DIST_MM   0.5f   // skip points closer than this
#define VIZ_ARC_SEGMENTS  16     // segments per G2/G3 arc

// ── State ────────────────────────────────────────────────────────────────────
static bool  _viz_busy   = false;
static float _modal_x = 0, _modal_y = 0, _modal_z = 0;
static bool  _modal_abs  = true;
static bool  _modal_inch = false;

// ── Helpers ──────────────────────────────────────────────────────────────────
static float to_mm(float v) { return _modal_inch ? v * 25.4f : v; }

static bool parse_float(const char** p, float* out) {
    while (**p == ' ' || **p == '\t') (*p)++;
    if (**p == '\0') return false;
    char* end;
    *out = strtof(*p, &end);
    if (end == *p) return false;
    *p = end;
    return true;
}

static const char* skip_to_letter(const char* p) {
    while (*p && !isalpha(*p)) p++;
    return p;
}

// ── Send message to pendant ──────────────────────────────────────────────────
static void viz_msg(const char* msg) {
    char buf[128];
    snprintf(buf, sizeof(buf), "[MSG:%s]\r\n", msg);
#ifdef GRBL_SEND_AVAILABLE
    grbl_send(CLIENT_SERIAL, buf);
#else
    log_msg(buf);
#endif
}

// ── Arc expansion ────────────────────────────────────────────────────────────
static int write_arc(File& f, float x0, float y0, float x1, float y1,
                     float i, float j, bool clockwise,
                     float* xmin, float* xmax, float* ymin, float* ymax,
                     int* count) {
    float cx = x0 + i, cy = y0 + j;
    float r = sqrtf(i*i + j*j);
    if (r < 0.001f) return 0;

    float a0 = atan2f(y0 - cy, x0 - cx);
    float a1 = atan2f(y1 - cy, x1 - cx);
    if (clockwise) { if (a1 >= a0) a1 -= 2.0f * M_PI; }
    else           { if (a1 <= a0) a1 += 2.0f * M_PI; }

    int written = 0;
    for (int s = 1; s <= VIZ_ARC_SEGMENTS && *count < VIZ_MAX_POINTS; s++) {
        float t   = (float)s / VIZ_ARC_SEGMENTS;
        float ang = a0 + t * (a1 - a0);
        float px  = cx + r * cosf(ang);
        float py  = cy + r * sinf(ang);
        char buf[32]; snprintf(buf, sizeof(buf), "%.3f,%.3f\n", px, py);
        f.print(buf);
        if (px < *xmin) *xmin = px; if (px > *xmax) *xmax = px;
        if (py < *ymin) *ymin = py; if (py > *ymax) *ymax = py;
        (*count)++; written++;
    }
    return written;
}

// ── Core: scan G-code and write .viz ─────────────────────────────────────────
static bool do_generate(const std::string& nc_path, const std::string& viz_out) {
    File nc = SD.open(nc_path.c_str(), FILE_READ);
    if (!nc) { viz_msg("VizErr:cannot open nc file"); return false; }

    std::string tmp = viz_out + ".tmp";
    SD.remove(tmp.c_str());
    File vf = SD.open(tmp.c_str(), FILE_WRITE);
    if (!vf) { nc.close(); viz_msg("VizErr:cannot write viz file"); return false; }

    _modal_x = 0; _modal_y = 0; _modal_z = 0;
    _modal_abs = true; _modal_inch = false;

    float xmin = 1e9f, xmax = -1e9f, ymin = 1e9f, ymax = -1e9f;
    int n_points = 0, line_num = 0, sample_ct = 0;
    float last_px = 1e9f, last_py = 1e9f;

    // Placeholder header — rewritten at end
    vf.println("VIZ 00000 +00000.000 +00000.000 +00000.000 +00000.000");

    char linebuf[256];
    while (nc.available() && n_points < VIZ_MAX_POINTS) {
        int len = 0;
        while (nc.available() && len < (int)sizeof(linebuf) - 1) {
            char c = nc.read();
            if (c == '\n' || c == '\r') break;
            linebuf[len++] = toupper(c);
        }
        linebuf[len] = '\0';
        line_num++;

        if (line_num % 1000 == 0) {
            char msg[64];
            snprintf(msg, sizeof(msg), "VizBusy:%s:%d",
                     nc_path.c_str(), n_points * 100 / VIZ_MAX_POINTS);
            viz_msg(msg);
        }

        char* p = linebuf;
        if (*p == 'N') { while (*p && !isspace(*p)) p++; }

        float new_x = _modal_x, new_y = _modal_y, new_z = _modal_z;
        float arc_i = 0, arc_j = 0;
        int motion = -1;
        bool has_x = false, has_y = false;

        const char* scan = p;
        while (*scan) {
            scan = skip_to_letter(scan);
            if (!*scan) break;
            char letter = *scan++;
            float val;
            if (!parse_float(&scan, &val)) continue;
            switch (letter) {
                case 'G':
                    switch ((int)val) {
                        case 0: motion=0; break; case 1: motion=1; break;
                        case 2: motion=2; break; case 3: motion=3; break;
                        case 20: _modal_inch=true; break; case 21: _modal_inch=false; break;
                        case 90: _modal_abs=true;  break; case 91: _modal_abs=false;  break;
                    } break;
                case 'X': new_x = _modal_abs ? to_mm(val) : _modal_x + to_mm(val); has_x=true; break;
                case 'Y': new_y = _modal_abs ? to_mm(val) : _modal_y + to_mm(val); has_y=true; break;
                case 'Z': new_z = _modal_abs ? to_mm(val) : _modal_z + to_mm(val); break;
                case 'I': arc_i = to_mm(val); break;
                case 'J': arc_j = to_mm(val); break;
            }
        }

        if (motion < 0 || (!has_x && !has_y)) {
            _modal_x = new_x; _modal_y = new_y; _modal_z = new_z;
            continue;
        }

        if (motion == 2 || motion == 3) {
            write_arc(vf, _modal_x, _modal_y, new_x, new_y,
                      arc_i, arc_j, motion == 2,
                      &xmin, &xmax, &ymin, &ymax, &n_points);
        } else {
            float dx = new_x - last_px, dy = new_y - last_py;
            if (sqrtf(dx*dx + dy*dy) >= VIZ_MIN_DIST_MM || last_px > 1e8f) {
                if (++sample_ct >= VIZ_SAMPLE_EVERY || last_px > 1e8f) {
                    sample_ct = 0;
                    char buf[32]; snprintf(buf, sizeof(buf), "%.3f,%.3f\n", new_x, new_y);
                    vf.print(buf);
                    if (new_x < xmin) xmin = new_x; if (new_x > xmax) xmax = new_x;
                    if (new_y < ymin) ymin = new_y; if (new_y > ymax) ymax = new_y;
                    last_px = new_x; last_py = new_y;
                    n_points++;
                }
            }
        }
        _modal_x = new_x; _modal_y = new_y; _modal_z = new_z;
    }
    nc.close(); vf.close();

    // Rewrite with real header: read temp, write final with proper header
    File vf_r = SD.open(tmp.c_str(), FILE_READ);
    SD.remove(viz_out.c_str());
    File vf_w = SD.open(viz_out.c_str(), FILE_WRITE);
    if (!vf_r || !vf_w) {
        if (vf_r) vf_r.close(); if (vf_w) vf_w.close();
        SD.remove(tmp.c_str()); return false;
    }

    char hdr[64];
    snprintf(hdr, sizeof(hdr), "VIZ %d %.3f %.3f %.3f %.3f\n",
             n_points, xmin, xmax, ymin, ymax);
    vf_w.print(hdr);

    // Skip placeholder header
    while (vf_r.available()) { if (vf_r.read() == '\n') break; }

    char copybuf[256];
    while (vf_r.available()) {
        int n = vf_r.read((uint8_t*)copybuf, sizeof(copybuf));
        if (n > 0) vf_w.write((uint8_t*)copybuf, n);
    }
    vf_r.close(); vf_w.close();
    SD.remove(tmp.c_str());

    char done_msg[128];
    snprintf(done_msg, sizeof(done_msg), "VizReady:%s:%d:%.3f:%.3f:%.3f:%.3f",
             viz_out.c_str(), n_points, xmin, xmax, ymin, ymax);
    viz_msg(done_msg);
    return true;
}

// ── FreeRTOS background task ─────────────────────────────────────────────────
struct VizTaskArgs { char nc_path[256]; char viz_out[256]; };
static VizTaskArgs _viz_task_args;

static void viz_task(void* arg) {
    VizTaskArgs* a = (VizTaskArgs*)arg;
    _viz_busy = true;
    do_generate(a->nc_path, a->viz_out);
    _viz_busy = false;
    vTaskDelete(nullptr);
}

// ── Public API ───────────────────────────────────────────────────────────────
std::string viz_path(const std::string& nc_path) { return nc_path + ".viz"; }

bool viz_exists(const std::string& nc_path) {
    return SD.exists(viz_path(nc_path).c_str());
}

bool viz_generate(const std::string& nc_path) {
    if (_viz_busy) { viz_msg("VizBusy:already generating"); return false; }
    std::string vp = viz_path(nc_path);
    if (viz_exists(nc_path)) {
        char msg[128];
        snprintf(msg, sizeof(msg), "VizReady:%s", vp.c_str());
        viz_msg(msg);
        return true;
    }
    strncpy(_viz_task_args.nc_path, nc_path.c_str(), sizeof(_viz_task_args.nc_path)-1);
    strncpy(_viz_task_args.viz_out, vp.c_str(), sizeof(_viz_task_args.viz_out)-1);
    xTaskCreatePinnedToCore(viz_task, "viz_gen", 8192, &_viz_task_args, 1, nullptr, 1);
    return true;
}

bool viz_handle_command(const char* line) {
    if (strncmp(line, "$Viz/", 5) != 0) return false;
    const char* cmd = line + 5;
    if (strncmp(cmd, "Generate=", 9) == 0) { viz_generate(cmd + 9); return true; }
    if (strncmp(cmd, "Delete=", 7) == 0) {
        SD.remove(viz_path(cmd + 7).c_str());
        viz_msg("VizDeleted"); return true;
    }
    if (strncmp(cmd, "Status=", 7) == 0) {
        bool exists = viz_exists(cmd + 7);
        char msg[128];
        snprintf(msg, sizeof(msg), "VizStatus:%s:%s", cmd + 7, exists ? "ready" : "missing");
        viz_msg(msg); return true;
    }
    return false;
}

void viz_init() { _viz_busy = false; }
