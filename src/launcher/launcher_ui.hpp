#pragma once

namespace launcher_ui
{
    // Creates a D3D11 popup window, runs the mode-selection + driver-mapping
    // UI, then re-launches the current executable with --n4teyw4re-ready and
    // the chosen mode flag before returning.
    //
    // Returns false in all cases — the caller should exit immediately after
    // this returns (the re-launched process carries on as the real app).
    bool run();
}
