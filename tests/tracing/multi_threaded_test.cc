#include <cactus_rt/rt.h>
#include <gtest/gtest.h>
#include <quill/detail/LogManager.h>

#include <memory>

#include "helpers/assert_helpers.h"
#include "helpers/mock_sink.h"
#include "helpers/mock_threads.h"
#include "helpers/utils.h"

namespace {
const char* kAppName = "TestApp";
}

class MultiThreadTracingTest : public ::testing::Test {
  static cactus_rt::AppConfig CreateAppConfig() {
    cactus_rt::AppConfig config;
    return config;
  }

 protected:
  cactus_rt::App                     app_;
  std::shared_ptr<MockRegularThread> regular_thread_;
  std::shared_ptr<MockCyclicThread>  cyclic_thread_;
  std::shared_ptr<MockSink>          sink_;

 public:
  MultiThreadTracingTest()
      : app_(kAppName, CreateAppConfig()),
        regular_thread_(std::make_shared<MockRegularThread>()),
        cyclic_thread_(std::make_shared<MockCyclicThread>()),
        sink_(std::make_shared<MockSink>()) {}

 protected:
  void SetUp() override {
    app_.StartTraceSession();
    app_.RegisterTraceSink(sink_);  // TODO: make this registerable before the trace session starts.
  }

  void TearDown() override {
    app_.RequestStop();
    app_.Join();

    // Need to stop it for every test as every app.Start() will start a background thread.
    quill::detail::LogManagerSingleton::instance().log_manager().stop_backend_worker();
  }
};

TEST_F(MultiThreadTracingTest, TraceFromMultipleThreads) {
  app_.RegisterThread(regular_thread_);
  app_.RegisterThread(cyclic_thread_);

  app_.Start();

  regular_thread_->RunOneIteration([](MockRegularThread* self) {
    self->TracerForTest().InstantEvent("Event1");
    WasteTime(std::chrono::microseconds(1000));
  });

  cyclic_thread_->Join();
  regular_thread_->RequestStop();
  regular_thread_->Join();

  app_.StopTraceSession();

  auto traces = sink_->LoggedTraces();
  auto packets = GetPacketsFromTraces(traces);
  ASSERT_EQ(packets.size(), 44);  // 3 track descriptor + 20 spans + 1 event

  AssertIsProcessTrackDescriptor(*packets[0], kAppName);
  const auto process_track_uuid = packets[0]->track_descriptor().uuid();

  auto traces_grouped = GetPacketsGroupByThreads(traces);
  ASSERT_EQ(traces_grouped.size(), 2);

  const auto& regular_thread_traces = traces_grouped.at(kRegularThreadName);
  const auto& cyclic_thread_traces = traces_grouped.at(kCyclicThreadName);

  ASSERT_EQ(regular_thread_traces.size(), 2);
  ASSERT_EQ(cyclic_thread_traces.size(), 41);

  AssertIsThreadTrackDescriptor(*regular_thread_traces[0], kRegularThreadName, process_track_uuid);
  const auto regular_thread_track_uuid = regular_thread_traces[0]->track_descriptor().uuid();

  AssertIsTrackEventInstant(*regular_thread_traces[1], "Event1", nullptr, regular_thread_track_uuid);

  AssertIsThreadTrackDescriptor(*cyclic_thread_traces[0], kCyclicThreadName, process_track_uuid);
  const auto cyclic_thread_track_uuid = cyclic_thread_traces[0]->track_descriptor().uuid();

  AssertIsTrackEventSliceBegin(*cyclic_thread_traces[1], "CyclicThread::Loop", "cactusrt", cyclic_thread_track_uuid);
  auto sequence_id = cyclic_thread_traces[1]->trusted_packet_sequence_id();

  for (size_t i = 0; i < 20; i++) {
    auto begin_idx = 1 + (i * 2);
    auto end_idx = 1 + (i * 2) + 1;

    AssertIsTrackEventSliceBegin(*cyclic_thread_traces[begin_idx], "CyclicThread::Loop", "cactusrt", cyclic_thread_track_uuid, sequence_id);
    AssertIsTrackEventSliceEnd(*cyclic_thread_traces[end_idx], cyclic_thread_track_uuid, sequence_id);
  }
}

TEST_F(MultiThreadTracingTest, CyclicThreadTracesLoop) {
  // TODO: move the configuration for the number of loops and time per loop here
  // so it's easier to check the assertions are working.
  app_.RegisterThread(cyclic_thread_);
  app_.Start();

  // The cyclic thread should shutdown on its own.
  app_.Join();
  app_.StopTraceSession();

  auto traces = sink_->LoggedTraces();
  auto packets = GetPacketsFromTraces(traces);
  ASSERT_EQ(packets.size(), 42);  // 2 track descriptor + 20 spans

  AssertIsProcessTrackDescriptor(*packets[0], kAppName);
  const auto process_track_uuid = packets[0]->track_descriptor().uuid();

  AssertIsThreadTrackDescriptor(*packets[1], kCyclicThreadName, process_track_uuid);
  const auto thread_track_uuid = packets[1]->track_descriptor().uuid();

  AssertIsTrackEventSliceBegin(*packets[2], "CyclicThread::Loop", "cactusrt", thread_track_uuid);
  auto sequence_id = packets[2]->trusted_packet_sequence_id();

  for (size_t i = 0; i < 20; i++) {
    auto begin_idx = 2 + (i * 2);
    auto end_idx = 2 + (i * 2) + 1;

    AssertIsTrackEventSliceBegin(*packets[begin_idx], "CyclicThread::Loop", "cactusrt", thread_track_uuid, sequence_id);
    AssertIsTrackEventSliceEnd(*packets[end_idx], thread_track_uuid, sequence_id);
  }
}