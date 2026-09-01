#include "playback/state/editing/ProjectSerializer.h"

#include "nlohmann/json.hpp"

#include <cmath>
#include <exception>

namespace playback::state::editing {

namespace {

using json = nlohmann::ordered_json;
using namespace model;

float finiteOr(json const& node, char const* key, float fallback) {
    auto const value = node.find(key);
    if (value == node.end() || !value->is_number()) return fallback;
    auto const result = value->get<float>();
    return std::isfinite(result) ? result : fallback;
}

int intOr(json const& node, char const* key, int fallback) {
    auto const value = node.find(key);
    if (value == node.end() || !value->is_number_integer()) return fallback;
    return value->get<int>();
}

bool boolOr(json const& node, char const* key, bool fallback) {
    auto const value = node.find(key);
    if (value == node.end() || !value->is_boolean()) return fallback;
    return value->get<bool>();
}

std::string stringOr(json const& node, char const* key) {
    auto const value = node.find(key);
    if (value == node.end() || !value->is_string()) return {};
    return value->get<std::string>();
}

json const* objectAt(json const& node, char const* key) {
    auto const value = node.find(key);
    return value != node.end() && value->is_object() ? &*value : nullptr;
}

json const* arrayAt(json const& node, char const* key) {
    auto const value = node.find(key);
    return value != node.end() && value->is_array() ? &*value : nullptr;
}

template <class Enum>
Enum enumOr(json const& node, char const* key, Enum fallback, int maxValue) {
    int const raw = intOr(node, key, static_cast<int>(fallback));
    if (raw < 0 || raw > maxValue) return fallback;
    return static_cast<Enum>(raw);
}

json toJson(Vec2 const& value) {
    return {
        {"x", value.x},
        {"y", value.y}
    };
}

json toJson(Vec3 const& value) {
    return {
        {"x", value.x},
        {"y", value.y},
        {"z", value.z}
    };
}

json toJson(Color4 const& value) {
    return {
        {"r", value.r},
        {"g", value.g},
        {"b", value.b},
        {"a", value.a}
    };
}

Vec2 vec2From(json const& node, char const* key, Vec2 fallback) {
    auto const* object = objectAt(node, key);
    if (!object) return fallback;
    return {finiteOr(*object, "x", fallback.x), finiteOr(*object, "y", fallback.y)};
}

Vec3 vec3From(json const& node, char const* key, Vec3 fallback) {
    auto const* object = objectAt(node, key);
    if (!object) return fallback;
    return {finiteOr(*object, "x", fallback.x), finiteOr(*object, "y", fallback.y), finiteOr(*object, "z", fallback.z)};
}

Color4 colorFrom(json const& node, char const* key, Color4 fallback) {
    auto const* object = objectAt(node, key);
    if (!object) return fallback;
    return {
        finiteOr(*object, "r", fallback.r),
        finiteOr(*object, "g", fallback.g),
        finiteOr(*object, "b", fallback.b),
        finiteOr(*object, "a", fallback.a)
    };
}

json toJson(CameraKeyframe const& key) {
    return {
        {"position",          toJson(key.position)                   },
        {"yaw",               key.yaw                                },
        {"pitch",             key.pitch                              },
        {"roll",              key.roll                               },
        {"fov",               key.fov                                },
        {"interpolationType", static_cast<int>(key.interpolationType)},
        {"bezierCtrl1",       toJson(key.bezierCtrl1)                },
        {"bezierCtrl2",       toJson(key.bezierCtrl2)                }
    };
}

CameraKeyframe keyframeFrom(json const& node) {
    CameraKeyframe key;
    key.position          = vec3From(node, "position", key.position);
    key.yaw               = finiteOr(node, "yaw", key.yaw);
    key.pitch             = finiteOr(node, "pitch", key.pitch);
    key.roll              = finiteOr(node, "roll", key.roll);
    key.fov               = finiteOr(node, "fov", key.fov);
    key.interpolationType = enumOr(
        node,
        "interpolationType",
        key.interpolationType,
        static_cast<int>(CameraInterpolationType::CubicBezier)
    );
    key.bezierCtrl1 = vec2From(node, "bezierCtrl1", key.bezierCtrl1);
    key.bezierCtrl2 = vec2From(node, "bezierCtrl2", key.bezierCtrl2);
    return key;
}

json toJson(CameraEntity const& camera) {
    auto keys = json::array();
    for (auto const& [tick, key] : camera.keysByTick) {
        auto entry    = toJson(key);
        entry["tick"] = tick;
        keys.push_back(std::move(entry));
    }

    json result{
        {"id",                camera.id               },
        {"name",              camera.name             },
        {"enabled",           camera.enabled          },
        {"locked",            camera.locked           },
        {"bindingEntityUuid", camera.bindingEntityUuid},
        {"bindingMode",       camera.bindingMode      },
        {"bindingDamping",    camera.bindingDamping   },
        {"keys",              std::move(keys)         }
    };
    if (camera.shake) {
        result["shake"] = {
            {"startTick",         camera.shake->startTick        },
            {"endTick",           camera.shake->endTick          },
            {"positionAmplitude", camera.shake->positionAmplitude},
            {"rotationAmplitude", camera.shake->rotationAmplitude},
            {"frequency",         camera.shake->frequency        }
        };
    }
    if (camera.limiter) {
        result["limiter"] = {
            {"min",     toJson(camera.limiter->min)},
            {"max",     toJson(camera.limiter->max)},
            {"enabled", camera.limiter->enabled    }
        };
    }
    return result;
}

CameraEntity cameraFrom(json const& node) {
    CameraEntity camera;
    camera.id                = stringOr(node, "id");
    camera.name              = stringOr(node, "name");
    camera.enabled           = boolOr(node, "enabled", true);
    camera.locked            = boolOr(node, "locked", false);
    camera.bindingEntityUuid = stringOr(node, "bindingEntityUuid");
    camera.bindingMode       = intOr(node, "bindingMode", 0);
    camera.bindingDamping    = finiteOr(node, "bindingDamping", 0.1f);

    if (auto const* keys = arrayAt(node, "keys")) {
        for (auto const& entry : *keys) {
            if (!entry.is_object()) continue;
            camera.keysByTick.insert_or_assign(intOr(entry, "tick", 0), keyframeFrom(entry));
        }
    }
    if (auto const* shake = objectAt(node, "shake")) {
        camera.shake = CameraShake{
            intOr(*shake, "startTick", 0),
            intOr(*shake, "endTick", 0),
            finiteOr(*shake, "positionAmplitude", 0.0f),
            finiteOr(*shake, "rotationAmplitude", 0.0f),
            finiteOr(*shake, "frequency", 1.0f)
        };
    }
    if (auto const* limiter = objectAt(node, "limiter")) {
        camera.limiter = CameraLimiter{
            vec3From(*limiter, "min", {}),
            vec3From(*limiter, "max", {}),
            boolOr(*limiter, "enabled", false)
        };
    }
    return camera;
}

json toJson(SequenceSegment const& segment) {
    return {
        {"id",        segment.id           },
        {"startTick", segment.startTick    },
        {"endTick",   segment.endTick      },
        {"cameraId",  segment.cameraId     },
        {"color",     toJson(segment.color)},
        {"locked",    segment.locked       }
    };
}

SequenceSegment sequenceSegmentFrom(json const& node) {
    SequenceSegment segment;
    segment.id        = stringOr(node, "id");
    segment.startTick = intOr(node, "startTick", 0);
    segment.endTick   = intOr(node, "endTick", 0);
    segment.cameraId  = stringOr(node, "cameraId");
    segment.color     = colorFrom(node, "color", segment.color);
    segment.locked    = boolOr(node, "locked", false);
    return segment;
}

json toJson(WorldActorSegment const& segment) {
    return {
        {"id",         segment.id           },
        {"startTick",  segment.startTick    },
        {"endTick",    segment.endTick      },
        {"sourceTick", segment.sourceTick   },
        {"speed",      segment.speed        },
        {"color",      toJson(segment.color)},
        {"locked",     segment.locked       }
    };
}

WorldActorSegment worldActorSegmentFrom(json const& node) {
    WorldActorSegment segment;
    segment.id         = stringOr(node, "id");
    segment.startTick  = intOr(node, "startTick", 0);
    segment.endTick    = intOr(node, "endTick", 0);
    segment.sourceTick = intOr(node, "sourceTick", 0);
    segment.speed      = finiteOr(node, "speed", 1.0f);
    segment.color      = colorFrom(node, "color", segment.color);
    segment.locked     = boolOr(node, "locked", false);
    return segment;
}

json toJson(SubActor const& actor) {
    auto details = json::object();
    for (auto const& [key, value] : actor.agentDetails) details[key] = value;

    return {
        {"id",             actor.id                        },
        {"name",           actor.name                      },
        {"category",       static_cast<int>(actor.category)},
        {"position",       toJson(actor.position)          },
        {"rotation",       toJson(actor.rotation)          },
        {"agentDetails",   std::move(details)              },
        {"boundCameraIds", actor.boundCameraIds            }
    };
}

SubActor subActorFrom(json const& node) {
    SubActor actor;
    actor.id       = stringOr(node, "id");
    actor.name     = stringOr(node, "name");
    actor.category = enumOr(node, "category", actor.category, static_cast<int>(SubActorCategory::Entities));
    actor.position = vec3From(node, "position", {});
    actor.rotation = vec2From(node, "rotation", {});
    if (auto const* details = objectAt(node, "agentDetails")) {
        for (auto const& [key, value] : details->items()) {
            if (value.is_string()) actor.agentDetails.insert_or_assign(key, value.get<std::string>());
        }
    }
    if (auto const* bound = arrayAt(node, "boundCameraIds")) {
        for (auto const& entry : *bound) {
            if (entry.is_string()) actor.boundCameraIds.push_back(entry.get<std::string>());
        }
    }
    return actor;
}

json toJson(Marker const& marker) {
    return {
        {"id",    marker.id   },
        {"label", marker.label},
        {"tick",  marker.tick }
    };
}

json toJson(Clip const& clip) {
    return {
        {"id",                   clip.id                  },
        {"replayFile",           clip.replayFile          },
        {"inTick",               clip.inTick              },
        {"outTick",              clip.outTick             },
        {"trackTick",            clip.trackTick           },
        {"activeCameraTrackIdx", clip.activeCameraTrackIdx},
        {"speed",                clip.speed               },
        {"name",                 clip.name                },
        {"color",                toJson(clip.color)       },
        {"muted",                clip.muted               },
        {"locked",               clip.locked              }
    };
}

Clip clipFrom(json const& node) {
    Clip clip;
    clip.id                   = stringOr(node, "id");
    clip.replayFile           = stringOr(node, "replayFile");
    clip.inTick               = intOr(node, "inTick", 0);
    clip.outTick              = intOr(node, "outTick", 0);
    clip.trackTick            = intOr(node, "trackTick", 0);
    clip.activeCameraTrackIdx = intOr(node, "activeCameraTrackIdx", 0);
    clip.speed                = finiteOr(node, "speed", 1.0f);
    clip.name                 = stringOr(node, "name");
    clip.color                = colorFrom(node, "color", clip.color);
    clip.muted                = boolOr(node, "muted", false);
    clip.locked               = boolOr(node, "locked", false);
    return clip;
}

json toJson(Track const& track) {
    auto clips = json::array();
    for (auto const& clip : track.clips) clips.push_back(toJson(clip));

    return {
        {"id",      track.id                    },
        {"name",    track.name                  },
        {"kind",    static_cast<int>(track.kind)},
        {"visible", track.visible               },
        {"locked",  track.locked                },
        {"height",  track.height                },
        {"clips",   std::move(clips)            }
    };
}

Track trackFrom(json const& node) {
    Track track;
    track.id      = stringOr(node, "id");
    track.name    = stringOr(node, "name");
    track.kind    = enumOr(node, "kind", track.kind, static_cast<int>(TrackKind::Marker));
    track.visible = boolOr(node, "visible", true);
    track.locked  = boolOr(node, "locked", false);
    track.height  = intOr(node, "height", 48);
    if (auto const* clips = arrayAt(node, "clips")) {
        for (auto const& entry : *clips) {
            if (entry.is_object()) track.clips.push_back(clipFrom(entry));
        }
    }
    return track;
}

json toJson(Transition const& transition) {
    return {
        {"id",            transition.id                    },
        {"kind",          static_cast<int>(transition.kind)},
        {"durationTicks", transition.durationTicks         },
        {"easing",        transition.easing                },
        {"fromClipId",    transition.fromClipId            },
        {"toClipId",      transition.toClipId              },
        {"fadeColor",     toJson(transition.fadeColor)     }
    };
}

Transition transitionFrom(json const& node) {
    Transition transition;
    transition.id            = stringOr(node, "id");
    transition.kind          = enumOr(node, "kind", transition.kind, static_cast<int>(TransitionKind::CrossDissolve));
    transition.durationTicks = intOr(node, "durationTicks", 20);
    transition.easing        = intOr(node, "easing", 0);
    transition.fromClipId    = stringOr(node, "fromClipId");
    transition.toClipId      = stringOr(node, "toClipId");
    transition.fadeColor     = colorFrom(node, "fadeColor", transition.fadeColor);
    return transition;
}

} // namespace

std::string serializeProject(model::EditorStateExt const& project) {
    auto cameras = json::array();
    for (auto const& camera : project.cameras) cameras.push_back(toJson(camera));

    auto sequence = json::array();
    for (auto const& segment : project.sequence) sequence.push_back(toJson(segment));

    auto worldSegments = json::array();
    for (auto const& segment : project.worldActor.segments) worldSegments.push_back(toJson(segment));

    auto subActors = json::array();
    for (auto const& actor : project.worldActor.subActors) subActors.push_back(toJson(actor));

    auto markers = json::array();
    for (auto const& marker : project.markers) markers.push_back(toJson(marker));

    auto videoTracks = json::array();
    for (auto const& track : project.videoTracks) videoTracks.push_back(toJson(track));

    auto transitions = json::array();
    for (auto const& transition : project.transitions) transitions.push_back(toJson(transition));

    json const document{
        {"formatVersion",       kProjectFormatVersion      },
        {"projectName",         project.projectName        },
        {"replayPath",          project.projectPath        },
        {"totalTicks",          project.totalTicks         },
        {"currentTick",         project.currentTick        },
        {"fps",                 project.fps                },
        {"activeVideoTrackIdx", project.activeVideoTrackIdx},
        {"cameras",             std::move(cameras)         },
        {"sequence",            std::move(sequence)        },
        {"worldActor",
         json{
             {"id", project.worldActor.id},
             {"name", project.worldActor.name},
             {"totalTicks", project.worldActor.totalTicks},
             {"segments", std::move(worldSegments)},
             {"subActors", std::move(subActors)}
         }                                                 },
        {"markers",             std::move(markers)         },
        {"videoTracks",         std::move(videoTracks)     },
        {"transitions",         std::move(transitions)     }
    };
    return document.dump(2);
}

bool deserializeProject(std::string const& payload, model::EditorStateExt& project, std::string& error) {
    json document;
    try {
        document = json::parse(payload);
    } catch (std::exception const& e) {
        error = e.what();
        return false;
    }
    if (!document.is_object()) {
        error = "project file is not a JSON object";
        return false;
    }

    int const formatVersion = intOr(document, "formatVersion", 0);
    if (formatVersion <= 0 || formatVersion > kProjectFormatVersion) {
        error = "unsupported project format version " + std::to_string(formatVersion);
        return false;
    }

    model::EditorStateExt loaded;
    loaded.projectName         = stringOr(document, "projectName");
    loaded.projectPath         = stringOr(document, "replayPath");
    loaded.totalTicks          = intOr(document, "totalTicks", 0);
    loaded.currentTick         = intOr(document, "currentTick", 0);
    loaded.fps                 = finiteOr(document, "fps", 60.0f);
    loaded.activeVideoTrackIdx = intOr(document, "activeVideoTrackIdx", 0);

    if (auto const* cameras = arrayAt(document, "cameras")) {
        for (auto const& entry : *cameras) {
            if (entry.is_object()) loaded.cameras.push_back(cameraFrom(entry));
        }
    }
    if (auto const* sequence = arrayAt(document, "sequence")) {
        for (auto const& entry : *sequence) {
            if (entry.is_object()) loaded.sequence.push_back(sequenceSegmentFrom(entry));
        }
    }
    if (auto const* worldActor = objectAt(document, "worldActor")) {
        loaded.worldActor.id         = stringOr(*worldActor, "id");
        loaded.worldActor.name       = stringOr(*worldActor, "name");
        loaded.worldActor.totalTicks = intOr(*worldActor, "totalTicks", loaded.totalTicks);
        if (auto const* segments = arrayAt(*worldActor, "segments")) {
            for (auto const& entry : *segments) {
                if (entry.is_object()) loaded.worldActor.segments.push_back(worldActorSegmentFrom(entry));
            }
        }
        if (auto const* subActors = arrayAt(*worldActor, "subActors")) {
            for (auto const& entry : *subActors) {
                if (entry.is_object()) loaded.worldActor.subActors.push_back(subActorFrom(entry));
            }
        }
    }
    if (auto const* markers = arrayAt(document, "markers")) {
        for (auto const& entry : *markers) {
            if (!entry.is_object()) continue;
            loaded.markers.push_back({stringOr(entry, "id"), stringOr(entry, "label"), intOr(entry, "tick", 0)});
        }
    }
    if (auto const* tracks = arrayAt(document, "videoTracks")) {
        for (auto const& entry : *tracks) {
            if (entry.is_object()) loaded.videoTracks.push_back(trackFrom(entry));
        }
    }
    if (auto const* transitions = arrayAt(document, "transitions")) {
        for (auto const& entry : *transitions) {
            if (entry.is_object()) loaded.transitions.push_back(transitionFrom(entry));
        }
    }

    project = std::move(loaded);
    return true;
}

} // namespace playback::state::editing
