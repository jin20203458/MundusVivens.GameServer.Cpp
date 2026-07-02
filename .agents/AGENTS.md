# C++ Game Server Build Rules
- **Do not run raw CMake commands directly**: Never run raw CMake commands (e.g. `cmake -B out/build -S .`) from standard terminal.
- **Use build script**: Compile with `powershell -ExecutionPolicy Bypass -File .\build_local.ps1`.
- **Safe cache clean**: To reset cache without losing 30-min vcpkg builds, use `powershell -ExecutionPolicy Bypass -File .\build_local.ps1 -Clean`.
