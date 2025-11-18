/*
 * Copyright 2022 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the LICENSE file.
 */

#ifndef CUSTOM_ALLOC_CPP_HEAP_HPP_
#define CUSTOM_ALLOC_CPP_HEAP_HPP_

#include <atomic>
#include <fstream>
#include <mutex>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <ostream>
#include <string>
#include <thread>
#include <vector>
#include <iostream>
#include <sstream>


#include "AtomicStack.hpp"
#include "ExtraObjectPage.hpp"
#include "ExtraObjectData.hpp"
#include "GCStatistics.hpp"
#include "Memory.h"
#include "SingleObjectPage.hpp"
#include "NextFitPage.hpp"
#include "PageStore.hpp"
#include "FixedBlockPage.hpp"
#include "GCApi.hpp"

namespace kotlin::alloc {

static ALWAYS_INLINE void AddHeapDumpWithTimestamp(std::ostream& out, std::chrono::steady_clock::time_point currentTime){
    out << std::chrono::duration_cast<std::chrono::microseconds>(currentTime.time_since_epoch()).count() << ", "
        << allocatedBytes() << std::endl;
}

#ifdef KONAN_OHOS
static constexpr const char* fileDumpDir = "/data/storage/el2/base/files/";
#else
static constexpr const char* fileDumpDir = "/Users/bytedance/log/";
#endif

class HeapUsageTracer {
public:
    // Singleton
    static HeapUsageTracer& Instance(const std::stringstream &pageDumpRef) {
        static HeapUsageTracer instance(pageDumpRef);
        return instance;
    }

    HeapUsageTracer(const std::stringstream &pageDumpRef) : pageDump(pageDumpRef) {
        start();
    }

    void start() {
        stopRequested_ = false;
        heapDump.open(std::string(fileDumpDir) + "memdump.log", std::ios::out);
        heapDump << "phase1: heap use" << std::endl;
        // Only start if not already running
        if (tracingThread_.joinable()) return;
        tracingThread_ = std::thread([this] { run(); });
    }

    void stop() {
        stopRequested_ = true;
        cv_.notify_all();
        if (tracingThread_.joinable())
            tracingThread_.join();
        dumpAll_ = true;
        dump();
    }

    bool isFileContentOne(const std::string& filename) {
        std::ifstream file(filename);
        if (!file) return false;

        std::string content;
        file >> content;
        auto result = content == "1";
        return result;
    }

    bool dumpAll() {
        if (dumpAll_) {
            return true;
        }
        if (!isFileContentOne(std::string(fileDumpDir) + "control.log")) {
            return false;
        }
        dumpAll_ = true;
        return dumpAll_;
    }

    void dump() {
        if (dumpFinish) return;
        static size_t current = 0;
        // size_t max = 0;
        // for (const auto& e : events_) if (e.bytes > max) max = e.bytes;
        auto end = events_.size();
        for (auto i = current; i < end; ++i) {
            heapDump << events_[i].timeStamp << ","
                << events_[i].bytes << std::endl;
        }
        current = end;
        if (dumpAll()) {
            dumpFinish = true;
            heapDump << pageDump.str();
        }
    }

    ~HeapUsageTracer() { stop(); }

private:
    struct Event {
        size_t bytes;
        std::chrono::steady_clock::rep timeStamp;
    };

    void run() {
        auto next = std::chrono::steady_clock::now();
        while (!stopRequested_) {
            next += std::chrono::microseconds(100);
            size_t bytes = allocatedBytes();
            {
                events_.push_back({bytes, std::chrono::duration_cast<std::chrono::microseconds>(next.time_since_epoch()).count()});
            }
            next += std::chrono::microseconds(100);
            std::unique_lock<std::mutex> lock(cvMutex_);
            cv_.wait_until(lock, next, [this]() { return stopRequested_.load(); });
        }
    }

    std::vector<Event> events_;
    // std::mutex mutex_;
    std::atomic<bool> stopRequested_{false};
    std::thread tracingThread_;
    std::condition_variable cv_;
    std::mutex cvMutex_;
    std::fstream heapDump;
    const std::stringstream &pageDump;
    bool dumpAll_ = false;
    bool dumpFinish = false;
};

class Heap {
public:
    // Called once by the GC thread after all mutators have been suspended
    void PrepareForGC() noexcept;

    // Sweep through all remaining pages, freeing those blocks where CanReclaim
    // returns true. If multiple sweepers are active, each page will only be
    // seen by one sweeper.
    FinalizerQueue Sweep(gc::GCHandle gcHandle) noexcept;

    FixedBlockPage* GetFixedBlockPage(uint32_t cellCount, FinalizerQueue& finalizerQueue) noexcept;
    NextFitPage* GetNextFitPage(uint32_t cellCount, FinalizerQueue& finalizerQueue) noexcept;
    SingleObjectPage* GetSingleObjectPage(uint64_t cellCount, FinalizerQueue& finalizerQueue) noexcept;
    ExtraObjectPage* GetExtraObjectPage(FinalizerQueue& finalizerQueue) noexcept;

    void AddToFinalizerQueue(FinalizerQueue queue) noexcept;
    FinalizerQueue ExtractFinalizerQueue() noexcept;

    // Test method
    std::vector<ObjHeader*> GetAllocatedObjects() noexcept;
    void ClearForTests() noexcept;

    auto& allocatedSizeTracker() noexcept { return allocatedSizeTracker_; }

    // region Tencent Code
    void TraverseFixedPagesPart(int index, const std::function<void(void *, int, size_t)>& process) {
        static constexpr int length = FixedBlockPage::MAX_BLOCK_SIZE + 1;
        // 预计算分界点 [0, 20%, 45%, 100%] 经验值，每组耗时接近。并发数量增加时调整这个 array
        static constexpr std::array thresholds{0, length / 5, length * 9 / 20, length};
        const int startIndex = thresholds[index];
        const int endIndex = thresholds[index + 1];
        for (int blockSize = startIndex; blockSize < endIndex; ++blockSize) {
            fixedBlockPages_[blockSize].TraversePages([process](FixedBlockPage *page) {
                process(page, 1, FixedBlockPage::SIZE);
            });
        }
    }

    void TraverseNextFitAndSinglePages(const std::function<void(void *, int, size_t)>&  process) {
        nextFitPages_.TraversePages([process](auto *page) {
            process(page, 2, NextFitPage::SIZE);
        });
        singleObjectPages_.TraversePages([process](SingleObjectPage *page) {
            process(page, 3, page->getSize());
        });
    }

    static ObjHeader *GetObjHeaderAndOriginObjPtr(uint8_t *block, uintptr_t &originPtr) {
        auto* customHeapObject = reinterpret_cast<CustomHeapObject *>(block);
        auto* objHeader = customHeapObject->object();
        uintptr_t originObjPtr = 0;
        if (objHeader->type_info()->IsArray()) {
            originObjPtr = reinterpret_cast<CustomHeapArray*>(block)->GetArrayHeaderAddress(originPtr);
        } else {
            originObjPtr = customHeapObject->GetObjHeaderAddress(originPtr);
        }
        originPtr = originObjPtr;
        return objHeader;
    }

    void TraverseFixedBlockPageObjects(void* page, const std::function<void(ObjHeader* obj, uintptr_t)>& process) {
        auto *fixedBlockPage = static_cast<FixedBlockPage *>(page);
        fixedBlockPage->TraverseAllocatedBlocksForFile([process, fixedBlockPage](auto *block, uint32_t next) {
            uintptr_t originPtr = fixedBlockPage->getOriginAddress(next);
            auto* objHeader = Heap::GetObjHeaderAndOriginObjPtr(block, originPtr);
            process(objHeader, originPtr);
        });
    }

    void TraverseNextFitPageObjects(void* page, const std::function<void(ObjHeader* obj, uintptr_t)>& process) {
        auto *nextFitPage = static_cast<NextFitPage *>(page);
        nextFitPage->TraverseAllocatedBlocksForFile([process, nextFitPage](auto *block, uint32_t offset) {
            uintptr_t originPtr = nextFitPage->getOriginAddress(offset);
            auto* objHeader = Heap::GetObjHeaderAndOriginObjPtr(block, originPtr);
            process(objHeader, originPtr);
        });
    }

    void TraverseSingleObjPageObjects(void* page, const std::function<void(ObjHeader* obj, uintptr_t)>& process) {
        auto *singleObjPage = static_cast<SingleObjectPage *>(page);
        singleObjPage->TraverseAllocatedBlocksForFile([process](auto *block, uintptr_t originPtr) {
            auto* objHeader = Heap::GetObjHeaderAndOriginObjPtr(block, originPtr);
            process(objHeader, originPtr);
        });
    }
    // endregion

    template <typename T>
    void TraverseAllocatedObjects(T process) noexcept(noexcept(process(std::declval<ObjHeader*>()))) {
        for (int blockSize = 0; blockSize <= FixedBlockPage::MAX_BLOCK_SIZE; ++blockSize) {
            fixedBlockPages_[blockSize].TraversePages([process](auto *page) {
                page->TraverseAllocatedBlocks([process](auto *block) {
                    process(reinterpret_cast<CustomHeapObject*>(block)->object());
                });
            });
        }
        nextFitPages_.TraversePages([process](auto *page) {
            page->TraverseAllocatedBlocks([process](auto *block) {
                process(reinterpret_cast<CustomHeapObject*>(block)->object());
            });
        });
        singleObjectPages_.TraversePages([process](auto *page) {
            page->TraverseAllocatedBlocks([process](auto *block) {
                process(reinterpret_cast<CustomHeapObject*>(block)->object());
            });
        });
    }

    template <typename T>
    void TraverseAllocatedExtraObjects(T process) noexcept(noexcept(process(std::declval<kotlin::mm::ExtraObjectData*>()))) {
        extraObjectPages_.TraversePages([process](auto *page) {
            page->TraverseAllocatedObjects(process);
        });
    }

    // region Tencent Code
    void TraverseAllPage() noexcept;
    void onFinishGC() noexcept;
    template <typename PageStoreType, typename PageType>
    void processPages(PageStoreType& pageStore, const char* typeName,
                      const std::function<void(PageType*)>& extraHandler = {});
    // endregion

    void Dump(std::string prefix = "") {
        if (dumpEnabled == false) return;
        if (pageDumpCount == 1) {
            pageDumpStore << "phase2: page dump" << std::endl;
        }
        pageDumpStore << "----------------------------" << prefix << " " << (pageDumpCount - 1)/2 << " ----------------------------" << std::endl;
        pageDumpStore << "Heap Dump at: ";
        AddHeapDumpWithTimestamp(pageDumpStore, std::chrono::steady_clock::now());

        auto pageDump = [this] (auto *page) {
            if (page) {
                page->Dump(pageDumpStore);
            }
        };
        for (int blockSize = 0; blockSize <= FixedBlockPage::MAX_BLOCK_SIZE; ++blockSize) {
            if (fixedBlockPages_[blockSize].GetPages().empty()) {
                continue;
            }
            pageDumpStore << blockSize << ": ";
            fixedBlockPages_[blockSize].TraversePages(pageDump);
            pageDumpStore << std::endl;
        }
        pageDumpStore << "nextFitPages" << ": ";
        nextFitPages_.TraversePages(pageDump);
        pageDumpStore << std::endl;
        pageDumpStore << "singleObjectPages" << ": ";
        singleObjectPages_.TraversePages(pageDump);
        pageDumpStore << std::endl;
        pageDumpStore << "extraObjectPages" << ": ";
        extraObjectPages_.TraversePages(pageDump);
        pageDumpStore << std::endl;
        if (pageDumpCount % 10 == 0) {
            heapUsageTracer_.dump();
        }
        ++pageDumpCount;
    }

private:
    PageStore<FixedBlockPage> fixedBlockPages_[FixedBlockPage::MAX_BLOCK_SIZE + 1];
    PageStore<NextFitPage> nextFitPages_;
    PageStore<SingleObjectPage> singleObjectPages_;
    PageStore<ExtraObjectPage> extraObjectPages_;

    FinalizerQueue pendingFinalizerQueue_;
    std::mutex pendingFinalizerQueueMutex_;

    std::atomic<std::size_t> concurrentSweepersCount_ = 0;

    AllocatedSizeTracker::Heap allocatedSizeTracker_{};
    std::stringstream pageDumpStore;  // 成员流，用于暂存字符串
    HeapUsageTracer& heapUsageTracer_ = HeapUsageTracer::Instance(pageDumpStore);
    bool dumpEnabled = true;
    int pageDumpCount = 1;                   // 计数，记录输入次数
};

} // namespace kotlin::alloc

#endif
