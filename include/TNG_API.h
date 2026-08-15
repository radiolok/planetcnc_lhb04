// TNG_API.h
//
// Declarations of the PlanetCNC TNG C API (PlanetCNCLib64.dll /
// libPlanetCNCLib64.so).
//
// The SDK does not ship a public header, so these declarations are derived
// from "PlanetCNC_TNG_API.pdf" (API Reference, 2020/07/21). All functions use
// the CDECL calling convention. This header is used only for documentation and
// for the function-pointer typedefs consumed by TngApi; nothing is linked
// against it. Symbols are resolved at runtime via LoadLibrary/dlopen and
// GetProcAddress/dlsym.
//
// NOTE: dumpbin on the local PlanetCNCLib64.dll shows an export named "IsRun"
//       while the manual documents "IsRunning". TngApi resolves both names and
//       prefers "IsRunning".

#ifndef MPGD_TNG_API_H
#define MPGD_TNG_API_H

#include <cstdint>

namespace mpgd::tng {

// --- Callbacks (all CDECL) -----------------------------------------------
using InitialiseCB = void (*)(int);        // 1 = ready, 0 = shutdown
using RefreshCB    = void (*)();
using IdleCB       = void (*)();
using LineNumCB    = void (*)(int);
using OpenCB       = void (*)(int);
using OutputCB     = void (*)(const char*);

// --- Raw function-pointer types matching the documented signatures --------
using Fn_Run                = int   (*)(bool);
using Fn_RunProfile         = int   (*)(bool, const char*);
using Fn_Exit               = void  (*)();
using Fn_GetVer             = int   (*)();
using Fn_BoolVoid           = bool  (*)();
using Fn_BoolBool           = bool  (*)(bool);
using Fn_GetCmdId           = int   (*)(const char*);
using Fn_CmdExec            = bool  (*)(int);
using Fn_CmdExecStr         = bool  (*)(int, const char*);
using Fn_CmdExecVal         = bool  (*)(int, double);
using Fn_SetParam           = bool  (*)(const char*, double);
using Fn_GetParam           = double(*)(const char*);
using Fn_StartCode          = bool  (*)(const char*);
using Fn_OpenCode           = bool  (*)(const char*);
using Fn_InfoPos3           = bool  (*)(double*, double*, double*);
using Fn_InfoPosAxis        = double(*)(int);
using Fn_InfoSpeed          = double(*)();
using Fn_InfoJogPot         = unsigned int (*)();
using Fn_Jog                = bool  (*)(bool, double, double, double);
using Fn_Jog9               = bool  (*)(bool, double, double, double, double, double, double, double, double, double);
using Fn_JogStop            = bool  (*)();
using Fn_MoveAxis           = bool  (*)(double, int, double);
using Fn_SetInitialiseCB    = void  (*)(InitialiseCB);
using Fn_SetRefreshCB       = void  (*)(RefreshCB);
using Fn_SetIdleCB          = void  (*)(IdleCB);
using Fn_SetLineNumCB       = void  (*)(LineNumCB);

} // namespace mpgd::tng

#endif // MPGD_TNG_API_H
