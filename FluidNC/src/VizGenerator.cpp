// VizGenerator.cpp — toolpath preview (.viz) builder for the TabUI pendant / WebUI
// ─────────────────────────────────────────────────────────────────────────────
// Scans a G-code file and writes "<file>.viz": header + x,y,z,type,line points.
// Builds are incremental: viz_poll() (protocol task main loop) advances the
// current build a few milliseconds at a time, so the controller keeps taking
// commands, and a build can run during a job (small, throttled slices) - which
// is how a job started from the WebUI gets its preview on the pendant.
// ─────────────────────────────────────────────────────────────────────────────

#include "VizGenerator.h"
#include "Serial.h"     // allChannels
#include "System.h"
#include "Driver/watchdog.h"  // feed_watchdog
#include "State.h"            // state_is
#include "Job.h"              // Job::active
#include "PendantLink.h"      // pendant_channel
#include "UartChannel.h"
#include "Logging.h"          // message_queue, MsgLevelNone
#include "Error.h"
#include "FluidPath.h"        // keeps the SD card mounted while we use it
#include "Planner.h"          // plan_get_block_buffer_available
#include "Machine/MachineConfig.h"  // config->_planner_blocks

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
#include <memory>
#include <esp_timer.h>  // esp_timer_get_time

#define VIZ_MAX_POINTS    8000
#define VIZ_ARC_SEGMENTS  16


// Thinning, scaled per file so large files are covered end to end instead of
// stopping at VIZ_MAX_POINTS part-way through.
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

// Builds from the upload queue report as VizAutoBusy/VizAutoReady/VizAutoErr
// and skip the pendant UART: the pendant loads whatever file a "VizReady:"
// names, which would hijack its preview. Builds the pendant asked for, or for
// the running job, report as plain Viz* to everyone.
static bool _auto_mode = false;

static void viz_say(const char* msg, bool auto_mode) {
    char buf[200];
    if (auto_mode && strncmp(msg, "Viz", 3) == 0) {
        snprintf(buf, sizeof(buf), "[MSG:VizAuto%s]\r\n", msg + 3);
        allChannels.print_except(buf, static_cast<Channel*>(pendant_channel()));
        return;
    }
    snprintf(buf, sizeof(buf), "[MSG:%s]\r\n", msg);
    allChannels.print(buf);
}
// Messages about the build in progress (labelled by who asked for it).
static void viz_msg(const char* msg) { viz_say(msg, _auto_mode); }

std::string viz_path(const std::string& nc_path) { return nc_path + ".viz"; }

static bool on_sd(const std::string& p) { return p.compare(0, 4, "/sd/") == 0; }
static std::string sd_norm(const std::string& p) {
    if (on_sd(p)) return p;
    return "/sd" + std::string(!p.empty() && p[0] == '/' ? "" : "/") + p;
}

// ---------------------------------------------------------------------------
// One build in progress. Only touched on the protocol task (viz_poll).
// ---------------------------------------------------------------------------
enum class VizMode { Auto, Requested, Job };

static const int HEADER_W = 96;  // fixed-width header, rewritten in place at the end

struct VizBuild {
    bool                       active = false;
    bool                       push   = false;  // send it to the pendant when done
    VizMode                    mode   = VizMode::Auto;
    std::string                nc, out, tmp;
    std::unique_ptr<FluidPath> mount;  // SD is mounted on demand; hold it while we work
    FILE*                      in  = nullptr;
    FILE*                      vf  = nullptr;
    long                       file_size = 0;
    float                      xmin, xmax, ymin, ymax;
    int                        n_points, line_num, sample_ct;
    float                      last_px, last_py, last_pz;
    int                        last_t;
    float                      pend_x, pend_y, pend_z;
    int                        pend_line;
    bool                       pend_ok;
    uint32_t                   last_busy_ms;
    // Two passes: 1 measures the path (length, rapids, bounds), 2 writes it
    // with one detail size for the whole file, so the point budget covers
    // the file end to end (a large file used to stop part-way at 8000).
    int                        pass = 1;
    double                     plen = 0;    // path length (pass 1)
    int                        forced = 0;  // rapids / rapid<->cut switches (pass 1)
    float                      res = 0;     // detail size in mm (pass 2)
    int                        retries = 0; // pass 2 restarted with a coarser res
};
static VizBuild         _b;
static std::atomic<bool> _cancel { false };
static std::string      _current;  // path being built (guarded by _q_mutex)

static void emit(float x, float y, float z, int t, int line) {
    if (_b.pass == 1) {  // measuring
        if (_b.last_px < 1e8f) {
            float dx = x - _b.last_px, dy = y - _b.last_py, dz = z - _b.last_pz;
            _b.plen += sqrtf(dx * dx + dy * dy + dz * dz);
        }
        if (x < _b.xmin) _b.xmin = x; if (x > _b.xmax) _b.xmax = x;
        if (y < _b.ymin) _b.ymin = y; if (y > _b.ymax) _b.ymax = y;
        _b.last_px = x; _b.last_py = y; _b.last_pz = z; _b.last_t = t;
        _b.n_points++;
        return;
    }
    if (_b.n_points >= VIZ_MAX_POINTS) return;
    fprintf(_b.vf, "%.3f,%.3f,%.3f,%d,%d\n", x, y, z, t, line);
    if (x < _b.xmin) _b.xmin = x; if (x > _b.xmax) _b.xmax = x;
    if (y < _b.ymin) _b.ymin = y; if (y > _b.ymax) _b.ymax = y;
    _b.last_px = x; _b.last_py = y; _b.last_pz = z; _b.last_t = t;
    _b.n_points++;
}
static void flush_pending() {
    if (_b.pend_ok) { emit(_b.pend_x, _b.pend_y, _b.pend_z, 1, _b.pend_line); _b.pend_ok = false; }
}

static void write_arc(float x0, float y0, float x1, float y1, float i, float j, bool cw, float z0, float z1, int line) {
    float cx = x0 + i, cy = y0 + j;
    float r = sqrtf(i*i + j*j);
    if (r < 0.001f) return;
    float a0 = atan2f(y0 - cy, x0 - cx);
    float a1 = atan2f(y1 - cy, x1 - cx);
    if (cw) { if (a1 >= a0) a1 -= 2.0f * M_PI; }
    else    { if (a1 <= a0) a1 += 2.0f * M_PI; }
    // Segments: one per detail size along the arc (2..16); 16 while measuring.
    int segs = _arc_segments;
    if (_b.pass == 2 && _b.res > 0) {
        float len = r * fabsf(a1 - a0);
        segs = std::max(2, std::min(VIZ_ARC_SEGMENTS, (int)ceilf(len / _b.res)));
    }
    for (int s = 1; s <= segs && (_b.pass == 1 || _b.n_points < VIZ_MAX_POINTS); s++) {
        float t = (float)s / segs;
        float ang = a0 + t * (a1 - a0);
        emit(cx + r * cosf(ang), cy + r * sinf(ang), z0 + t * (z1 - z0), 1, line);  // helical Z
    }
}

static void build_close(bool keep) {
    if (_b.in) { fclose(_b.in); _b.in = nullptr; }
    if (_b.vf) { fclose(_b.vf); _b.vf = nullptr; }
    if (!keep && !_b.tmp.empty()) remove(_b.tmp.c_str());
    _b.mount.reset();
    _b.active  = false;
    _auto_mode = false;
    _cancel    = false;
}

static void build_fail(const char* what, const std::string& path) {
    int  e = errno;
    char msg[200];
    snprintf(msg, sizeof(msg), "VizErr:%s:%s (%s)", what, path.c_str(), e ? strerror(e) : "unknown");
    viz_msg(msg);
    build_close(false);
}

// Open the input and output; false (and a VizErr) when that is not possible.
static bool build_begin(const std::string& nc, VizMode mode, bool push) {
    _b        = VizBuild();
    _b.nc     = nc;
    _b.out    = viz_path(nc);
    _b.tmp    = _b.out + ".tmp";
    _b.mode   = mode;
    _b.push   = push;
    _auto_mode = (mode == VizMode::Auto);
    _cancel    = false;

    std::error_code ec;
    _b.mount.reset(new FluidPath(nc, SD, ec));
    if (ec) {
        char msg[200];
        snprintf(msg, sizeof(msg), "VizErr:no SD card:%s (%s)", nc.c_str(), ec.message().c_str());
        viz_msg(msg);
        build_close(false);
        return false;
    }
    errno = 0;
    _b.in = fopen(nc.c_str(), "r");
    if (!_b.in) { build_fail("cannot open", nc); return false; }

    if (fseek(_b.in, 0, SEEK_END) == 0) _b.file_size = ftell(_b.in);
    fseek(_b.in, 0, SEEK_SET);
    _arc_segments = VIZ_ARC_SEGMENTS;

    remove(_b.tmp.c_str());
    errno = 0;
    _b.vf = fopen(_b.tmp.c_str(), "w");
    if (!_b.vf) { build_fail("cannot write", _b.tmp); return false; }
    fprintf(_b.vf, "%-*s\n", HEADER_W - 1, "VIZ 0");  // placeholder, same width as the real one

    _modal_x = 0; _modal_y = 0; _modal_z = 0;
    _modal_abs = true; _modal_inch = false;
    _modal_motion = -1;
    _b.xmin = 1e9f; _b.xmax = -1e9f; _b.ymin = 1e9f; _b.ymax = -1e9f;
    _b.n_points = 0; _b.line_num = 0; _b.sample_ct = 0;
    _b.last_px = 1e9f; _b.last_py = 1e9f; _b.last_pz = 0; _b.last_t = -1;
    _b.pend_ok = false; _b.pend_line = 0;
    _b.last_busy_ms = millis();
    _b.active = true;
    return true;
}

// Read one G-code line; long lines are cut, the rest of the line is skipped
// so line numbers stay right. false at end of file.
static bool read_line(char* buf, size_t len) {
    if (!fgets(buf, len, _b.in)) return false;
    size_t n = strlen(buf);
    if (n && buf[n - 1] != '\n' && !feof(_b.in)) {
        int c;
        while ((c = fgetc(_b.in)) != EOF && c != '\n') {}
    }
    return true;
}

static void process_line(char* linebuf) {
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
    }
    const int line_num = ++_b.line_num;

    char* p = linebuf;
    if (*p == 'N') { while (*p && !isspace(*p)) p++; }

    float new_x = _modal_x, new_y = _modal_y, new_z = _modal_z;
    float arc_i = 0, arc_j = 0;
    int motion = -1;
    bool has_x = false, has_y = false, has_z = false;

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
            case 'Z': new_z = _modal_abs ? to_mm(val) : _modal_z + to_mm(val); has_z=true; break;
            case 'I': arc_i = to_mm(val); break;
            case 'J': arc_j = to_mm(val); break;
        }
    }

    // Motion is modal: "X10 Y20" after a G1 is still a G1.
    if (motion >= 0) _modal_motion = motion;
    else motion = _modal_motion;

    if (motion < 0 || (!has_x && !has_y && !has_z)) {
        _modal_x = new_x; _modal_y = new_y; _modal_z = new_z;
        return;
    }

    if (motion == 2 || motion == 3) {
        flush_pending();  // the arc starts at the current position
        write_arc(_modal_x, _modal_y, new_x, new_y, arc_i, arc_j, motion == 2, _modal_z, new_z, line_num);
        _b.last_px = new_x; _b.last_py = new_y; _b.last_pz = new_z; _b.last_t = 1;
    } else {
        // Keep every rapid, every Z change and every switch between rapid
        // and cut (so plunges/retracts and the 3D shape survive); thin only
        // runs of cuts at one depth, and then keep the last skipped point
        // before the next kept one so corners stay put.
        // Pass 1 keeps everything (measuring); pass 2 keeps a cut point when it
        // is _b.res away from the last kept one (XY or depth).
        int   t      = (motion == 0) ? 0 : 1;
        float dx     = new_x - _b.last_px, dy = new_y - _b.last_py;
        bool  first  = _b.last_px > 1e8f;
        bool  tchg   = t != _b.last_t;
        if (_b.pass == 1 && (t == 0 || tchg)) ++_b.forced;
        float res    = _b.pass == 1 ? 0.0f : _b.res;
        bool  zchg   = fabsf(new_z - _b.last_pz) > std::max(res, 0.001f);
        bool  far    = (dx * dx + dy * dy) >= res * res;
        bool  keep   = first || zchg || tchg || t == 0 || far;
        if (keep) {
            if (zchg || tchg || t == 0) flush_pending();
            emit(new_x, new_y, new_z, t, line_num);
            _b.sample_ct = 0;
            _b.pend_ok   = false;
        } else {
            _b.pend_x = new_x; _b.pend_y = new_y; _b.pend_z = new_z; _b.pend_line = line_num; _b.pend_ok = true;
        }
    }
    _modal_x = new_x; _modal_y = new_y; _modal_z = new_z;
}

static void push_begin(const std::string& vpath);

static void build_finish() {
    flush_pending();
    fclose(_b.in); _b.in = nullptr;
    // " v4": generator version (v3 points, whole-file coverage); older files are rebuilt.
    char head[HEADER_W + 1];
    int  n = snprintf(head, sizeof(head), "VIZ %d %.3f %.3f %.3f %.3f v4", _b.n_points, _b.xmin, _b.xmax, _b.ymin, _b.ymax);
    if (n >= HEADER_W) n = HEADER_W - 1;
    errno = 0;
    bool ok = fseek(_b.vf, 0, SEEK_SET) == 0 && fprintf(_b.vf, "%-*.*s\n", HEADER_W - 1, n, head) == HEADER_W;
    ok      = (fclose(_b.vf) == 0) && ok;
    _b.vf   = nullptr;
    if (!ok) { build_fail("cannot write", _b.tmp); return; }
    remove(_b.out.c_str());
    errno = 0;
    if (rename(_b.tmp.c_str(), _b.out.c_str()) != 0) { build_fail("cannot rename", _b.out); return; }

    char done_msg[200];
    snprintf(done_msg, sizeof(done_msg), "VizReady:%s:%d:%.3f:%.3f:%.3f:%.3f",
             _b.out.c_str(), _b.n_points, _b.xmin, _b.xmax, _b.ymin, _b.ymax);
    viz_msg(done_msg);
    bool        push = _b.push;
    std::string out  = _b.out;
    build_close(true);
    if (push) push_begin(out);
}

// (Re)start the writing pass with detail size res.
static bool pass2_start(float res) {
    _b.pass = 2;
    _b.res  = res;
    fseek(_b.in, 0, SEEK_SET);
    if (_b.vf) { fclose(_b.vf); _b.vf = nullptr; }
    errno = 0;
    _b.vf = fopen(_b.tmp.c_str(), "w");
    if (!_b.vf) { build_fail("cannot write", _b.tmp); return false; }
    fprintf(_b.vf, "%-*s\n", HEADER_W - 1, "VIZ 0");
    _modal_x = 0; _modal_y = 0; _modal_z = 0;
    _modal_abs = true; _modal_inch = false;
    _modal_motion = -1;
    _b.xmin = 1e9f; _b.xmax = -1e9f; _b.ymin = 1e9f; _b.ymax = -1e9f;
    _b.n_points = 0; _b.line_num = 0; _b.sample_ct = 0;
    _b.last_px = 1e9f; _b.last_py = 1e9f; _b.last_pz = 0; _b.last_t = -1;
    _b.pend_ok = false; _b.pend_line = 0;
    return true;
}

// Pass 1 done: one detail size that spreads the point budget over the whole
// path (rapids and rapid/cut switches are always kept), never finer than
// 1/4000 of the part.
static bool pass1_done() {
    const double budget = VIZ_MAX_POINTS * 0.9;
    double avail = std::max(budget - _b.forced, budget / 4);
    float  span  = std::max(_b.xmax - _b.xmin, _b.ymax - _b.ymin);
    float  res   = (float)(_b.plen / avail);
    res          = std::max(res, span > 0 ? span / 4000.0f : 0.0f);
    res          = std::max(res, 0.005f);
    return pass2_start(res);
}

// Advance the current build until `budget_us` is used up.
static void build_step(int64_t budget_us) {
    if (_cancel) { build_close(false); return; }
    const int64_t t0 = esp_timer_get_time();
    char linebuf[256];
    int  n = 0;
    for (;;) {
        if (_b.pass == 2 && _b.n_points >= VIZ_MAX_POINTS) {
            // More points than measured: coarser detail, write again.
            if (_b.retries < 3 && !feof(_b.in)) {
                ++_b.retries;
                if (!pass2_start(_b.res * 1.6f)) return;
                continue;
            }
            break;
        }
        if (!read_line(linebuf, sizeof(linebuf))) {
            if (ferror(_b.in)) { errno = EIO; build_fail("read error", _b.nc); return; }
            if (_b.pass == 1) {
                if (!pass1_done()) return;
                continue;
            }
            break;  // end of file
        }
        process_line(linebuf);
        if ((++n & 31) == 0) {
            if (esp_timer_get_time() - t0 >= budget_us) {
                uint32_t now = millis();
                if (now - _b.last_busy_ms >= 2000) {  // progress through the file
                    _b.last_busy_ms = now;
                    char msg[200];
                    long pos = ftell(_b.in);
                    int  pct = _b.file_size > 0 ? (int)(pos * 50 / _b.file_size) : 0;  // two passes
                    snprintf(msg, sizeof(msg), "VizBusy:%s:%d", _b.nc.c_str(), _b.pass == 1 ? pct : 50 + pct);
                    viz_msg(msg);
                }
                return;
            }
        }
    }
    build_finish();
}

// A usable .viz exists: present and written by this generator version
// (files from the old generator missed modal moves - treat them as missing).
bool viz_exists(const std::string& nc_path) {
    FILE* f = fopen(viz_path(nc_path).c_str(), "r");
    if (!f) return false;
    char head[HEADER_W + 8] = {};
    bool ok = fgets(head, sizeof(head), f) && strstr(head, " v4");
    fclose(f);
    return ok;
}


// ---------------------------------------------------------------------------
// Push a .viz to the pendant ($Viz/Push=). FluidNC sends the points, the
// pendant only listens - no file reads from the pendant, no request/reply
// matching. Messages go to the pendant UART only:
//   [MSG:VZB:<n>:<xmin>:<xmax>:<ymin>:<ymax>:<zmin>:<zmax>:<viz path>]   begin
//   [MSG:VZ:<seq>:<sum>:<x,y,z,t,ln>;<x,y,z,t,ln>;...]     up to 4 points;
//        <sum> = 4 hex digits, byte sum of the points text (line noise check)
//   [MSG:VZE:<count>:<viz path>]                           end
//   [MSG:VZX:<viz path>:<reason>]                          cannot send it
//
// Sized for the pendant's screen: "$Viz/Push=<px>:<max>:<file>" says the
// screen is <px> pixels across and it has room for <max> points. Detail
// smaller than one pixel of the whole part is dropped (a point is kept when
// it is a pixel away from the last one, changes depth by a pixel, or
// switches between rapid and cut). If that is still more than <max>, the
// pixel is enlarged until it fits. A first pass counts, the second sends.
// Paced from viz_poll() so the UART and the output queue are never flooded.
// ---------------------------------------------------------------------------
static int _push_px  = 0;     // pendant screen size in pixels (0 = send every point)
static int _push_max = 8000;  // points the pendant can hold

struct ThinState {
    bool        have = false;
    float       lx = 0, ly = 0, lz = 0;
    int         lt = -1;
    bool        pend = false;
    std::string pline;
};

// Feed one .viz point line; calls out() for each point to keep.
template <class F>
static void thin_point(ThinState& s, const char* line, float res, F&& out) {
    float    x, y, z;
    int      t;
    unsigned ln;
    if (sscanf(line, "%f,%f,%f,%d,%u", &x, &y, &z, &t, &ln) < 4) return;
    bool keep = true;
    if (s.have && res > 0) {
        float dx = x - s.lx, dy = y - s.ly;
        bool  tchg = t != s.lt;
        bool  zchg = fabsf(z - s.lz) >= res;
        keep       = tchg || zchg || (dx * dx + dy * dy >= res * res);
        if (keep && (tchg || zchg) && s.pend) out(s.pline.c_str());  // exact corner before a plunge / rapid
    }
    if (keep) {
        out(line);
        s.have = true; s.lx = x; s.ly = y; s.lz = z; s.lt = t;
        s.pend = false;
    } else {
        s.pend  = true;
        s.pline = line;
    }
}
template <class F>
static void thin_end(ThinState& s, F&& out) {
    if (s.pend) out(s.pline.c_str());  // the last point
    s.pend = false;
}

struct VizPush {
    bool                       active = false;
    bool                       sending = false;  // false: counting pass
    std::string                path;             // .viz
    std::unique_ptr<FluidPath> mount;
    FILE*                      f     = nullptr;
    long                       data_pos = 0;     // first point line
    float                      b[4] = {};
    int                        n_file = 0;       // points in the file
    float                      res = 0;          // thinning "pixel" in mm
    int                        iter = 0;
    int                        count = 0;        // counting pass result
    float                      zmin = 0, zmax = 0;
    int                        stride = 1;
    int                        idx = 0;          // kept points seen while sending
    int                        seq = 0;
    int                        sent = 0;
    ThinState                  thin;
    std::vector<std::string>   outq;
    uint32_t                   last_ms = 0;
};
static VizPush _p;

static void to_pendant(const std::string& line) {
    UartChannel* ch = pendant_channel();
    if (ch) {
        ch->sendLine(MsgLevelNone, line);
    }
}

static void push_end() {
    if (_p.f) { fclose(_p.f); _p.f = nullptr; }
    _p.mount.reset();
    _p.active = false;
    _p.outq.clear();
}

static void push_rewind() {
    fseek(_p.f, _p.data_pos, SEEK_SET);
    _p.thin  = ThinState();
    _p.count = 0;
    _p.zmin  = 1e9f;
    _p.zmax  = -1e9f;
    _p.idx   = 0;
}

static void push_begin(const std::string& vpath) {
    push_end();
    if (!pendant_channel()) return;
    std::error_code ec;
    _p.mount.reset(new FluidPath(vpath, SD, ec));
    if (!ec) _p.f = fopen(vpath.c_str(), "r");
    char head[HEADER_W + 8] = {};
    int  n = 0;
    if (!_p.f || !fgets(head, sizeof(head), _p.f) || !strstr(head, " v4") ||
        sscanf(head, "VIZ %d %f %f %f %f", &n, &_p.b[0], &_p.b[1], &_p.b[2], &_p.b[3]) != 5) {
        to_pendant("[MSG:VZX:" + vpath + ":" + (_p.f ? "old format" : "missing") + "]");
        push_end();
        return;
    }
    _p.path     = vpath;
    _p.data_pos = ftell(_p.f);
    _p.n_file   = n;
    float span  = std::max(_p.b[1] - _p.b[0], _p.b[3] - _p.b[2]);
    _p.res      = (_push_px > 0 && span > 0) ? span / _push_px : 0;
    _p.iter     = 0;
    _p.stride   = 1;
    _p.seq      = 0;
    _p.sent     = 0;
    _p.sending  = false;
    _p.last_ms  = 0;
    _p.active   = true;
    push_rewind();
}

// Counting pass finished: fits? Otherwise coarser pixel and count again.
static void push_counted() {
    if (_p.count > _push_max && _p.iter < 10) {
        // Coarser until it fits. Every-n-th thinning (below) is only a last
        // resort: it would drop corners.
        float f = std::max(1.3f, sqrtf((float)_p.count / _push_max) * 1.15f);
        float span = std::max(_p.b[1] - _p.b[0], _p.b[3] - _p.b[2]);
        _p.res = _p.res > 0 ? _p.res * f : std::max(span / 2000.0f, 0.01f);
        ++_p.iter;
        push_rewind();
        return;
    }
    _p.stride = (_p.count > _push_max && _push_max > 0) ? (_p.count + _push_max - 1) / _push_max : 1;
    int n     = (_p.count + _p.stride - 1) / _p.stride;
    char buf[256];
    if (_p.zmin > _p.zmax) { _p.zmin = _p.zmax = 0; }
    snprintf(buf, sizeof(buf), "[MSG:VZB:%d:%.3f:%.3f:%.3f:%.3f:%.3f:%.3f:%s]", n, _p.b[0], _p.b[1], _p.b[2], _p.b[3], _p.zmin, _p.zmax,
             _p.path.c_str());
    to_pendant(buf);
    snprintf(buf, sizeof(buf), "[MSG:VizPush:%s:%d of %d points, %.2f mm detail]\r\n", _p.path.c_str(), n, _p.n_file, _p.res);
    allChannels.print_except(buf, static_cast<Channel*>(pendant_channel()));
    _p.sending = true;
    push_rewind();
}

static void push_step() {
    if (!_p.active) return;
    uint32_t   now = millis();
    const bool job = Job::active();
    char       line[64];
    if (!_p.sending) {
        // Counting: a few hundred lines per call (a 8000-point .viz is ~200 KB).
        int  lines = job ? 150 : 600;
        auto cnt   = [&](const char* l) {
            ++_p.count;
            float z;
            if (sscanf(l, "%*f,%*f,%f", &z) == 1) {
                _p.zmin = std::min(_p.zmin, z);
                _p.zmax = std::max(_p.zmax, z);
            }
        };
        while (lines-- > 0) {
            if (!fgets(line, sizeof(line), _p.f)) {
                thin_end(_p.thin, cnt);
                push_counted();
                return;
            }
            thin_point(_p.thin, line, _p.res, cnt);
        }
        return;
    }
    if (now - _p.last_ms < (job ? 20u : 8u)) return;
    if (uxQueueMessagesWaiting(message_queue) > 16) return;  // let the output drain first
    _p.last_ms = now;
    auto keep = [&](const char* l) {
        if ((_p.idx++ % _p.stride) != 0) return;
        std::string s(l);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
        _p.outq.push_back(s);
    };
    const int msgs = job ? 3 : 6;
    for (int m = 0; m < msgs; ++m) {
        bool eof = false;
        while (_p.outq.size() < 4) {
            if (!fgets(line, sizeof(line), _p.f)) {
                thin_end(_p.thin, keep);
                eof = true;
                break;
            }
            thin_point(_p.thin, line, _p.res, keep);
        }
        size_t take = std::min<size_t>(4, _p.outq.size());
        if (take) {
            std::string pts;
            for (size_t i = 0; i < take; ++i) {
                if (i) pts += ';';
                pts += _p.outq[i];
            }
            unsigned sum = 0;
            for (unsigned char c : pts) sum += c;
            char head[32];
            snprintf(head, sizeof(head), "[MSG:VZ:%d:%04X:", _p.seq, sum & 0xFFFF);
            std::string out = head + pts + "]";
            _p.outq.erase(_p.outq.begin(), _p.outq.begin() + take);
            to_pendant(out);
            ++_p.seq;
            _p.sent += take;
        }
        if (eof && _p.outq.empty()) {
            to_pendant("[MSG:VZE:" + std::to_string(_p.sent) + ":" + _p.path + "]");
            push_end();
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Request queue. Filled from any task (WebDAV/uploads on the web server task,
// $Viz commands on the polling task); drained by viz_poll() on the protocol
// task. Uploads wait a short settle delay to coalesce the empty-file PUT +
// data PUT that Windows/macOS do, and bursts of copied files.
// ---------------------------------------------------------------------------
struct VizRequest {
    std::string path;
    uint32_t    due;
    VizMode     mode;
    bool        push;  // the pendant wants the points sent to it
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

static void enqueue(const std::string& path, uint32_t delay, VizMode mode, bool push = false) {
    {
        std::lock_guard<std::mutex> lock(_q_mutex);
        uint32_t due = millis() + delay;
        bool     found = false;
        for (auto& r : _queue) {
            if (r.path == path) {
                r.due = due;
                if (mode == VizMode::Auto) r.mode = VizMode::Auto;  // file changed: rebuild
                else if (r.mode == VizMode::Job) r.mode = mode;
                r.push = r.push || push;
                found = true;
                break;
            }
        }
        if (!found) {
            VizRequest r { path, due, mode, push };
            if (mode == VizMode::Auto) _queue.push_back(r);
            else _queue.insert(_queue.begin(), r);  // the pendant / the job is waiting for it
        }
        _q_len = _queue.size();
        // The file was rewritten while its preview was being built: start over.
        if (mode == VizMode::Auto && path == _current) _cancel = true;
    }
    if (mode == VizMode::Auto) {
        // Tell the web UI it is waiting.
        char buf[200];
        snprintf(buf, sizeof(buf), "[MSG:VizAutoQueued:%s]\r\n", path.c_str());
        allChannels.print_except(buf, static_cast<Channel*>(pendant_channel()));
    }
}

void viz_file_written(const std::string& path) {
    if (on_sd(path) && is_gcode(path)) enqueue(path, settle_ms, VizMode::Auto);
}

void viz_job_started(const std::string& path) {
    if (on_sd(path) && is_gcode(path)) enqueue(path, 0, VizMode::Job);
}

void viz_file_removed(const std::string& path) {
    if (!on_sd(path) || !is_gcode(path)) return;
    {
        std::lock_guard<std::mutex> lock(_q_mutex);
        for (auto it = _queue.begin(); it != _queue.end(); ++it) {
            if (it->path == path) { _queue.erase(it); break; }
        }
        _q_len = _queue.size();
        if (path == _current) _cancel = true;
    }
    std::error_code ec;
    FluidPath       mount { path, SD, ec };
    if (!ec) remove(viz_path(path).c_str());
}

static bool idle_no_job() {
    return !Job::active() && (state_is(State::Idle) || state_is(State::Alarm) || state_is(State::Sleep));
}

// Start the next request that is due and allowed now. Uploads (Auto) wait
// for Idle with no job; the pendant's requests and the running job's preview
// go any time.
static void start_next() {
    VizRequest req;
    bool       got = false;
    bool       idle = idle_no_job();
    {
        std::lock_guard<std::mutex> lock(_q_mutex);
        uint32_t now = millis();
        for (auto it = _queue.begin(); it != _queue.end(); ++it) {
            if ((int32_t)(now - it->due) < 0) continue;
            if (it->mode == VizMode::Auto && !idle) continue;
            req = *it;
            _queue.erase(it);
            got = true;
            break;
        }
        _q_len = _queue.size();
        if (got) _current = req.path;
    }
    if (!got) return;

    bool started = false;
    {
        std::error_code ec;
        FluidPath       mount { req.path, SD, ec };  // hold the card for the checks below
        if (!ec) {
            if (req.mode != VizMode::Auto && viz_exists(req.path)) {
                if (req.push) {
                    push_begin(viz_path(req.path));
                } else if (req.mode == VizMode::Requested) {
                    char msg[200];
                    snprintf(msg, sizeof(msg), "VizReady:%s", viz_path(req.path).c_str());
                    viz_say(msg, false);
                }
            } else {
                if (req.mode == VizMode::Auto) {
                    FILE* f = fopen(req.path.c_str(), "r");  // deleted or renamed meanwhile?
                    if (f) {
                        fclose(f);
                        remove(viz_path(req.path).c_str());  // never serve a stale preview for a rewritten file
                        started = build_begin(req.path, req.mode, req.push);
                    }
                } else {
                    started = build_begin(req.path, req.mode, req.push);
                }
            }
        } else if (req.push) {
            to_pendant("[MSG:VZX:" + viz_path(req.path) + ":no SD card]");
        } else if (req.mode != VizMode::Auto) {
            char msg[200];
            snprintf(msg, sizeof(msg), "VizErr:no SD card:%s (%s)", req.path.c_str(), ec.message().c_str());
            viz_say(msg, false);
        }
    }
    if (!started) {
        std::lock_guard<std::mutex> lock(_q_mutex);
        _current.clear();
    }
}

// ---------------------------------------------------------------------------
// Job start waits for the pendant's preview. $SD/Run with a pendant connected
// does not start the file right away: the pendant is told which file is
// coming ([MSG:VZJ:<file>]), asks for the path ($Viz/Push), and once it is on
// its screen says so ($Viz/Shown=<file>). Then the job starts. It also starts
// if the pendant goes quiet for 15 s (no build / push progress), disconnects,
// or does not know VZJ (older firmware). Reset cancels the waiting job.
// ---------------------------------------------------------------------------
struct PendingJob {
    bool        active = false;
    bool        ready  = false;
    Channel*    in     = nullptr;  // the opened file
    Channel*    out    = nullptr;
    Channel*    ack    = nullptr;
    std::string nc;
    uint32_t    deadline = 0;
};
static PendingJob        _pj;
static const uint32_t    job_wait_ms = 15000;

static bool pendant_connected() { return strcmp(pendant_state_name(), "connected") == 0; }

bool viz_hold_job(Channel* in, const std::string& path, Channel* out, Channel* ack) {
    if (_pj.active || Job::active() || !pendant_connected() || !on_sd(path) || !is_gcode(path)) return false;
    _pj          = PendingJob();
    _pj.active   = true;
    _pj.in       = in;
    _pj.out      = out;
    _pj.ack      = ack;
    _pj.nc       = path;
    _pj.deadline = millis() + job_wait_ms;
    to_pendant("[MSG:VZJ:" + path + "]");
    allChannels.print_except("[MSG:JobWait:starting once the pendant shows the preview]\r\n", static_cast<Channel*>(pendant_channel()));
    return true;
}

void viz_cancel_pending_job() {
    if (!_pj.active) return;
    if (_pj.ack) {
        _pj.ack->ack(Error::Reset);
        _pj.ack->release_processing_ref();
    }
    delete _pj.in;
    _pj = PendingJob();
    allChannels.print("[MSG:JobWait:cancelled]\r\n");
}

static void pending_poll() {
    if (!_pj.active) return;
    uint32_t now = millis();
    if (state_is(State::Alarm) || state_is(State::ConfigAlarm) || state_is(State::Critical)) {
        viz_cancel_pending_job();
        return;
    }
    // Building or sending this file's preview counts as progress.
    bool busy = (_b.active && _b.nc == _pj.nc) || (_p.active && _p.path == viz_path(_pj.nc));
    if (busy) _pj.deadline = now + job_wait_ms;
    bool timeout = (int32_t)(now - _pj.deadline) >= 0;
    if (!_pj.ready && !timeout && pendant_connected()) return;
    if (!state_is(State::Idle)) return;  // e.g. a jog still finishing
    if (!_pj.ready) {
        allChannels.print(timeout ? "[MSG:JobWait:pendant did not confirm - starting]\r\n"
                                  : "[MSG:JobWait:pendant disconnected - starting]\r\n");
    }
    Channel* in = _pj.in; Channel* out = _pj.out; Channel* ack = _pj.ack;
    _pj = PendingJob();
    Job::nest(in, out, ack);
}

// ---------------------------------------------------------------------------
// Prepare: "$Job/Prepare=<file>" (WebUI's Prepare button) shows the file on the
// pendant before anything runs. Shared state, broadcast to every channel but
// the pendant as [MSG:Prepared:<state>:<file>]:
//   loading   - the pendant is loading the path
//   ready     - the pendant shows it ($Viz/Shown)
//   nopendant - no pendant connected (Run as usual)
//   none      - nothing prepared (cancelled, or the job started)
// The pendant gets [MSG:VZP:<file>] (show it, offer Run / Cancel) and
// [MSG:VZC] (cancelled). Run from either side is the usual $SD/Run;
// "$Job/Unprepare" cancels from either side; "$Job/Prepared" reports.
// ---------------------------------------------------------------------------
static std::string _prep_nc;
static std::string _prep_state = "none";

static void prep_broadcast() {
    std::string m = "[MSG:Prepared:" + _prep_state + ":" + _prep_nc + "]\r\n";
    allChannels.print_except(m.c_str(), static_cast<Channel*>(pendant_channel()));
}
static void prep_set(const std::string& state, const std::string& nc) {
    _prep_state = state;
    _prep_nc    = nc;
    prep_broadcast();
}
static void prep_clear(bool tell_pendant) {
    if (_prep_state == "none") return;
    if (tell_pendant) to_pendant("[MSG:VZC]");
    prep_set("none", "");
}
// $SD/Run of the prepared file: it is running now.
bool viz_prepared_ready(const std::string& path) {
    bool ready = _prep_nc == path && _prep_state == "ready";
    if (_prep_nc == path) prep_clear(false);
    return ready;
}

static void viz_job_shown(const std::string& arg) {
    std::string p = sd_norm(arg);
    if (p.size() > 4 && p.compare(p.size() - 4, 4, ".viz") == 0) p.resize(p.size() - 4);
    if (_pj.active && p == _pj.nc) _pj.ready = true;
    if (p == _prep_nc && _prep_state == "loading") prep_set("ready", p);
}

bool viz_job_command(const char* line, Channel& out) {
    if (strncasecmp(line, "$Job/", 5) != 0) return false;
    const char* cmd = line + 5;
    if (strncasecmp(cmd, "Prepare=", 8) == 0) {
        std::string p = sd_norm(cmd + 8);
        if (!is_gcode(p)) { out.print("[MSG:ERR: Prepare: not a G-code file on the SD card]\n"); return true; }
        if (pendant_connected()) {
            to_pendant("[MSG:VZP:" + p + "]");
            prep_set("loading", p);
        } else {
            prep_set("nopendant", p);
        }
        return true;
    }
    if (strcasecmp(cmd, "Unprepare") == 0) { prep_clear(true); return true; }
    if (strcasecmp(cmd, "Prepared") == 0) {
        std::string m = "[MSG:Prepared:" + _prep_state + ":" + _prep_nc + "]\n";
        out.print(m.c_str());
        return true;
    }
    return false;
}

void viz_poll() {
    static uint32_t last_slice_ms = 0;
    pending_poll();
    push_step();
    if (!_b.active) {
        if (_q_len == 0) return;
        start_next();
        if (!_b.active) return;
    }
    // Slice size by what the machine is doing. Idle: big slices (commands
    // still get through between them). Job running: a short slice now and
    // then, and only while the planner has moves queued, so the job is never
    // starved. Jogging / homing / MDI motion: wait.
    int64_t  budget;
    uint32_t now = millis();
    if (idle_no_job()) {
        budget = 30000;
    } else if (Job::active()) {
        if (now - last_slice_ms < 25) return;
        int queued = (config->_planner_blocks - 1) - plan_get_block_buffer_available();
        if (state_is(State::Cycle) && queued < 4) return;
        budget = 2000;
    } else {
        return;
    }
    feed_watchdog();
    build_step(budget);
    last_slice_ms = millis();
    if (!_b.active) {
        std::lock_guard<std::mutex> lock(_q_mutex);
        _current.clear();
    }
}

// Pendant: send me this file's preview (build it first if needed).
// "$Viz/Push=<px>:<max>:<file>" or "$Viz/Push=<file>"
static void viz_push_request(const char* arg) {
    if (isdigit((unsigned char)*arg)) {
        char* e;
        long  px = strtol(arg, &e, 10);
        if (*e == ':') {
            long mx = strtol(e + 1, &e, 10);
            if (*e == ':') {
                _push_px  = (int)std::max(0L, std::min(px, 4000L));
                _push_max = (int)std::max(100L, std::min(mx, 20000L));
                arg       = e + 1;
            }
        }
    } else {
        _push_px  = 0;
        _push_max = 8000;
    }
    std::string p = sd_norm(arg);
    if (_p.active && _p.path == viz_path(p)) return;  // already on its way
    enqueue(p, 0, VizMode::Requested, true);
}

bool viz_generate(const std::string& nc_path) {
    enqueue(sd_norm(nc_path), 0, VizMode::Requested);
    return true;
}

bool viz_handle_command(const char* line) {
    if (strncmp(line, "$Viz/", 5) != 0) return false;
    const char* cmd = line + 5;
    // Rebuild in the background (Idle only, pendant not disturbed).
    if (strncmp(cmd, "Refresh=", 8) == 0) {
        std::string p = sd_norm(cmd + 8);
        if (!is_gcode(p)) { viz_say("VizErr:not a G-code file", true); return true; }
        enqueue(p, 0, VizMode::Auto);
        return true;
    }
    // Pendant: answer VizReady when the .viz exists, otherwise build it.
    if (strncmp(cmd, "Generate=", 9) == 0) { viz_generate(cmd + 9); return true; }
    if (strncmp(cmd, "Push=", 5) == 0) { viz_push_request(cmd + 5); return true; }
    if (strncmp(cmd, "Shown=", 6) == 0) { viz_job_shown(cmd + 6); return true; }
    if (strncmp(cmd, "Delete=", 7) == 0) {
        std::string     p = sd_norm(cmd + 7);
        std::error_code ec;
        FluidPath       mount { p, SD, ec };
        if (!ec) remove(viz_path(p).c_str());
        viz_say("VizDeleted", false); return true;
    }
    if (strncmp(cmd, "Status=", 7) == 0) {
        std::string     p = sd_norm(cmd + 7);
        std::error_code ec;
        FluidPath       mount { p, SD, ec };
        bool exists = !ec && viz_exists(p);
        char msg[200]; snprintf(msg, sizeof(msg), "VizStatus:%s:%s", cmd + 7, exists ? "ready" : "missing");
        viz_say(msg, false); return true;
    }
    return false;
}

void viz_init() {}
