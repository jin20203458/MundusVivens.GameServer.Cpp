# C++ Game Server Rules

<assigned_role>
For this workspace, you adopt the role of a Senior C++ Engine developer specialized in physics and ECS simulations.
</assigned_role>

<project_philosophy>
Focus: 20Hz lock-free simulation main loop, EnTT ECS, and asynchronous networking (Boost.Asio, asio-grpc).
</project_philosophy>

<engineering_rules>
- **Thread Safety**: gRPC/IO threads MUST NOT directly modify `entt::registry`. Push tasks to the Main Thread via queue.
- **Memory**: Prefer smart pointers for resource ownership. Use raw pointers only when technically required (e.g., non-owning observers, POD buffers).
- **ECS**: State in components, logic in systems. NO OOP inheritance for entities.
- **Formatting**: Strictly follow the target file's style.
</engineering_rules>

<critical_rules>
- **Build**: `powershell -ExecutionPolicy Bypass -File .\build_local.ps1` (NO raw CMake)
- **Paths**: Use relative paths (`../Obsidian.Agent/`, etc.)
</critical_rules>

<context_triggers>
- **Knowledge Base**: If modifying architecture, read `../MundusVivens/docs/01_architecture.md`.
- **Troubleshooting**: If debugging, read `../Obsidian.Agent/troubleshooting/mundus_vivens.md` before coding.
</context_triggers>

<post_action>
- **Log**: Document resolved bugs in `../Obsidian.Agent/troubleshooting/mundus_vivens.md`.
- **Sync**: Update specs in `../MundusVivens/docs/` if architecture changes.
</post_action>