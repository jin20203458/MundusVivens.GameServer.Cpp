// TracyIntegration.h
// Tracy Profiler 조건부 포함 래퍼.
// TRACY_ENABLE이 정의된 경우에만 실제 Tracy 헤더를 로드하고,
// 그렇지 않은 경우 모든 매크로를 no-op으로 정의합니다.
#pragma once

#ifdef TRACY_ENABLE
#   define TRACY_ON_DEMAND
#   include <tracy/Tracy.hpp>
#else
#   define FrameMark
#   define ZoneScoped
#   define ZoneScopedN(name)
#   define TracyLockable(type, name) type name
#   define LockableBase(type) type
#endif
