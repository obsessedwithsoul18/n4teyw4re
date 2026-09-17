#pragma once
#include <string>

namespace launcher
{
    // Re-launches this executable with |extra_args| appended to the current
    // command line via ShellExecuteEx runas (admin + UIAccess).
    // The caller should exit immediately after this returns true.
    bool relaunch_self( const std::wstring& extra_args );

    // Extracts the appropriate embedded driver resource into a temp file,
    // maps it directly into the kernel using the in-process kdmapper library,
    // then deletes the temp file.
    //
    // Returns an empty string on success, or a human-readable error message.
    std::string map_driver( bool km_mode );
}
