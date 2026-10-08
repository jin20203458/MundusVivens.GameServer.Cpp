<project_philosophy>
Focus: 20Hz lock-free simulation main loop, EnTT cache-coherent DOD, thread boundary isolation, and Boost.Asio/asio-grpc asynchronous networking.
</project_philosophy>

<engineering_rules>
- **Thread Safety**: gRPC/IO threads MUST NOT directly modify `entt::registry`. Main Thread exclusively owns EnTT registry write access; cross-thread tasks must be pushed to Main Thread via queue.
- **Memory**: Prefer smart pointers for resource ownership. Use raw pointers only when technically required (e.g., non-owning observers, POD buffers).
- **ECS**: State in components, logic in systems. NO OOP inheritance for entities.
- **Formatting**: Strictly follow the target file's style; zero decorative emojis.
</engineering_rules>

<critical_rules>
- **Build**: `powershell -ExecutionPolicy Bypass -File .\build_local.ps1` (NO raw CMake)
- **Paths**: Use relative paths (`../Obsidian.Agent/`, etc.).
</critical_rules>

<context_triggers>
- **Server Architecture**: 20Hz loop, EnTT ECS, gRPC streaming, thread isolation -> `../Obsidian.Agent/MundusVivens/docs/01_game_server_architecture.md`
- **Troubleshooting**: Concurrency bugs, crash post-mortems, runbook -> `../Obsidian.Agent/troubleshooting/mundus_vivens.md`
</context_triggers>

<post_action>
- **Log**: Document resolved bugs in `../Obsidian.Agent/troubleshooting/mundus_vivens.md`. (Ignore simple refactors/optimizations)
- **Sync**: Update specs in `../Obsidian.Agent/MundusVivens/docs/` if architecture changes.
</post_action>