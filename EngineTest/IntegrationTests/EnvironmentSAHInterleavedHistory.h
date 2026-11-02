#pragma once
#include <cstdint>

// A stable run of observed, newly completed host frame slots, not a GPU
// timestamp-to-exact-frame mapping. Completion serials are consumed per slot.
struct EnvironmentSAHInterleavedHistory {
    uint64_t consumed[3]{};
    uint64_t lastRecording = 0, lastObservation = 0;
    int lastMode = -1;
    unsigned consecutive = 0;

    template<class Completions>
    bool Observe(const Completions& completions, uint32_t slot, uint64_t recording,
                 uint64_t observation, int mode) {
        if (slot >= 3 || !recording || !completions.CanReuse(slot, consumed[slot])) {
            consecutive = 0; lastRecording = 0; lastMode = -1;
            return false;
        }
        const uint64_t serial = completions.Serial(slot);
        const bool serialContinuous = serial == consumed[slot] + 1;
        consumed[slot] = serial;
        if (!serialContinuous) {
            consecutive = 0; lastRecording = 0; lastMode = -1;
            return false;
        }
        const bool continuous = consecutive && recording == lastRecording + 1 &&
                                observation == lastObservation + 1 && mode == lastMode;
        consecutive = continuous ? consecutive + 1 : 1;
        lastRecording = recording; lastObservation = observation; lastMode = mode;
        return consecutive >= 8;
    }
};
