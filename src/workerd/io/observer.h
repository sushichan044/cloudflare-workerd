// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// Defines abstract interfaces for observing the activity of various components of the system,
// e.g. to collect logs and metrics.

#include <workerd/io/features.capnp.h>
#include <workerd/io/trace.h>
#include <workerd/jsg/observer.h>
#include <workerd/util/sqlite.h>

#include <kj/exception.h>
#include <kj/refcount.h>
#include <kj/string.h>
#include <kj/time.h>

namespace workerd {

class IoContext;
class WorkerInterface;
class LimitEnforcer;
class TimerChannel;

class WebSocketObserver: public kj::Refcounted {
 public:
  virtual ~WebSocketObserver() noexcept(false) = default;
  // Called when a worker sends a message on this WebSocket (includes close messages).
  virtual void sentMessage(size_t bytes) {};
  // Called when a worker receives a message on this WebSocket (includes close messages).
  virtual void receivedMessage(size_t bytes) {};
};

// Observes a byte stream. Byte streams which use instances of this observer should call enqueue()
// and dequeue() once for each chunk that passes through the stream. The order of enqueues should
// match the order of dequeues.
//
// Byte observer implementations can then calculate the current number of chunks and the sum of the
// size of the chunks in the internal queue by incrementing and decrementing each metric in
// enqueue() and dequeue() respectively.
class ByteStreamObserver {
 public:
  virtual ~ByteStreamObserver() noexcept(false) = default;
  // Called when a chunk of size `bytes` is enqueued on the stream.
  virtual void onChunkEnqueued(size_t bytes) {};
  // Called when a chunk of size `bytes` is dequeued from the stream (e.g. when a writable byte
  // stream writes the chunk to its corresponding sink).
  virtual void onChunkDequeued(size_t bytes) {};
};

// Observes a specific request to a specific worker. Also observes outgoing subrequests.
//
// Observing anything is optional. Default implementations of all methods observe nothing.
class RequestObserver: public kj::Refcounted {
 public:
  // This is called when the request is converted to a WebSocket connection terminating in a worker.
  // An optional WebSocket observer may be returned to observe events on the worker's end of the
  // WebSocket connection.
  //
  // This means that, when the returned observer observes a message being sent, the message is being
  // sent from the worker to the client making the request.
  virtual kj::Maybe<kj::Own<WebSocketObserver>> tryCreateWebSocketObserver() {
    return kj::none;
  };

  // This is called when a writable byte stream is created whilst processing this request. It will
  // be destroyed when the corresponding byte stream is destroyed.
  virtual kj::Maybe<kj::Own<ByteStreamObserver>> tryCreateWritableByteStreamObserver() {
    return kj::none;
  }

  // Invoked when the request is actually delivered.
  //
  // If, for some reason, this is not invoked before the object is destroyed, this indicate that
  // the event was canceled for some reason before delivery. No JavaScript was invoked. In this
  // case, the request should not be billed.
  virtual void delivered() {};

  // Call when no more JavaScript will run on behalf of this request. Note that deferred proxying
  // may still be in progress.
  virtual void jsDone() {}

  // Called to indicate this was a prewarm request. Normal request metrics won't be logged, but
  // the prewarm metric will be incremented.
  virtual void setIsPrewarm() {}

  // Describes the source of a failure
  enum class FailureSource : uint8_t {
    // Failure occurred during deferred proxying
    DEFERRED_PROXY,

    // Failure occurred elsewhere
    OTHER,
  };

  // Report that the request failed with the given exception. This only needs to be called in
  // cases where the wrapper created with wrapWorkerInterface() wouldn't otherwise see the
  // exception, e.g. because it has been replaced with an HTTP error response or because it
  // occurred asynchronously.
  virtual void reportFailure(const kj::Exception& e, FailureSource source = FailureSource::OTHER) {}

  // Wrap the given WorkerInterface with a version that collects metrics. This method may only be
  // called once, and only one method call may be made to the returned interface.
  //
  // The returned reference remains valid as long as the observer and `worker` both remain live.
  virtual WorkerInterface& wrapWorkerInterface(WorkerInterface& worker) {
    return worker;
  }

  // Wrap an HttpClient so that its usage is counted in the request's subrequest stats.
  virtual kj::Own<WorkerInterface> wrapSubrequestClient(kj::Own<WorkerInterface> client) {
    return kj::mv(client);
  }

  // Wrap an HttpClient so that its usage is counted in the request's actor subrequest count.
  virtual kj::Own<WorkerInterface> wrapActorSubrequestClient(kj::Own<WorkerInterface> client) {
    return kj::mv(client);
  }

  // Used to record when a worker has used a dynamic dispatch binding.
  virtual void setHasDispatched() {};

  virtual SpanParent getSpan() {
    return nullptr;
  }

  virtual void setOutcome(EventOutcome outcome) {}

  virtual kj::Own<void> addedContextTask() {
    return kj::Own<void>();
  }
  virtual kj::Own<void> addedWaitUntilTask() {
    return kj::Own<void>();
  }

  virtual void setFailedOpen(bool value) {}

  virtual uint64_t clockRead() {
    return 0;
  }
};

class JsgIsolateObserver: public kj::AtomicRefcounted, public jsg::IsolateObserver {};

class IsolateObserver: public kj::AtomicRefcounted {
 public:
  virtual ~IsolateObserver() noexcept(false) {}

  // Called when Worker::Isolate is created.
  virtual void created() {};

  // Called when the owning Worker::Script is being destroyed. The IsolateObserver may
  // live a while longer to handle deferred proxy requests.
  virtual void evicted() {}

  virtual void teardownStarted() {}
  virtual void teardownLockAcquired() {}
  virtual void teardownFinished() {}

  // Describes why a worker was started.
  enum class StartType : uint8_t {
    // Cold start with active request waiting.
    COLD,

    // Started due to prewarm hint (e.g. from TLS SNI); a real request is expected soon.
    PREWARM,

    // Started due to preload at process startup.
    PRELOAD
  };

  // Created while parsing a script, to record related metrics.
  class Parse {
   public:
    // Marks the ScriptReplica as finished parsing, which starts reporting of isolate metrics.
    virtual void done() {}
  };

  virtual kj::Own<Parse> parse(StartType startType) const {
    class FinalParse final: public Parse {};
    return kj::heap<FinalParse>();
  }

  class LockTiming {
   public:
    // Called by `Isolate::takeAsyncLock()` when it is blocked by a different isolate lock on the
    // same thread.
    virtual void waitingForOtherIsolate(kj::StringPtr id) {}

    // Call if this is an async lock attempt, before constructing LockRecord.
    virtual void reportAsyncInfo(
        uint currentLoad, bool threadWaitingSameLock, uint threadWaitingDifferentLockCount) {}
    // TODO(cleanup): Should be able to get this data at `tryCreateLockTiming()` time. It'd be
    //   easier if IsolateObserver were an AOP class, and thus had access to the real isolate.

    virtual void start() {}
    virtual void stop() {}

    virtual void locked() {}
    virtual void gcPrologue() {}
    virtual void gcEpilogue() {}
  };

  // Construct a LockTiming if config.reportScriptLockTiming is true, or if the
  // request (if any) is being traced.
  virtual kj::Maybe<kj::Own<LockTiming>> tryCreateLockTiming(
      kj::OneOf<SpanParent, kj::Maybe<RequestObserver&>> parentOrRequest) const {
    return kj::none;
  }

  // Use like so:
  //
  //   auto lockTiming = MetricsCollector::ScriptReplica::LockTiming::tryCreate(
  //       script, maybeRequest);
  //   MetricsCollector::ScriptReplica::LockRecord record(lockTiming);
  //   isolate.runInLockScope([&](MyIsolate::Lock& lock) {
  //     record.locked();
  //   });
  //
  // And `record()` will report the time spent waiting for the lock (including any asynchronous
  // time you might insert between the construction of `lockTiming` and `LockRecord()`), plus
  // the time spent holding the lock for the given ScriptReplica.
  //
  // This is a thin wrapper around LockTiming which efficiently handles the case where we don't
  // want to track timing.
  class LockRecord {
   public:
    explicit LockRecord(kj::Maybe<kj::Own<LockTiming>> lockTimingParam)
        : lockTiming(kj::mv(lockTimingParam)) {
      KJ_IF_SOME(l, lockTiming) l.get()->start();
    }
    ~LockRecord() noexcept(false) {
      KJ_IF_SOME(l, lockTiming) l.get()->stop();
    }
    KJ_DISALLOW_COPY_AND_MOVE(LockRecord);

    void locked() {
      KJ_IF_SOME(l, lockTiming) l.get()->locked();
    }
    void gcPrologue() {
      KJ_IF_SOME(l, lockTiming) l.get()->gcPrologue();
    }
    void gcEpilogue() {
      KJ_IF_SOME(l, lockTiming) l.get()->gcEpilogue();
    }

   private:
    // The presence of `lockTiming` determines whether or not we need to record timing data. If
    // we have no `lockTiming`, then this LockRecord wrapper is just a big nothingburger.
    kj::Maybe<kj::Own<LockTiming>> lockTiming;
  };
};

class WorkerObserver: public kj::AtomicRefcounted {
 public:
  // Created while executing a script's global scope, to record related metrics.
  class Startup {
   public:
    virtual void done() {}
  };

  virtual kj::Own<Startup> startup(IsolateObserver::StartType startType) const {
    class FinalStartup final: public Startup {};
    return kj::heap<FinalStartup>();
  }

  virtual void teardownStarted() {}
  virtual void teardownLockAcquired() {}
  virtual void teardownFinished() {}
};

class ActorObserver: public kj::Refcounted, public SqliteObserver {
 public:
  // Allows the observer to run in the background, periodically making observations. Owner must
  // call this and store the promise. `limitEnforcer` is used to collect CPU usage metrics, it
  // must remain valid as long as the loop is running.
  virtual kj::Promise<void> flushLoop(TimerChannel& timer, LimitEnforcer& limitEnforcer) {
    return kj::NEVER_DONE;
  }

  virtual void startRequest() {}
  virtual void endRequest() {}

  virtual void webSocketAccepted() {}
  virtual void webSocketClosed() {}
  virtual void receivedWebSocketMessage(size_t bytes) {}
  virtual void sentWebSocketMessage(size_t bytes) {}

  virtual void addCachedStorageReadUnits(uint32_t units) {}
  virtual void addUncachedStorageReadUnits(uint32_t units) {}
  virtual void addStorageWriteUnits(uint32_t units) {}
  virtual void addStorageDeletes(uint32_t count) {}

  virtual void storageReadCompleted(kj::Duration latency) {}
  virtual void storageWriteCompleted(kj::Duration latency) {}

  virtual void inputGateLocked() {}
  virtual void inputGateReleased() {}
  virtual void inputGateWaiterAdded() {}
  virtual void inputGateWaiterRemoved() {}
  virtual void outputGateLocked() {}
  virtual void outputGateReleased() {}
  virtual void outputGateWaiterAdded() {}
  virtual void outputGateWaiterRemoved() {}

  virtual void shutdown(uint16_t reasonCode, LimitEnforcer& limitEnforcer) {}
};

// RAII object to call `teardownFinished()` on an observer for you.
template <typename Observer>
class TeardownFinishedGuard {
 public:
  TeardownFinishedGuard(Observer& ref): ref(ref) {}
  ~TeardownFinishedGuard() noexcept(false) {
    ref.teardownFinished();
  }
  KJ_DISALLOW_COPY_AND_MOVE(TeardownFinishedGuard);

 private:
  Observer& ref;
};

// Provides counters/observers for various features. The intent is to
// make it possible to collect metrics on which runtime features are
// used and how often.
//
// There is exactly one instance of this class per worker process.
class FeatureObserver {
 public:
  static kj::Own<FeatureObserver> createDefault();
  static void init(kj::Own<FeatureObserver> instance);
  static kj::Maybe<FeatureObserver&> get();

  // A "Feature" is just an opaque identifier defined in the features.capnp
  // file.
  using Feature = workerd::Features;

  // Called to increment the usage counter for a feature.
  virtual void use(Feature feature) const {}

  using CollectCallback = kj::Function<void(Feature, const uint64_t)>;
  // This method is called from the internal metrics collection mechanism to harvest the
  // current features and counts that have been recorded by the observer.
  virtual void collect(CollectCallback&& callback) const {}

  // Records the use of the feature if a FeatureObserver is available.
  static inline void maybeRecordUse(Feature feature) {
    KJ_IF_SOME(observer, get()) {
      observer.use(feature);
    }
  }
};

}  // namespace workerd
