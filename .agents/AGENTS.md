# C++ Game Server Rules
- **Execution**: Run from repository root only. No raw `cmake` commands from terminal.
- **Build**: `powershell -ExecutionPolicy Bypass -File .\build_local.ps1`
- **Clean Build (Keep vcpkg)**: `powershell -ExecutionPolicy Bypass -File .\build_local.ps1 -Clean`
- **Paths**: Use relative paths only (e.g., `../Obsidian.Agent/`).
- **SSOT Docs**: Update specs in `../MundusVivens/docs/` synchronously with any code changes.
- **Troubleshooting**: Log resolved C++ server bugs in `../Obsidian.Agent/troubleshooting/mundus_vivens.md`.