// VizGenerator.h — FluidNC plugin: generate .viz toolpath preview files
// Drop into FluidNC/FluidNC/src/
//
// Scans a G-code file and produces a compact CSV viz file on the SD card.
// The pendant requests the .viz file using the existing $File/ShowSome
// protocol — no new response types needed.
//
// Viz file format (text CSV, readable by $File/ShowSome):
//   Line 0:  VIZ <n_points> <xmin> <xmax> <ymin> <ymax>
//   Line 1+: <x>,<y>   (float, mm, machine coordinates, one per line)
//
// Commands from pendant:
//   $Viz/Generate=/sd/file.nc   — VizReady:<viz> if it exists, else build it (incremental)
//   $Viz/Status=/sd/file.nc     — check if viz exists
//   $Viz/Delete=/sd/file.nc     — delete cached viz (force regenerate)
//
// Responses to pendant:
//   [MSG:VizReady:/sd/file.nc.viz:N:xmin:xmax:ymin:ymax]  — generation complete
//   [MSG:VizBusy:/sd/file.nc:pct]                          — generating, pct% done
//   [MSG:VizErr:reason]                                     — generation failed
//   [MSG:VizStatus:/sd/file.nc:ready|missing]               — status check response
//
// Integration: see INTEGRATION.md
#pragma once
#include <string>

bool viz_generate(const std::string& nc_path);
bool viz_exists(const std::string& nc_path);
std::string viz_path(const std::string& nc_path);
bool viz_handle_command(const char* line);

// Background builds after uploads (paths like "/sd/dir/file.nc").
//   $Viz/Refresh=/sd/file.nc  -> queue a rebuild; reports VizAutoBusy/VizAutoReady/VizAutoErr
//   (Auto messages go to every channel except the pendant UART.)
void viz_file_written(const std::string& path);  // any task; G-code on /sd only
void viz_file_removed(const std::string& path);
void viz_job_started(const std::string& path);   // $SD/Run: build the job's preview if missing  // any task; deletes the .viz too
void viz_poll();                                 // protocol task; advances the build a slice at a time
void viz_init();
