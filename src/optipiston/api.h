#pragma once

// Stable C ABI for other mods. Look it up at runtime; OptiPiston stays an optional dependency.

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

#define OPTIPISTON_API_VERSION     1u
#define OPTIPISTON_API_EXPORT_NAME "optipiston_get_api"

typedef struct OptiPistonApiV1 {
    // sizeof(OptiPistonApiV1) as built by OptiPiston; newer fields are only appended.
    uint32_t size;
    uint32_t version;
    // Drive piston visuals from an external tick; bump epoch on any jump (seek, clock switch, export start).
    void (*set_external_clock)(int64_t tick, uint64_t epoch);
    // Frame fraction in [0, 1] for the external clock; negative returns to the native render alpha.
    void (*set_external_partial)(float partial);
    // Return to the local level tick.
    void (*clear_external_clock)(void);
    // Setters clamp to the ranges below and persist to OptiPiston's config.
    int (*get_piston_enabled)(void);
    void (*set_piston_enabled)(int enabled);
    // Visual length of a piston move in game ticks; fractions allowed.
    float (*get_piston_duration)(void);
    void (*set_piston_duration)(float ticks);
    float min_piston_duration;
    float max_piston_duration;
} OptiPistonApiV1;

// Returns null if the running OptiPiston is older than min_version.
typedef OptiPistonApiV1 const* (*OptiPistonGetApiFn)(uint32_t min_version);

#ifdef __cplusplus
}
#endif
