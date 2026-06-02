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
//   $Viz/Generate=/sd/file.nc   — generate viz (async, background task)
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
void viz_init();
