#pragma once
//
// PLAN_SEMA_ROOT_FIX — S5 (Scheduler-backed Mono telemetry).
//
// S4 introduced MonoScheduler as a read-only sidecar for Sema's
// `pendingInstantiations_` pushes. S5 extends the same ledger across
// Mono's real worklist: every accepted `Monomorphize::enqueue` request
// records its work-item kind, and every successfully emitted concrete
// declaration records a ready item.
//
// The scheduler still does not own production semantics. Sema keeps its
// historical `pendingInstantiations_` vector, Mono keeps its worklist, and
// CodeGen still consumes the translation unit. The value here is a single
// request/ready ledger we can compare against CodeGen failures while the
// authority is moved one step at a time.
//
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>

namespace vyx::canon {

enum class WorkItemKind : std::uint8_t {
    InstantiateClass,
    InstantiateFreeFn,
    InstantiateMethod,
};

class MonoScheduler {
public:
    void recordClassRequest(const std::string& mangledName) {
        record(mangledName, WorkItemKind::InstantiateClass, RequestOrigin::Sema);
    }
    void recordFreeFnRequest(const std::string& mangledName) {
        record(mangledName, WorkItemKind::InstantiateFreeFn, RequestOrigin::Sema);
    }
    void recordMethodRequest(const std::string& mangledClassName,
                             const std::string& methodName) {
        record(mangledClassName + "::" + methodName,
               WorkItemKind::InstantiateMethod,
               RequestOrigin::Sema);
    }

    void recordMonoClassRequest(const std::string& mangledName) {
        record(mangledName, WorkItemKind::InstantiateClass, RequestOrigin::Mono);
    }
    void recordMonoFreeFnRequest(const std::string& mangledName) {
        record(mangledName, WorkItemKind::InstantiateFreeFn, RequestOrigin::Mono);
    }
    void recordMonoMethodRequest(const std::string& mangledClassName,
                                 const std::string& methodName) {
        record(mangledClassName + "::" + methodName,
               WorkItemKind::InstantiateMethod,
               RequestOrigin::Mono);
    }

    void markReady(const std::string& key, WorkItemKind kind) {
        if (key.empty()) return;
        if (readyKeys_.insert(compositeKey(key, kind)).second) {
            switch (kind) {
                case WorkItemKind::InstantiateClass:  ++readyClassItems_;  break;
                case WorkItemKind::InstantiateFreeFn: ++readyFreeFnItems_; break;
                case WorkItemKind::InstantiateMethod: ++readyMethodItems_; break;
            }
        }
    }

    bool isFrozen() const { return frozen_; }
    void freeze() { frozen_ = true; }
    void clear() {
        frozen_ = false;
        totalRequests_ = 0;
        semaRequests_ = 0;
        monoRequests_ = 0;
        classRequests_ = 0;
        freeFnRequests_ = 0;
        methodRequests_ = 0;
        readyClassItems_ = 0;
        readyFreeFnItems_ = 0;
        readyMethodItems_ = 0;
        uniqueKeys_.clear();
        readyKeys_.clear();
    }

    std::size_t totalRequests() const { return totalRequests_; }
    std::size_t semaRequests() const { return semaRequests_; }
    std::size_t monoRequests() const { return monoRequests_; }
    std::size_t uniqueRequests() const { return uniqueKeys_.size(); }
    std::size_t classRequests() const { return classRequests_; }
    std::size_t freeFnRequests() const { return freeFnRequests_; }
    std::size_t methodRequests() const { return methodRequests_; }
    std::size_t readyItems() const { return readyKeys_.size(); }
    std::size_t readyClassItems() const { return readyClassItems_; }
    std::size_t readyFreeFnItems() const { return readyFreeFnItems_; }
    std::size_t readyMethodItems() const { return readyMethodItems_; }

private:
    enum class RequestOrigin : std::uint8_t {
        Sema,
        Mono,
    };

    static char prefixFor(WorkItemKind k) {
        switch (k) {
            case WorkItemKind::InstantiateClass:  return 'C';
            case WorkItemKind::InstantiateFreeFn: return 'F';
            case WorkItemKind::InstantiateMethod: return 'M';
        }
        return '?';
    }

    static std::string compositeKey(const std::string& key, WorkItemKind kind) {
        std::string composite;
        composite.reserve(key.size() + 2);
        composite.push_back(prefixFor(kind));
        composite.push_back('\x1f');
        composite.append(key);
        return composite;
    }

    void record(const std::string& key, WorkItemKind kind, RequestOrigin origin) {
        if (key.empty()) return;
        ++totalRequests_;
        switch (origin) {
            case RequestOrigin::Sema: ++semaRequests_; break;
            case RequestOrigin::Mono: ++monoRequests_; break;
        }
        switch (kind) {
            case WorkItemKind::InstantiateClass:  ++classRequests_;  break;
            case WorkItemKind::InstantiateFreeFn: ++freeFnRequests_; break;
            case WorkItemKind::InstantiateMethod: ++methodRequests_; break;
        }
        uniqueKeys_.insert(compositeKey(key, kind));
    }

    bool frozen_ = false;
    std::size_t totalRequests_ = 0;
    std::size_t semaRequests_ = 0;
    std::size_t monoRequests_ = 0;
    std::size_t classRequests_ = 0;
    std::size_t freeFnRequests_ = 0;
    std::size_t methodRequests_ = 0;
    std::size_t readyClassItems_ = 0;
    std::size_t readyFreeFnItems_ = 0;
    std::size_t readyMethodItems_ = 0;
    std::set<std::string> uniqueKeys_;
    std::set<std::string> readyKeys_;
};

} // namespace vyx::canon
