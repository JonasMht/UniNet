// UniNet — the ThermoNav application message taxonomy, migrated to one source of
// truth. These string tags were previously duplicated across three peers:
//   ThermoNavMR     Assets/Scripts/SceneManager.cs:50-108
//   ThermoNavServer src/core/toolkit.h:263-340
//   ThermoNavSlicer heat_sim_module/heat_sim_module.py:136-206
// Any rename in one silently broke the others. Here they live once, in version
// control, next to the wire codec that carries them.
//
// These are application-level (ThermoNav); UniNet's transport/codec layers are
// schema-agnostic and carry any Cbor payload. Consumers adopt whichever subset
// they need.
#pragma once

namespace uninet::schema {

struct Code {
    static constexpr const char* UPDATE      = "update";
    static constexpr const char* REQUEST     = "request";
    static constexpr const char* INFORMATION = "information";
    static constexpr const char* MESSAGE     = "message";
};

struct UpdateType {
    static constexpr const char* OBJECT            = "object";
    static constexpr const char* TRANSFORM         = "transform";
    static constexpr const char* REMOVE            = "remove";
    static constexpr const char* RESET             = "reset";
    static constexpr const char* EVENTS            = "events";
    static constexpr const char* MATERIAL          = "material";
    static constexpr const char* STATE             = "state";
    static constexpr const char* STATS             = "stats";
    static constexpr const char* INSERTION_MAP     = "insertion_map";
    static constexpr const char* VERTEX_DISTANCES  = "vertex_distances";
    static constexpr const char* SAFETY_MAP        = "safety_map";
    static constexpr const char* SAFETY_TEXTURE    = "safety_texture";
    static constexpr const char* RESULT            = "result";
    static constexpr const char* METRICS           = "metrics";
    static constexpr const char* INFO              = "info";
};

struct RequestType {
    static constexpr const char* APPLICATOR       = "applicator";
    static constexpr const char* SYNC             = "sync";
    static constexpr const char* RESET            = "reset";
    static constexpr const char* NEW_CASE         = "new_case";
    static constexpr const char* VOLUME           = "volume";
    static constexpr const char* SOLVE            = "solve";
    static constexpr const char* METRICS          = "metrics";
    static constexpr const char* START_PROCEDURE  = "start_procedure";
    static constexpr const char* STOP_PROCEDURE   = "stop_procedure";
    static constexpr const char* NEXT_PROCEDURE   = "next_procedure";
    static constexpr const char* PREV_PROCEDURE   = "previous_procedure";
    static constexpr const char* START_EXPERIMENT = "start_experiment";
};

struct ObjectType {
    static constexpr const char* SURFACE     = "surface";
    static constexpr const char* APPLICATOR  = "applicator";
    static constexpr const char* VOLUME      = "volume";
};

struct ApplicatorType {
    static constexpr const char* CRYO = "cryo";
    static constexpr const char* RF   = "rf";
    static constexpr const char* MW   = "mw";
};

}  // namespace uninet::schema
