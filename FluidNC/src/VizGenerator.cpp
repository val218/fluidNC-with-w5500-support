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
#define VIZ_SAMPLE_EVERY  1   // keep every cut unless the file is large (see thinning)
#define VIZ_MIN_DIST_MM   0.5f
#define VIZ_ARC_SEGMENTS  16


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
};
static VizBuild         _b;
static std::atomic<bool> _cancel { false };
static std::string      _current;  // path being built (guarded by _q_mutex)

static void emit(float x, float y, float z, int t, int line) {
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
    for (int s = 1; s <= _arc_segments && _b.n_points < VIZ_MAX_POINTS; s++) {
        float t = (float)s / _arc_segments;
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

    // File size -> thinning. ~22 bytes per G-code line is typical CAM output;
    // aim to use ~85% of the point budget over the whole file.
    if (fseek(_b.in, 0, SEEK_END) == 0) _b.file_size = ftell(_b.in);
    fseek(_b.in, 0, SEEK_SET);
    {
        const long est_lines = _b.file_size / 22;
        const long budget    = (VIZ_MAX_POINTS * 85L) / 100;
        _sample_every        = VIZ_SAMPLE_EVERY;
        _arc_segments        = VIZ_ARC_SEGMENTS;
        if (est_lines > budget * VIZ_SAMPLE_EVERY) {
            _sample_every = (int)((est_lines + budget - 1) / budget);
            _arc_segments = std::max(3, VIZ_ARC_SEGMENTS * VIZ_SAMPLE_EVERY / _sample_every);
        }
    }

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
        int   t      = (motion == 0) ? 0 : 1;
        float dx     = new_x - _b.last_px, dy = new_y - _b.last_py;
        bool  first  = _b.last_px > 1e8f;
        bool  zchg   = fabsf(new_z - _b.last_pz) > 0.001f;
        bool  tchg   = t != _b.last_t;
        bool  far    = sqrtf(dx*dx + dy*dy) >= VIZ_MIN_DIST_MM;
        bool  keep   = first || zchg || tchg || t == 0 || (far && ++_b.sample_ct >= _sample_every);
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
    // " v3": generator version (x,y,z,type,line points); older files are rebuilt.
    char head[HEADER_W + 1];
    int  n = snprintf(head, sizeof(head), "VIZ %d %.3f %.3f %.3f %.3f v3", _b.n_points, _b.xmin, _b.xmax, _b.ymin, _b.ymax);
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

// Advance the current build until `budget_us` is used up.
static void build_step(int64_t budget_us) {
    if (_cancel) { build_close(false); return; }
    const int64_t t0 = esp_timer_get_time();
    char linebuf[256];
    int  n = 0;
    while (_b.n_points < VIZ_MAX_POINTS) {
        if (!read_line(linebuf, sizeof(linebuf))) {
            if (ferror(_b.in)) { errno = EIO; build_fail("read error", _b.nc); return; }
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
                    snprintf(msg, sizeof(msg), "VizBusy:%s:%d", _b.nc.c_str(),
                             _b.file_size > 0 ? (int)(pos * 100 / _b.file_size) : 0);
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
    bool ok = fgets(head, sizeof(head), f) && strstr(head, " v3");
    fclose(f);
    return ok;
}


// ---------------------------------------------------------------------------
// Push a .viz to the pendant ($Viz/Push=). FluidNC sends the points, the
// pendant only listens - no file reads from the pendant, no request/reply
// matching. Messages go to the pendant UART only:
//   [MSG:VZB:<n>:<xmin>:<xmax>:<ymin>:<ymax>:<viz path>]   begin
//   [MSG:VZ:<seq>:<x,y,z,t,ln>;<x,y,z,t,ln>;...]           up to 4 points
//   [MSG:VZE:<count>:<viz path>]                           end
//   [MSG:VZX:<viz path>:<reason>]                          cannot send it
// Paced from viz_poll() so the UART and the output queue are never flooded.
// ---------------------------------------------------------------------------
struct VizPush {
    bool                       active = false;
    std::string                path;  // .viz
    std::unique_ptr<FluidPath> mount;
    FILE*                      f     = nullptr;
    int                        seq   = 0;
    int                        count = 0;
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
}

static void push_begin(const std::string& vpath) {
    push_end();
    if (!pendant_channel()) return;
    std::error_code ec;
    _p.mount.reset(new FluidPath(vpath, SD, ec));
    if (!ec) _p.f = fopen(vpath.c_str(), "r");
    char head[HEADER_W + 8] = {};
    int  n = 0;
    float b[4] = {};
    if (!_p.f || !fgets(head, sizeof(head), _p.f) || !strstr(head, " v3") ||
        sscanf(head, "VIZ %d %f %f %f %f", &n, &b[0], &b[1], &b[2], &b[3]) != 5) {
        to_pendant("[MSG:VZX:" + vpath + ":" + (_p.f ? "old format" : "missing") + "]");
        push_end();
        return;
    }
    char buf[256];
    snprintf(buf, sizeof(buf), "[MSG:VZB:%d:%.3f:%.3f:%.3f:%.3f:%s]", n, b[0], b[1], b[2], b[3], vpath.c_str());
    to_pendant(buf);
    _p.path   = vpath;
    _p.seq    = 0;
    _p.count  = 0;
    _p.active = true;
    _p.last_ms = 0;
    snprintf(buf, sizeof(buf), "[MSG:VizPush:%s:%d points]\r\n", vpath.c_str(), n);
    allChannels.print_except(buf, static_cast<Channel*>(pendant_channel()));
}

// Send a few messages; false when finished.
static void push_step() {
    if (!_p.active) return;
    uint32_t now = millis();
    const bool job = Job::active();
    if (now - _p.last_ms < (job ? 20u : 8u)) return;
    if (uxQueueMessagesWaiting(message_queue) > 16) return;  // let the output drain first
    _p.last_ms = now;
    const int msgs = job ? 3 : 6;
    char line[64];
    for (int m = 0; m < msgs; ++m) {
        std::string out = "[MSG:VZ:" + std::to_string(_p.seq) + ":";
        int         pts = 0;
        while (pts < 4 && fgets(line, sizeof(line), _p.f)) {
            size_t len = strlen(line);
            while (len && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' ')) line[--len] = '\0';
            if (!len || !strchr(line, ',')) continue;
            if (pts) out += ';';
            out += line;
            ++pts;
        }
        if (pts) {
            out += ']';
            to_pendant(out);
            ++_p.seq;
            _p.count += pts;
        }
        if (pts < 4) {  // end of file
            to_pendant("[MSG:VZE:" + std::to_string(_p.count) + ":" + _p.path + "]");
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

void viz_poll() {
    static uint32_t last_slice_ms = 0;
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
static void viz_push_request(const std::string& nc_path) {
    std::string p = sd_norm(nc_path);
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
