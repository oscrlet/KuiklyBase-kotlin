/*
 * Copyright 2010-2023 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the LICENSE file.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <fstream>
#include "RunLoopFinalizerProcessor.hpp"

namespace kotlin::gcScheduler {

// NOTE: When changing default values, reflect them in GC.kt as well.
struct GCSchedulerConfig {
    enum class MutatorAssists {
        kDefault,
        kEnable,
        kDisable,
    };

    std::atomic<bool> autoTune = true;
    // The target interval between collections when Kotlin code is idle. GC will be triggered
    // by timer no sooner than this value and no later than twice this value since the previous collection.
    std::atomic<int64_t> regularGcIntervalMicroseconds = 10 * 1000 * 1000;
    // GC will try to keep object bytes under this amount. If object bytes have
    // become bigger than this value, and `mutatorAssists` are enabled the GC will
    // stop the world and wait until current epoch finishes.
    // Adapts after each GC epoch when `autoTune = true`.
    std::atomic<int64_t> targetHeapBytes = 10 * 1024 * 1024;
    // The rate at which `targetHeapBytes` changes when `autoTune = true`. Concretely: if after the collection
    // `N` object bytes remain in the heap, the next `targetHeapBytes` will be `N / targetHeapUtilization` capped
    // between `minHeapBytes` and `maxHeapBytes`.
    std::atomic<double> targetHeapUtilization = 0.55;
    // The minimum value of `targetHeapBytes` for `autoTune = true`
    std::atomic<int64_t> minHeapBytes = 5 * 1024 * 1024; // In `custom` allocator pages are 256KiB. 5MiB here is 20 pages.
    // The maximum value of `targetHeapBytes` for `autoTune = true`
    std::atomic<int64_t> maxHeapBytes = std::numeric_limits<int64_t>::max();
    // GC will be triggered when object bytes reach `heapTriggerCoefficient * targetHeapBytes`.
    std::atomic<double> heapTriggerCoefficient = 0.9;
    // See `mutatorAssists()`.
    std::atomic<std::underlying_type_t<MutatorAssists>> mutatorAssistsImpl =
            static_cast<std::underlying_type_t<MutatorAssists>>(MutatorAssists::kDefault);
    // region Tencent Code
    // 控制 GC 挂起开关
    std::atomic<bool> enableGCSuspend = true;
    // endregion

    std::chrono::microseconds regularGcInterval() const { return std::chrono::microseconds(regularGcIntervalMicroseconds.load()); }

    // region Tencent Code
    std::atomic<bool> delayFreePageAfterSTW = false;
    // end region

    // Whether mutators should stop and wait for GC to complete when
    // current object heap size is bigger than `targetHeapBytes`.
    // By default on, unless `autoTune = false` or `maxHeapBytes` is set.
    bool mutatorAssists() const noexcept {
        switch (static_cast<MutatorAssists>(mutatorAssistsImpl.load())) {
            case MutatorAssists::kDisable:
                return false;
            case MutatorAssists::kEnable:
                return true;
            case MutatorAssists::kDefault:
                // If after a GC epoch the alive set is more than maximum `targetHeapBytes`, the next GC will be
                // scheduled instantly and when the assists are turned on, the mutators would be immediately paused.
                // This will look like the program has hanged.
                // So, by default, disable assisting if `targetHeapBytes` has a non-infinite limit
                // (either `autoTune == false`, so `targetHeapBytes` is fixed; or `maxHeapBytes`
                // is lower than infinity).
                // TODO: Figure out what to do with OOMs.
                return autoTune.load() && maxHeapBytes.load() == std::numeric_limits<int64_t>::max();
        }
    }

    // See `mutatorAssists()`.
    void setMutatorAssists(bool assist) noexcept {
        mutatorAssistsImpl.store(
                static_cast<std::underlying_type_t<MutatorAssists>>(assist ? MutatorAssists::kEnable : MutatorAssists::kDisable));
    }

    bool parseBool(const std::string& val) const {
        return val == "true" || val == "True" || val == "1";
    }

    MutatorAssists parseMutatorAssists(const std::string& val) const {
        if (val == "enable") return MutatorAssists::kEnable;
        if (val == "disable") return MutatorAssists::kDisable;
        return MutatorAssists::kDefault;
    }

    void loadFromFile(const std::string& filename) {
        static bool loaded = false;
        if (loaded) return;
        std::ifstream fin(filename);
        if (!fin.is_open()) return;

        std::string line;
        while (std::getline(fin, line)) {
            // 简单清理处理
            line.erase(std::remove_if(line.begin(), line.end(), ::isspace), line.end());
            if (line.empty() || line[0] == '#') continue;

            auto delim_pos = line.find('=');
            if (delim_pos == std::string::npos) continue;
            std::string key = line.substr(0, delim_pos);
            std::string value = line.substr(delim_pos + 1);

            if (key == "autoTune") autoTune.store(parseBool(value));
            else if (key == "regularGcIntervalMicroseconds") regularGcIntervalMicroseconds.store(std::stoll(value));
            else if (key == "targetHeapBytes") targetHeapBytes.store(std::stoll(value));
            else if (key == "targetHeapUtilization") targetHeapUtilization.store(std::stod(value));
            else if (key == "minHeapBytes") minHeapBytes.store(std::stoll(value));
            else if (key == "maxHeapBytes") maxHeapBytes.store(std::stoll(value));
            else if (key == "heapTriggerCoefficient") heapTriggerCoefficient.store(std::stod(value));
            else if (key == "mutatorAssists")
                mutatorAssistsImpl.store(static_cast<std::underlying_type_t<MutatorAssists>>(parseMutatorAssists(value)));
        }
        RuntimeLogInfo({kTagGC}, "GC Scheduler config loaded from %s", filename.c_str());
        RuntimeLogInfo({kTagGC}, "targetHeapUtilization: %lf", targetHeapUtilization.load());
        loaded = true;
    }
};

} // namespace kotlin::gcScheduler
