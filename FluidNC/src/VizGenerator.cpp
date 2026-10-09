// VizGenerator.cpp — FluidNC 4.0.3: toolpath viz file generator
// ─────────────────────────────────────────────────────────────────────────────
// Scans a G-code file, extracts XY motion, writes a compact .viz CSV file.
// Pendant loads it via $File/ShowSome — no new protocol needed.
// Runs as a background FreeRTOS task, priority 1 (below motion).
// ─────────────────────────────────────────────────────────────────────────────

#include "VizGenerator.h"
#include "Serial.h"     // allChannels
#include "System.h"
#include "Driver/watchdog.h"  // feed_watchdog
#include "State.h"            // state_is
#include "Job.h"              // Job::active
#include "PendantLink.h"      // pendant_channel
#include "UartChannel.h"
#include "FluidPath.h"        // keeps the SD card mounted while we use it

#include <Arduino.h>  // millis
#include <atomic>
#include <mutex>
#include <vector>

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <cctype>
#include <algorithm>
#include <string>
#include <cerrno>

#define VIZ_MAX_POINTS    8000
#define VIZ_SAMPLE_EVERY  3
#define VIZ_MIN_DIST_MM   0.5f
#define VIZ_ARC_SEGMENTS  16

static bool  _viz_busy   = false;
// Thinning, scaled per file so large files are covered end to end instead of
// stopping at VIZ_MAX_POINTS part-way through.
static int   _sample_every = VIZ_SAMPLE_EVERY;
static int   _arc_segments = VIZ_ARC_SEGMENTS;
static float _modal_x = 0, _modal_y = 0, _modal_z = 0;
static bool  _modal_abs  = true;
static int   _modal_motion = -1;  // G0/G1/G2/G3 stays active on following lines
static bool  _modal_inch = false;

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

// Builds started from the upload queue (not by a $Viz/Generate request) report
// as VizAutoBusy/VizAutoReady/VizAutoErr and skip the pendant UART: the pendant
// loads whatever file a "VizReady:" names, which would hijack its preview.
static bool _auto_mode = false;

static void viz_msg(const char* msg) {
    char buf[160];
    if (_auto_mode && strncmp(msg, "Viz", 3) == 0) {
        snprintf(buf, sizeof(buf), "[MSG:VizAuto%s]\r\n", msg + 3);
        allChannels.print_except(buf, static_cast<Channel*>(pendant_channel()));
        return;
    }
    snprintf(buf, sizeof(buf), "[MSG:%s]\r\n", msg);
    allChannels.print(buf);
}

static int write_arc(FILE* f, float x0, float y0, float x1, float y1,
                     float i, float j, bool cw,
                     float* xmin, float* xmax, float* ymin, float* ymax,
                     int* count) {
    float cx = x0 + i, cy = y0 + j;
    float r = sqrtf(i*i + j*j);
    if (r < 0.001f) return 0;
    float a0 = atan2f(y0 - cy, x0 - cx);
    float a1 = atan2f(y1 - cy, x1 - cx);
    if (cw) { if (a1 >= a0) a1 -= 2.0f * M_PI; }
    else    { if (a1 <= a0) a1 += 2.0f * M_PI; }
    int written = 0;
    for (int s = 1; s <= _arc_segments && *count < VIZ_MAX_POINTS; s++) {
        float t = (float)s / _arc_segments;
        float ang = a0 + t * (a1 - a0);
        float px = cx + r * cosf(ang);
        float py = cy + r * sinf(ang);
        fprintf(f, "%.3f,%.3f\n", px, py);
        if (px < *xmin) *xmin = px; if (px > *xmax) *xmax = px;
        if (py < *ymin) *ymin = py; if (py > *ymax) *ymax = py;
        (*count)++; written++;
    }
    return written;
}

static bool do_generate(const std::string& nc_path, const std::string& viz_out) {
    // Try path as given, then with /sd prefix, then without /sd prefix
    FILE* nc = fopen(nc_path.c_str(), "r");
    if (!nc && nc_path.find("/sd") != 0) {
        // Try adding /sd prefix
        std::string with_sd = "/sd/" + nc_path;
        nc = fopen(with_sd.c_str(), "r");
    }
    if (!nc && nc_path.find("/sd/") == 0) {
        // Try stripping /sd/ prefix (relative path)
        std::string without_sd = nc_path.substr(4);
        nc = fopen(without_sd.c_str(), "r");
    }
    if (!nc) {
        char errmsg[128];
        snprintf(errmsg, sizeof(errmsg), "VizErr:cannot open:%s", nc_path.c_str());
        viz_msg(errmsg);
        return false;
    }

    // File size -> thinning. ~22 bytes per G-code line is typical CAM output;
    // aim to use ~85% of the point budget over the whole file.
    long file_size = 0;
    if (fseek(nc, 0, SEEK_END) == 0) {
        file_size = ftell(nc);
    }
    fseek(nc, 0, SEEK_SET);
    {
        const long est_lines = file_size / 22;
        const long budget    = (VIZ_MAX_POINTS * 85L) / 100;
        _sample_every        = VIZ_SAMPLE_EVERY;
        _arc_segments        = VIZ_ARC_SEGMENTS;
        if (est_lines > budget * VIZ_SAMPLE_EVERY) {
            _sample_every = (int)((est_lines + budget - 1) / budget);
            _arc_segments = std::max(3, VIZ_ARC_SEGMENTS * VIZ_SAMPLE_EVERY / _sample_every);
        }
    }

    std::string tmp = viz_out + ".tmp";
    remove(tmp.c_str());
    FILE* vf = fopen(tmp.c_str(), "w");
    if (!vf) { fclose(nc); viz_msg("VizErr:cannot write viz file"); return false; }

    _modal_x = 0; _modal_y = 0; _modal_z = 0;
    _modal_abs = true; _modal_inch = false;
    _modal_motion = -1;

    float xmin = 1e9f, xmax = -1e9f, ymin = 1e9f, ymax = -1e9f;
    int n_points = 0, line_num = 0, sample_ct = 0;
    float last_px = 1e9f, last_py = 1e9f;

    fprintf(vf, "VIZ 00000 +00000.000 +00000.000 +00000.000 +00000.000\n");

    char linebuf[256];
    while (fgets(linebuf, sizeof(linebuf), nc) && n_points < VIZ_MAX_POINTS) {
        // Strip newline and uppercase
        int len = strlen(linebuf);
        while (len > 0 && (linebuf[len-1] == '\n' || linebuf[len-1] == '\r')) linebuf[--len] = '\0';
        for (int i = 0; i < len; i++) linebuf[i] = toupper(linebuf[i]);
        // Drop comments: "(...)" anywhere and ";" to end of line - their
        // letters (T1 D=6, X-axis notes...) must not be read as words.
        {
            int  w = 0;
            bool inParen = false;
            for (int r = 0; r < len; r++) {
                char c = linebuf[r];
                if (inParen) { if (c == ')') inParen = false; continue; }
                if (c == '(') { inParen = true; continue; }
                if (c == ';') break;
                linebuf[w++] = c;
            }
            linebuf[w] = '\0';
            len = w;
        }
        line_num++;

        if (line_num % 100 == 0) {
            feed_watchdog();  // 4.1.x: protocol/poller tasks are TWDT-monitored
        }
        if (line_num % 1000 == 0) {
            char msg[160];
            long pos = ftell(nc);  // progress through the file, not the point budget
            snprintf(msg, sizeof(msg), "VizBusy:%s:%d",
                     nc_path.c_str(), file_size > 0 ? (int)(pos * 100 / file_size) : 0);
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
                        case 20: _modal_inch=true;  break; case 21: _modal_inch=false; break;
                        case 90: _modal_abs=true;   break; case 91: _modal_abs=false;  break;
                    } break;
                case 'X': new_x = _modal_abs ? to_mm(val) : _modal_x + to_mm(val); has_x=true; break;
                case 'Y': new_y = _modal_abs ? to_mm(val) : _modal_y + to_mm(val); has_y=true; break;
                case 'Z': new_z = _modal_abs ? to_mm(val) : _modal_z + to_mm(val); break;
                case 'I': arc_i = to_mm(val); break;
                case 'J': arc_j = to_mm(val); break;
            }
        }

        // Motion is modal: "X10 Y20" after a G1 is still a G1 (CAM output
        // usually omits the repeated G word - those moves were all lost).
        if (motion >= 0) _modal_motion = motion;
        else motion = _modal_motion;

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
                if (++sample_ct >= _sample_every || last_px > 1e8f) {
                    sample_ct = 0;
                    fprintf(vf, "%.3f,%.3f\n", new_x, new_y);
                    if (new_x < xmin) xmin = new_x; if (new_x > xmax) xmax = new_x;
                    if (new_y < ymin) ymin = new_y; if (new_y > ymax) ymax = new_y;
                    last_px = new_x; last_py = new_y;
                    n_points++;
                }
            }
        }
        _modal_x = new_x; _modal_y = new_y; _modal_z = new_z;
    }
    fclose(nc); fclose(vf);

    // Rewrite with real header
    FILE* vf_r = fopen(tmp.c_str(), "r");
    remove(viz_out.c_str());
    FILE* vf_w = fopen(viz_out.c_str(), "w");
    if (!vf_r || !vf_w) {
        if (vf_r) fclose(vf_r); if (vf_w) fclose(vf_w);
        remove(tmp.c_str()); return false;
    }
    // " v2": generator version; older files (modal-motion bug) are rebuilt.
    fprintf(vf_w, "VIZ %d %.3f %.3f %.3f %.3f v2\n", n_points, xmin, xmax, ymin, ymax);
    // Skip placeholder header
    char skip[128]; fgets(skip, sizeof(skip), vf_r);
    // Copy rest
    char copybuf[256];
    while (fgets(copybuf, sizeof(copybuf), vf_r)) fputs(copybuf, vf_w);
    fclose(vf_r); fclose(vf_w);
    remove(tmp.c_str());

    char done_msg[128];
    snprintf(done_msg, sizeof(done_msg), "VizReady:%s:%d:%.3f:%.3f:%.3f:%.3f",
             viz_out.c_str(), n_points, xmin, xmax, ymin, ymax);
    viz_msg(done_msg);
    return true;
}

std::string viz_path(const std::string& nc_path) { return nc_path + ".viz"; }

// A usable .viz exists: present and written by this generator version
// (files from the old generator missed modal moves - treat them as missing).
bool viz_exists(const std::string& nc_path) {
    FILE* f = fopen(viz_path(nc_path).c_str(), "r");
    if (!f) return false;
    char head[96] = {};
    bool ok = fgets(head, sizeof(head), f) && strstr(head, " v2");
    fclose(f);
    return ok;
}

bool viz_generate(const std::string& nc_path) {
    if (_viz_busy) { viz_msg("VizBusy:already generating"); return false; }
    std::error_code ec;  // SD is mounted on demand; hold it while we work
    FluidPath       mount { nc_path.compare(0, 4, "/sd/") == 0 ? nc_path : "/sd/" + nc_path, SD, ec };
    std::string vp = viz_path(nc_path);
    if (viz_exists(nc_path)) {
        char msg[128]; snprintf(msg, sizeof(msg), "VizReady:%s", vp.c_str());
        viz_msg(msg); return true;
    }
    // Run synchronously on calling task — avoids FatFS mutex contention
    // that occurs when using a background FreeRTOS task on IDF 4.4.x
    _viz_busy = true;
    do_generate(nc_path, vp);
    _viz_busy = false;
    return true;
}

// ---------------------------------------------------------------------------
// Upload queue: WebDAV / web uploads of G-code to /sd queue a rebuild here
// (from the web server task); viz_poll() runs it on the protocol task once the
// machine is Idle with no job. A short settle delay coalesces the empty-file
// PUT + data PUT that Windows/macOS do, and bursts of copied files.
// ---------------------------------------------------------------------------
struct VizRequest {
    std::string path;
    uint32_t    due;
};
static std::mutex              _q_mutex;
static std::vector<VizRequest> _queue;
static std::atomic<int>        _q_len { 0 };
static const uint32_t          settle_ms = 1500;

static bool is_gcode(const std::string& p) {
    static const char* const exts[] = { ".nc", ".gcode", ".gc", ".ngc", ".tap", ".cnc", ".g" };
    auto dot = p.find_last_of('.');
    if (dot == std::string::npos || p.find('/', dot) != std::string::npos) return false;
    std::string ext = p.substr(dot);
    for (auto& c : ext) c = tolower(c);
    for (auto e : exts) if (ext == e) return true;
    return false;
}

static bool on_sd(const std::string& p) { return p.compare(0, 4, "/sd/") == 0; }

static void enqueue(const std::string& path, uint32_t delay) {
    {
        std::lock_guard<std::mutex> lock(_q_mutex);
        uint32_t due = millis() + delay;
        bool     found = false;
        for (auto& r : _queue) {
            if (r.path == path) { r.due = due; found = true; break; }
        }
        if (!found) _queue.push_back({ path, due });
        _q_len = _queue.size();
    }
    // Tell the web UI it is waiting (it only runs while nothing moves).
    char buf[200];
    snprintf(buf, sizeof(buf), "[MSG:VizAutoQueued:%s]\r\n", path.c_str());
    allChannels.print_except(buf, static_cast<Channel*>(pendant_channel()));
}

void viz_file_written(const std::string& path) {
    if (on_sd(path) && is_gcode(path)) enqueue(path, settle_ms);
}

void viz_file_removed(const std::string& path) {
    if (!on_sd(path) || !is_gcode(path)) return;
    {
        std::lock_guard<std::mutex> lock(_q_mutex);
        for (auto it = _queue.begin(); it != _queue.end(); ++it) {
            if (it->path == path) { _queue.erase(it); break; }
        }
        _q_len = _queue.size();
    }
    std::error_code ec;
    FluidPath       mount { path, SD, ec };
    if (!ec) remove(viz_path(path).c_str());
}

void viz_poll() {
    if (_q_len == 0 || _viz_busy) return;
    // Any state without motion: Alarm is normal after power-up until homing.
    if (Job::active() || !(state_is(State::Idle) || state_is(State::Alarm) || state_is(State::Sleep))) return;
    std::string path;
    {
        std::lock_guard<std::mutex> lock(_q_mutex);
        uint32_t now = millis();
        for (auto it = _queue.begin(); it != _queue.end(); ++it) {
            if ((int32_t)(now - it->due) >= 0) {
                path = it->path;
                _queue.erase(it);
                break;
            }
        }
        _q_len = _queue.size();
    }
    if (path.empty()) return;
    std::error_code ec;
    FluidPath       mount { path, SD, ec };  // SD is mounted on demand; hold it
    if (ec) return;
    FILE* f = fopen(path.c_str(), "r");  // deleted or renamed meanwhile?
    if (!f) return;
    fclose(f);
    std::string vp = viz_path(path);
    remove(vp.c_str());  // never serve a stale preview for a rewritten file
    _viz_busy  = true;
    _auto_mode = true;
    do_generate(path, vp);
    _auto_mode = false;
    _viz_busy  = false;
}

bool viz_handle_command(const char* line) {
    if (strncmp(line, "$Viz/", 5) != 0) return false;
    const char* cmd = line + 5;
    // Rebuild in the background (queued, Idle-only, pendant not disturbed).
    if (strncmp(cmd, "Refresh=", 8) == 0) {
        std::string p = cmd + 8;
        if (!on_sd(p)) p = "/sd" + std::string(p[0] == '/' ? "" : "/") + p;
        if (!is_gcode(p)) { viz_msg("VizAutoErr:not a G-code file"); return true; }
        enqueue(p, 0);
        return true;
    }
    if (strncmp(cmd, "Generate=", 9) == 0) { viz_generate(cmd + 9); return true; }
    if (strncmp(cmd, "Delete=", 7) == 0) {
        remove(viz_path(cmd + 7).c_str());
        viz_msg("VizDeleted"); return true;
    }
    if (strncmp(cmd, "Status=", 7) == 0) {
        bool exists = viz_exists(cmd + 7);
        char msg[128]; snprintf(msg, sizeof(msg), "VizStatus:%s:%s", cmd + 7, exists ? "ready" : "missing");
        viz_msg(msg); return true;
    }
    return false;
}

void viz_init() { _viz_busy = false; }
