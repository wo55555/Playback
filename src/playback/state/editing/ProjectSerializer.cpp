#include "playback/state/editing/ProjectSerializer.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <unordered_set>

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

double doubleOr(json const& node, char const* key, double fallback) {
    auto const value = node.find(key);
    if (value == node.end() || !value->is_number()) return fallback;
    auto const result = value->get<double>();
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
        {"position",                  toJson(key.position)                   },
        {"yaw",                       key.yaw                                },
        {"pitch",                     key.pitch                              },
        {"roll",                      key.roll                               },
        {"fov",                       key.fov                                },
        {"mode",                      static_cast<int>(key.mode)             },
        {"interpolationType",         static_cast<int>(key.interpolationType)},
        {"bezierCtrl1",               toJson(key.bezierCtrl1)                },
        {"bezierCtrl2",               toJson(key.bezierCtrl2)                },
        {"subject",                   key.subject                            },
        {"ignoreClipSpeed",           key.ignoreClipSpeed                    },
        {"thirdPersonCustom",         key.thirdPersonCustom                  },
        {"thirdPersonDistance",       key.thirdPersonDistance                },
        {"thirdPersonYaw",            key.thirdPersonYaw                     },
        {"thirdPersonPitch",          key.thirdPersonPitch                   },
        {"thirdPersonRoll",           key.thirdPersonRoll                    },
        {"thirdPersonFov",            key.thirdPersonFov                     },
        {"thirdPersonOffset",         toJson(key.thirdPersonOffset)          },
        {"thirdPersonFollowRotation", key.thirdPersonFollowRotation          },
        {"thirdPersonCollideBlocks",  key.thirdPersonCollideBlocks           },
        {"lookAtEnabled",             key.lookAtEnabled                      },
        {"lookAtTarget",              key.lookAtTarget                       },
        {"lookAtOffsetY",             key.lookAtOffsetY                      },
    };
}

CameraKeyframe keyframeFrom(json const& node) {
    CameraKeyframe key;
    key.position          = vec3From(node, "position", key.position);
    key.yaw               = finiteOr(node, "yaw", key.yaw);
    key.pitch             = finiteOr(node, "pitch", key.pitch);
    key.roll              = finiteOr(node, "roll", key.roll);
    key.fov               = finiteOr(node, "fov", key.fov);
    key.mode              = enumOr(node, "mode", key.mode, static_cast<int>(CameraKeyframeMode::ThirdPerson));
    key.interpolationType = enumOr(
        node,
        "interpolationType",
        key.interpolationType,
        static_cast<int>(CameraInterpolationType::CubicBezier)
    );
    key.bezierCtrl1               = vec2From(node, "bezierCtrl1", key.bezierCtrl1);
    key.bezierCtrl2               = vec2From(node, "bezierCtrl2", key.bezierCtrl2);
    key.subject                   = stringOr(node, "subject");
    key.ignoreClipSpeed           = boolOr(node, "ignoreClipSpeed", false);
    key.thirdPersonCustom         = boolOr(node, "thirdPersonCustom", false);
    key.thirdPersonDistance       = std::max(0.0f, finiteOr(node, "thirdPersonDistance", key.thirdPersonDistance));
    key.thirdPersonYaw            = finiteOr(node, "thirdPersonYaw", key.thirdPersonYaw);
    key.thirdPersonPitch          = finiteOr(node, "thirdPersonPitch", key.thirdPersonPitch);
    key.thirdPersonRoll           = finiteOr(node, "thirdPersonRoll", key.thirdPersonRoll);
    key.thirdPersonFov            = std::clamp(finiteOr(node, "thirdPersonFov", key.thirdPersonFov), 1.0f, 179.0f);
    key.thirdPersonOffset         = vec3From(node, "thirdPersonOffset", key.thirdPersonOffset);
    key.thirdPersonFollowRotation = boolOr(node, "thirdPersonFollowRotation", false);
    key.thirdPersonCollideBlocks  = boolOr(node, "thirdPersonCollideBlocks", true);
    key.lookAtEnabled             = boolOr(node, "lookAtEnabled", false);
    key.lookAtTarget              = stringOr(node, "lookAtTarget");
    key.lookAtOffsetY             = finiteOr(node, "lookAtOffsetY", 0.0f);
    return key;
}

json toJson(Clip const& clip);
Clip clipFrom(json const& node);

json toJson(CameraEntity const& camera) {
    auto keys = json::array();
    for (auto const& [tick, key] : camera.keysByTick) {
        auto entry    = toJson(key);
        entry["tick"] = tick;
        keys.push_back(std::move(entry));
    }

    auto clips = json::array();
    for (auto const& clip : camera.clips) clips.push_back(toJson(clip));

    json result{
        {"id",      camera.id       },
        {"name",    camera.name     },
        {"enabled", camera.enabled  },
        {"locked",  camera.locked   },
        {"keys",    std::move(keys) },
        {"clips",   std::move(clips)}
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
    camera.id            = stringOr(node, "id");
    camera.name          = stringOr(node, "name");
    int const legacyKind = intOr(node, "kind", 0);
    camera.enabled       = boolOr(node, "enabled", true);
    camera.locked        = boolOr(node, "locked", false);

    if (auto const* keys = arrayAt(node, "keys")) {
        for (auto const& entry : *keys) {
            if (!entry.is_object()) continue;
            auto key = keyframeFrom(entry);
            // Migrate legacy track-level person kinds onto keyframes that lack an explicit mode.
            if (!entry.contains("mode") && (legacyKind == 1 || legacyKind == 2)) {
                key.mode = legacyKind == 2 ? CameraKeyframeMode::ThirdPerson : CameraKeyframeMode::FirstPerson;
            }
            camera.keysByTick.insert_or_assign(intOr(entry, "tick", 0), std::move(key));
        }
    }
    if (auto const* clips = arrayAt(node, "clips")) {
        for (auto const& entry : *clips) {
            if (entry.is_object()) camera.clips.push_back(clipFrom(entry));
        }
        std::ranges::stable_sort(camera.clips, {}, &Clip::inTick);
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

json toJson(Marker const& marker) {
    return {
        {"id",    marker.id   },
        {"label", marker.label},
        {"tick",  marker.tick }
    };
}

json toJson(Clip const& clip) {
    return {
        {"id",            clip.id           },
        {"replayFile",    clip.replayFile   },
        {"inTick",        clip.inTick       },
        {"outTick",       clip.outTick      },
        {"trackTick",     clip.trackTick    },
        {"name",          clip.name         },
        {"color",         toJson(clip.color)},
        {"locked",        clip.locked       },
        {"exportEnabled", clip.exportEnabled}
    };
}

Clip clipFrom(json const& node) {
    Clip clip;
    clip.id            = stringOr(node, "id");
    clip.replayFile    = stringOr(node, "replayFile");
    clip.inTick        = std::max(0, intOr(node, "inTick", 0));
    clip.outTick       = std::max(clip.inTick, intOr(node, "outTick", 0));
    clip.trackTick     = intOr(node, "trackTick", 0);
    clip.name          = stringOr(node, "name");
    clip.color         = colorFrom(node, "color", clip.color);
    clip.locked        = boolOr(node, "locked", false);
    clip.exportEnabled = boolOr(node, "exportEnabled", true);
    return clip;
}

json toJson(AudioClip const& clip) {
    return {
        {"id",               clip.id              },
        {"name",             clip.name            },
        {"filePath",         clip.filePath        },
        {"startTick",        clip.startTick       },
        {"trimStartSeconds", clip.trimStartSeconds},
        {"durationSeconds",  clip.durationSeconds },
        {"volume",           clip.volume          },
        {"muted",            clip.muted           }
    };
}

AudioClip audioClipFrom(json const& node) {
    AudioClip clip;
    clip.id               = stringOr(node, "id");
    clip.name             = stringOr(node, "name");
    clip.filePath         = stringOr(node, "filePath");
    clip.startTick        = std::max(0, intOr(node, "startTick", 0));
    clip.trimStartSeconds = clampAudioSeconds(doubleOr(node, "trimStartSeconds", 0.0));
    clip.durationSeconds  = clampAudioSeconds(doubleOr(node, "durationSeconds", 0.0));
    clip.volume           = clampAudioVolume(finiteOr(node, "volume", 1.0f));
    clip.muted            = boolOr(node, "muted", false);
    return clip;
}

json toJson(AudioTrack const& track) {
    auto clips = json::array();
    for (auto const& clip : track.clips) clips.push_back(toJson(clip));
    return {
        {"id",      track.id        },
        {"name",    track.name      },
        {"enabled", track.enabled   },
        {"muted",   track.muted     },
        {"locked",  track.locked    },
        {"volume",  track.volume    },
        {"clips",   std::move(clips)}
    };
}

AudioTrack audioTrackFrom(json const& node) {
    AudioTrack track;
    track.id      = stringOr(node, "id");
    track.name    = stringOr(node, "name");
    track.enabled = boolOr(node, "enabled", true);
    track.muted   = boolOr(node, "muted", false);
    track.locked  = boolOr(node, "locked", false);
    track.volume  = clampAudioVolume(finiteOr(node, "volume", 1.0f));
    if (auto const* clips = arrayAt(node, "clips")) {
        for (auto const& entry : *clips) {
            if (entry.is_object()) track.clips.push_back(audioClipFrom(entry));
        }
        std::ranges::stable_sort(track.clips, {}, &AudioClip::startTick);
    }
    return track;
}

std::vector<AudioTrack> audioTracksFrom(json const& node) {
    std::vector<AudioTrack> tracks;
    if (auto const* arr = arrayAt(node, "audioTracks")) {
        for (auto const& entry : *arr) {
            if (entry.is_object()) tracks.push_back(audioTrackFrom(entry));
        }
    }
    return tracks;
}

struct LegacyClip {
    Clip clip;
    int  activeCameraTrackIdx{0};
};

struct LegacyTrack {
    std::vector<LegacyClip> clips;
    bool                    visible{true};
    bool                    locked{false};
};

LegacyTrack legacyTrackFrom(json const& node) {
    LegacyTrack track;
    track.visible = boolOr(node, "visible", true);
    track.locked  = boolOr(node, "locked", false);
    if (auto const* clips = arrayAt(node, "clips")) {
        for (auto const& entry : *clips) {
            if (entry.is_object()) {
                track.clips.push_back(LegacyClip{clipFrom(entry), intOr(entry, "activeCameraTrackIdx", 0)});
            }
        }
    }
    return track;
}

json toJson(SpeedKeyframe const& key) {
    return {
        {"speed",             key.speed                              },
        {"interpolationType", static_cast<int>(key.interpolationType)},
        {"bezierCtrl1",       toJson(key.bezierCtrl1)                },
        {"bezierCtrl2",       toJson(key.bezierCtrl2)                },
    };
}

SpeedKeyframe speedKeyframeFrom(json const& node) {
    SpeedKeyframe key;
    key.speed             = clampSpeedKeyframeValue(finiteOr(node, "speed", key.speed));
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

json toJson(SpeedTrack const& track) {
    auto keys = json::array();
    for (auto const& [tick, key] : track.keysByTick) {
        auto entry    = toJson(key);
        entry["tick"] = tick;
        keys.push_back(std::move(entry));
    }
    return {
        {"present", track.isVisible()},
        {"enabled", track.enabled    },
        {"locked",  track.locked     },
        {"keys",    std::move(keys)  },
    };
}

SpeedTrack speedTrackFrom(json const& node) {
    SpeedTrack track;
    track.enabled = boolOr(node, "enabled", true);
    track.locked  = boolOr(node, "locked", false);
    if (auto const* keys = arrayAt(node, "keys")) {
        for (auto const& entry : *keys) {
            if (!entry.is_object()) continue;
            int const tick = std::max(0, intOr(entry, "tick", 0));
            track.keysByTick.insert_or_assign(tick, speedKeyframeFrom(entry));
        }
    }
    track.present = boolOr(node, "present", !track.keysByTick.empty());
    if (!track.keysByTick.empty()) track.present = true;
    return track;
}

json toJson(FreezeKeyframe const& key) {
    return {
        {"frozen",      key.frozen     },
        {"frozenDelay", key.frozenDelay},
    };
}

FreezeKeyframe freezeKeyframeFrom(json const& node) {
    FreezeKeyframe key;
    key.frozen      = boolOr(node, "frozen", key.frozen);
    key.frozenDelay = clampFreezeDelayTicks(intOr(node, "frozenDelay", key.frozenDelay));
    return key;
}

json toJson(FreezeTrack const& track) {
    auto keys = json::array();
    for (auto const& [tick, key] : track.keysByTick) {
        auto entry    = toJson(key);
        entry["tick"] = tick;
        keys.push_back(std::move(entry));
    }
    return {
        {"present", track.isVisible()},
        {"enabled", track.enabled    },
        {"locked",  track.locked     },
        {"keys",    std::move(keys)  },
    };
}

FreezeTrack freezeTrackFrom(json const& node) {
    FreezeTrack track;
    track.enabled = boolOr(node, "enabled", true);
    track.locked  = boolOr(node, "locked", false);
    if (auto const* keys = arrayAt(node, "keys")) {
        for (auto const& entry : *keys) {
            if (!entry.is_object()) continue;
            int const tick = std::max(0, intOr(entry, "tick", 0));
            track.keysByTick.insert_or_assign(tick, freezeKeyframeFrom(entry));
        }
    }
    track.present = boolOr(node, "present", !track.keysByTick.empty());
    if (!track.keysByTick.empty()) track.present = true;
    return track;
}

json toJson(TimelapseKeyframe const& key) {
    return {
        {"outputTicks", key.outputTicks},
    };
}

TimelapseKeyframe timelapseKeyframeFrom(json const& node) {
    TimelapseKeyframe key;
    key.outputTicks = clampTimelapseOutputTicks(intOr(node, "outputTicks", key.outputTicks));
    return key;
}

json toJson(TimelapseTrack const& track) {
    auto keys = json::array();
    for (auto const& [tick, key] : track.keysByTick) {
        auto entry    = toJson(key);
        entry["tick"] = tick;
        keys.push_back(std::move(entry));
    }
    return {
        {"present", track.isVisible()},
        {"enabled", track.enabled    },
        {"locked",  track.locked     },
        {"keys",    std::move(keys)  },
    };
}

TimelapseTrack timelapseTrackFrom(json const& node) {
    TimelapseTrack track;
    track.enabled = boolOr(node, "enabled", true);
    track.locked  = boolOr(node, "locked", false);
    if (auto const* keys = arrayAt(node, "keys")) {
        for (auto const& entry : *keys) {
            if (!entry.is_object()) continue;
            int const tick = std::max(0, intOr(entry, "tick", 0));
            track.keysByTick.insert_or_assign(tick, timelapseKeyframeFrom(entry));
        }
    }
    track.present = boolOr(node, "present", !track.keysByTick.empty());
    if (!track.keysByTick.empty()) track.present = true;
    return track;
}

json toJson(TimeOfDayKeyframe const& key) {
    return {
        {"timeOfDay",         key.timeOfDay                          },
        {"interpolationType", static_cast<int>(key.interpolationType)},
        {"bezierCtrl1",       toJson(key.bezierCtrl1)                },
        {"bezierCtrl2",       toJson(key.bezierCtrl2)                },
    };
}

TimeOfDayKeyframe timeOfDayKeyframeFrom(json const& node) {
    TimeOfDayKeyframe key;
    key.timeOfDay         = clampTimeOfDayTicks(intOr(node, "timeOfDay", key.timeOfDay));
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

json toJson(TimeOfDayTrack const& track) {
    auto keys = json::array();
    for (auto const& [tick, key] : track.keysByTick) {
        auto entry    = toJson(key);
        entry["tick"] = tick;
        keys.push_back(std::move(entry));
    }
    return {
        {"present", track.isVisible()},
        {"enabled", track.enabled    },
        {"locked",  track.locked     },
        {"keys",    std::move(keys)  },
    };
}

TimeOfDayTrack timeOfDayTrackFrom(json const& node) {
    TimeOfDayTrack track;
    track.enabled = boolOr(node, "enabled", true);
    track.locked  = boolOr(node, "locked", false);
    if (auto const* keys = arrayAt(node, "keys")) {
        for (auto const& entry : *keys) {
            if (!entry.is_object()) continue;
            int const tick = std::max(0, intOr(entry, "tick", 0));
            track.keysByTick.insert_or_assign(tick, timeOfDayKeyframeFrom(entry));
        }
    }
    track.present = boolOr(node, "present", !track.keysByTick.empty());
    if (!track.keysByTick.empty()) track.present = true;
    return track;
}

char const* weatherOverrideToString(WeatherOverride weather) noexcept {
    switch (weather) {
    case WeatherOverride::Clear:
        return "clear";
    case WeatherOverride::Overcast:
        return "overcast";
    case WeatherOverride::Rain:
        return "rain";
    case WeatherOverride::Snow:
        return "snow";
    case WeatherOverride::Thunder:
        return "thunder";
    case WeatherOverride::Recorded:
    default:
        return "recorded";
    }
}

WeatherOverride weatherOverrideFrom(json const& node, char const* key) {
    auto const value = node.find(key);
    if (value == node.end()) return WeatherOverride::Recorded;
    if (value->is_string()) {
        std::string const str = value->get<std::string>();
        if (str == "clear") return WeatherOverride::Clear;
        if (str == "overcast") return WeatherOverride::Overcast;
        if (str == "rain") return WeatherOverride::Rain;
        if (str == "snow") return WeatherOverride::Snow;
        if (str == "thunder") return WeatherOverride::Thunder;
        return WeatherOverride::Recorded;
    }
    if (value->is_number_integer()) {
        return enumOr(node, key, WeatherOverride::Recorded, static_cast<int>(WeatherOverride::Thunder));
    }
    return WeatherOverride::Recorded;
}

json toJson(WeatherKeyframe const& key) {
    return {
        {"weather", weatherOverrideToString(key.weather)},
    };
}

WeatherKeyframe weatherKeyframeFrom(json const& node) {
    WeatherKeyframe key;
    if (node.contains("weather")) {
        key.weather = weatherOverrideFrom(node, "weather");
    }
    return key;
}

json toJson(WeatherTrack const& track) {
    auto keys = json::array();
    for (auto const& [tick, key] : track.keysByTick) {
        auto entry    = toJson(key);
        entry["tick"] = tick;
        keys.push_back(std::move(entry));
    }
    return {
        {"present", track.isVisible()},
        {"enabled", track.enabled    },
        {"locked",  track.locked     },
        {"keys",    std::move(keys)  },
    };
}

WeatherTrack weatherTrackFrom(json const& node) {
    WeatherTrack track;
    track.enabled = boolOr(node, "enabled", true);
    track.locked  = boolOr(node, "locked", false);
    if (auto const* keys = arrayAt(node, "keys")) {
        for (auto const& entry : *keys) {
            if (!entry.is_object()) continue;
            int const tick = std::max(0, intOr(entry, "tick", 0));
            track.keysByTick.insert_or_assign(tick, weatherKeyframeFrom(entry));
        }
    }
    track.present = boolOr(node, "present", !track.keysByTick.empty());
    if (!track.keysByTick.empty()) track.present = true;
    return track;
}

json toJson(Scene const& scene) {
    auto cameras = json::array();
    for (auto const& camera : scene.cameras) cameras.push_back(toJson(camera));

    auto markers = json::array();
    for (auto const& marker : scene.markers) markers.push_back(toJson(marker));

    json result{
        {"id",            scene.id           },
        {"name",          scene.name         },
        {"exportInTick",  scene.exportInTick },
        {"exportOutTick", scene.exportOutTick},
        {"cameras",       std::move(cameras) },
        {"markers",       std::move(markers) },
    };
    if (scene.speedTrack.isVisible()) {
        result["speedTrack"] = toJson(scene.speedTrack);
    }
    if (scene.freezeTrack.isVisible()) {
        result["freezeTrack"] = toJson(scene.freezeTrack);
    }
    if (scene.timelapseTrack.isVisible()) {
        result["timelapseTrack"] = toJson(scene.timelapseTrack);
    }
    if (scene.timeOfDayTrack.isVisible()) {
        result["timeOfDayTrack"] = toJson(scene.timeOfDayTrack);
    }
    if (scene.weatherTrack.isVisible()) {
        result["weatherTrack"] = toJson(scene.weatherTrack);
    }
    result["daytimeOverrideEnabled"] = scene.daytimeOverrideEnabled;
    result["daytimeOverrideTicks"]   = clampTimeOfDayTicks(scene.daytimeOverrideTicks);
    result["weatherOverride"]        = weatherOverrideToString(scene.weatherOverride);
    if (!scene.audioTracks.empty()) {
        auto audioTracks = json::array();
        for (auto const& track : scene.audioTracks) audioTracks.push_back(toJson(track));
        result["audioTracks"] = std::move(audioTracks);
    }
    return result;
}

Scene sceneFrom(json const& node) {
    Scene scene;
    scene.id            = stringOr(node, "id");
    scene.name          = stringOr(node, "name");
    scene.exportInTick  = intOr(node, "exportInTick", -1);
    scene.exportOutTick = intOr(node, "exportOutTick", -1);
    if (scene.exportInTick < 0) scene.exportInTick = -1;
    if (scene.exportOutTick < 0) scene.exportOutTick = -1;
    if (scene.exportInTick >= 0 && scene.exportOutTick >= 0 && scene.exportInTick >= scene.exportOutTick) {
        scene.exportInTick  = -1;
        scene.exportOutTick = -1;
    }

    bool anyCameraHadClips = false;
    if (auto const* cameras = arrayAt(node, "cameras")) {
        for (auto const& entry : *cameras) {
            if (!entry.is_object()) continue;
            if (arrayAt(entry, "clips") != nullptr) anyCameraHadClips = true;
            scene.cameras.push_back(cameraFrom(entry));
        }
    }
    // Migrate earlier v2 scene-level clips onto the primary camera track.
    if (auto const* legacyClips = arrayAt(node, "clips"); legacyClips && !anyCameraHadClips) {
        if (scene.cameras.empty()) {
            CameraEntity camera;
            camera.id   = "camera_1";
            camera.name = "Camera 1";
            scene.cameras.push_back(std::move(camera));
        }
        auto& primary = scene.cameras.front();
        for (auto const& entry : *legacyClips) {
            if (entry.is_object()) primary.clips.push_back(clipFrom(entry));
        }
        std::ranges::stable_sort(primary.clips, {}, &Clip::inTick);
        if (boolOr(node, "clipTrackLocked", false)) primary.locked = true;
    }
    if (auto const* markers = arrayAt(node, "markers")) {
        for (auto const& entry : *markers) {
            if (!entry.is_object()) continue;
            scene.markers.push_back({stringOr(entry, "id"), stringOr(entry, "label"), intOr(entry, "tick", 0)});
        }
    }
    if (auto const* speedTrack = objectAt(node, "speedTrack")) {
        scene.speedTrack = speedTrackFrom(*speedTrack);
    }
    if (auto const* freezeTrack = objectAt(node, "freezeTrack")) {
        scene.freezeTrack = freezeTrackFrom(*freezeTrack);
    }
    if (auto const* timelapseTrack = objectAt(node, "timelapseTrack")) {
        scene.timelapseTrack = timelapseTrackFrom(*timelapseTrack);
    }
    if (auto const* timeOfDayTrack = objectAt(node, "timeOfDayTrack")) {
        scene.timeOfDayTrack = timeOfDayTrackFrom(*timeOfDayTrack);
    }
    if (auto const* weatherTrack = objectAt(node, "weatherTrack")) {
        scene.weatherTrack = weatherTrackFrom(*weatherTrack);
    }
    scene.daytimeOverrideEnabled = boolOr(node, "daytimeOverrideEnabled", false);
    scene.daytimeOverrideTicks   = clampTimeOfDayTicks(intOr(node, "daytimeOverrideTicks", 6000));
    scene.weatherOverride        = weatherOverrideFrom(node, "weatherOverride");
    scene.audioTracks = audioTracksFrom(node);
    return scene;
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
    auto        scenesJson    = json::array();
    std::string activeSceneId = project.activeSceneId;

    if (!project.scenes.empty()) {
        if (std::ranges::none_of(project.scenes, [&](Scene const& scene) { return scene.id == activeSceneId; })) {
            activeSceneId = project.scenes.front().id;
        }
        for (auto const& scene : project.scenes) {
            if (scene.id == activeSceneId) {
                Scene synced;
                synced.id            = scene.id;
                synced.name          = scene.name;
                synced.cameras       = project.cameras;
                synced.markers       = project.markers;
                synced.speedTrack     = project.speedTrack;
                synced.freezeTrack    = project.freezeTrack;
                synced.timelapseTrack = project.timelapseTrack;
                synced.timeOfDayTrack         = project.timeOfDayTrack;
                synced.weatherTrack           = project.weatherTrack;
                synced.daytimeOverrideEnabled = project.daytimeOverrideEnabled;
                synced.daytimeOverrideTicks   = project.daytimeOverrideTicks;
                synced.weatherOverride        = project.weatherOverride;
                synced.audioTracks    = project.audioTracks;
                synced.exportInTick   = project.exportInTick;
                synced.exportOutTick  = project.exportOutTick;
                scenesJson.push_back(toJson(synced));
            } else {
                scenesJson.push_back(toJson(scene));
            }
        }
    } else {
        activeSceneId = "scene_1";
        Scene fallback;
        fallback.id             = activeSceneId;
        fallback.name           = "Scene 1";
        fallback.cameras        = project.cameras;
        fallback.markers        = project.markers;
        fallback.speedTrack     = project.speedTrack;
        fallback.freezeTrack    = project.freezeTrack;
        fallback.timelapseTrack = project.timelapseTrack;
        fallback.timeOfDayTrack         = project.timeOfDayTrack;
        fallback.weatherTrack           = project.weatherTrack;
        fallback.daytimeOverrideEnabled = project.daytimeOverrideEnabled;
        fallback.daytimeOverrideTicks   = project.daytimeOverrideTicks;
        fallback.weatherOverride        = project.weatherOverride;
        fallback.audioTracks    = project.audioTracks;
        fallback.exportInTick   = project.exportInTick;
        fallback.exportOutTick = project.exportOutTick;
        scenesJson.push_back(toJson(fallback));
    }

    auto transitions = json::array();
    for (auto const& transition : project.transitions) transitions.push_back(toJson(transition));

    json const document{
        {"formatVersion", kProjectFormatVersion },
        {"projectName",   project.projectName   },
        {"replayPath",    project.projectPath   },
        {"totalTicks",    project.totalTicks    },
        {"currentTick",   project.currentTick   },
        {"fps",           project.fps           },
        {"activeSceneId", activeSceneId         },
        {"scenes",        std::move(scenesJson) },
        {"transitions",   std::move(transitions)}
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
    loaded.projectName = stringOr(document, "projectName");
    loaded.projectPath = stringOr(document, "replayPath");
    loaded.totalTicks  = intOr(document, "totalTicks", 0);
    loaded.currentTick = intOr(document, "currentTick", 0);
    loaded.fps         = finiteOr(document, "fps", 60.0f);

    if (auto const* scenesArray = arrayAt(document, "scenes"); formatVersion >= 2 && scenesArray) {
        for (auto const& entry : *scenesArray) {
            if (entry.is_object()) loaded.scenes.push_back(sceneFrom(entry));
        }
        loaded.activeSceneId = stringOr(document, "activeSceneId");
    } else {
        // Legacy v1 migration: merge videoTracks into a single default scene.
        Scene migrated;
        migrated.id   = "scene_1";
        migrated.name = "Scene 1";

        if (auto const* cameras = arrayAt(document, "cameras")) {
            for (auto const& entry : *cameras) {
                if (entry.is_object()) migrated.cameras.push_back(cameraFrom(entry));
            }
        }
        if (migrated.cameras.empty()) {
            CameraEntity camera;
            camera.id   = "camera_1";
            camera.name = "Camera 1";
            migrated.cameras.push_back(std::move(camera));
        }
        if (auto const* markers = arrayAt(document, "markers")) {
            for (auto const& entry : *markers) {
                if (!entry.is_object()) continue;
                migrated.markers.push_back({stringOr(entry, "id"), stringOr(entry, "label"), intOr(entry, "tick", 0)});
            }
        }

        std::vector<LegacyTrack> legacyTracks;
        if (auto const* tracks = arrayAt(document, "videoTracks")) {
            for (auto const& entry : *tracks) {
                if (entry.is_object()) legacyTracks.push_back(legacyTrackFrom(entry));
            }
        }
        int const activeTrackIdx = intOr(document, "activeVideoTrackIdx", 0);

        std::vector<size_t> orderedIndices;
        orderedIndices.reserve(legacyTracks.size());
        if (activeTrackIdx >= 0 && static_cast<size_t>(activeTrackIdx) < legacyTracks.size()) {
            orderedIndices.push_back(static_cast<size_t>(activeTrackIdx));
        }
        for (size_t i = 0; i < legacyTracks.size(); ++i) {
            if (static_cast<int>(i) != activeTrackIdx && legacyTracks[i].visible) orderedIndices.push_back(i);
        }
        for (size_t i = 0; i < legacyTracks.size(); ++i) {
            if (static_cast<int>(i) != activeTrackIdx && !legacyTracks[i].visible) orderedIndices.push_back(i);
        }

        for (size_t idx : orderedIndices) {
            auto const& track = legacyTracks[idx];
            for (auto const& legacyEntry : track.clips) {
                Clip clip = legacyEntry.clip;
                if (!track.visible) clip.exportEnabled = false;
                size_t const targetIdx =
                    (legacyEntry.activeCameraTrackIdx >= 0
                     && static_cast<size_t>(legacyEntry.activeCameraTrackIdx) < migrated.cameras.size())
                        ? static_cast<size_t>(legacyEntry.activeCameraTrackIdx)
                        : 0;
                auto&      targetClips = migrated.cameras[targetIdx].clips;
                bool const overlaps    = std::ranges::any_of(targetClips, [&](Clip const& existing) {
                    return existing.inTick < clip.outTick && clip.inTick < existing.outTick;
                });
                if (!overlaps) targetClips.push_back(std::move(clip));
            }
        }
        for (auto& camera : migrated.cameras) {
            std::ranges::stable_sort(camera.clips, {}, &Clip::inTick);
        }

        if (auto const* timeOfDayTrack = objectAt(document, "timeOfDayTrack")) {
            migrated.timeOfDayTrack = timeOfDayTrackFrom(*timeOfDayTrack);
        }
        if (auto const* weatherTrack = objectAt(document, "weatherTrack")) {
            migrated.weatherTrack = weatherTrackFrom(*weatherTrack);
        }
        migrated.daytimeOverrideEnabled = boolOr(document, "daytimeOverrideEnabled", false);
        migrated.daytimeOverrideTicks   = clampTimeOfDayTicks(intOr(document, "daytimeOverrideTicks", 6000));
        migrated.weatherOverride        = weatherOverrideFrom(document, "weatherOverride");

        loaded.scenes.push_back(std::move(migrated));
        loaded.activeSceneId = "scene_1";
    }

    // Ensure unique non-empty clip IDs across all loaded scenes and cameras.
    {
        std::unordered_set<std::string> reservedIds;
        for (auto const& scene : loaded.scenes) {
            for (auto const& camera : scene.cameras) {
                for (auto const& clip : camera.clips) {
                    if (!clip.id.empty()) reservedIds.insert(clip.id);
                }
            }
        }
        std::unordered_set<std::string> seenIds;
        size_t                          nextId = 1;
        for (auto& scene : loaded.scenes) {
            for (auto& camera : scene.cameras) {
                for (auto& clip : camera.clips) {
                    if (!clip.id.empty() && seenIds.insert(clip.id).second) continue;
                    for (;;) {
                        auto candidate = "clip_" + std::to_string(nextId++);
                        if (!reservedIds.contains(candidate) && seenIds.insert(candidate).second) {
                            clip.id = std::move(candidate);
                            break;
                        }
                    }
                }
            }
        }
    }

    if (!loaded.scenes.empty()) {
        auto it = std::ranges::find(loaded.scenes, loaded.activeSceneId, &Scene::id);
        if (it == loaded.scenes.end()) it = loaded.scenes.begin();
        loaded.activeSceneId = it->id;
        loaded.cameras       = it->cameras;
        loaded.markers       = it->markers;
        loaded.speedTrack     = it->speedTrack;
        loaded.freezeTrack    = it->freezeTrack;
        loaded.timelapseTrack = it->timelapseTrack;
        loaded.timeOfDayTrack         = it->timeOfDayTrack;
        loaded.weatherTrack           = it->weatherTrack;
        loaded.daytimeOverrideEnabled = it->daytimeOverrideEnabled;
        loaded.daytimeOverrideTicks   = it->daytimeOverrideTicks;
        loaded.weatherOverride        = it->weatherOverride;
        loaded.audioTracks    = it->audioTracks;
        loaded.exportInTick   = it->exportInTick;
        loaded.exportOutTick = it->exportOutTick;
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
