#include "pychron/grains/runner.hpp"

#include <chrono>
#include <future>
#include <memory>
#include <utility>

namespace pychron::grains {

namespace {
constexpr std::chrono::milliseconds kPoll{5};
}

GrainRunner::GrainRunner(std::function<FrameTick()> poll, GrainOptions options, std::function<void()> on_report)
    : poll_(std::move(poll)), options_(options), on_report_(std::move(on_report)), pipeline_(options) {
  thread_ = std::thread([this] { run(); });
}

GrainRunner::~GrainRunner() {
  {
    const std::lock_guard lock(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  thread_.join();
}

std::optional<GrainRunner::Shown> GrainRunner::take_report() {
  const std::lock_guard lock(mutex_);
  std::optional<Shown> out = std::move(mailbox_);
  mailbox_.reset();
  return out;
}

void GrainRunner::post(Shown shown) {
  {
    const std::lock_guard lock(mutex_);
    mailbox_ = std::move(shown);
  }
  if (on_report_) on_report_();
}

void GrainRunner::set_reference(vision::Vec2 frame_px, Side side, std::function<void(Result<void>)> done) {
  {
    const std::lock_guard lock(mutex_);
    commands_.emplace_back([this, frame_px, side, done = std::move(done)] {
      const Result<void> r = pipeline_.set_reference(frame_px, side);
      if (r && recording_) {
        RecordEvent e;
        e.kind = RecordEvent::Kind::Set;
        e.first = ReferenceMark{frame_px, side};
        events_.push_back(e);
      }
      if (done) done(r);
    });
  }
  cv_.notify_all();
}

void GrainRunner::set_second_reference(vision::Vec2 frame_px, Side side, std::function<void(Result<void>)> done) {
  {
    const std::lock_guard lock(mutex_);
    commands_.emplace_back([this, frame_px, side, done = std::move(done)] {
      const Result<void> r = pipeline_.set_second_reference(frame_px, side);
      if (r && recording_) {
        RecordEvent e;
        e.kind = RecordEvent::Kind::SetSecond;
        e.first = ReferenceMark{frame_px, side};
        events_.push_back(e);
      }
      if (done) done(r);
    });
  }
  cv_.notify_all();
}

void GrainRunner::clear_reference() {
  {
    const std::lock_guard lock(mutex_);
    commands_.emplace_back([this] {
      pipeline_.clear_reference();
      if (recording_) {
        RecordEvent e;
        e.kind = RecordEvent::Kind::Clear;
        events_.push_back(e);
      }
    });
  }
  cv_.notify_all();
}

Result<void> GrainRunner::start_recording(const std::filesystem::path& dir) {
  auto promise = std::make_shared<std::promise<Result<void>>>();
  std::future<Result<void>> future = promise->get_future();
  {
    const std::lock_guard lock(mutex_);
    commands_.emplace_back([this, dir, promise] {
      if (recording_) {
        promise->set_value(fail(ErrorKind::Config, "already recording"));
        return;
      }
      pipeline_.reset();
      auto opened = GrainRecording::open(dir, options_);
      if (!opened) {
        promise->set_value(fail(opened.error()));
        return;
      }
      recording_.emplace(std::move(*opened));
      recording_error_.reset();
      events_.clear();
      if (const auto first = pipeline_.reference_mark()) {
        RecordEvent e;
        e.kind = RecordEvent::Kind::Restore;
        e.first = *first;
        e.second = pipeline_.second_reference_mark();
        events_.push_back(e);
      }
      promise->set_value(Result<void>{});
    });
  }
  cv_.notify_all();
  return future.get();
}

Result<void> GrainRunner::stop_recording() {
  auto promise = std::make_shared<std::promise<Result<void>>>();
  std::future<Result<void>> future = promise->get_future();
  {
    const std::lock_guard lock(mutex_);
    commands_.emplace_back([this, promise] {
      if (!recording_ && !recording_error_) {
        promise->set_value(fail(ErrorKind::Config, "not recording"));
        return;
      }
      std::optional<Error> error = std::move(recording_error_);
      recording_error_.reset();
      if (recording_) {
        const Result<void> finished = recording_->finish();
        recording_.reset();
        if (!finished && !error) error = finished.error();
      }
      events_.clear();
      if (error) {
        promise->set_value(fail(*error));
        return;
      }
      promise->set_value(Result<void>{});
    });
  }
  cv_.notify_all();
  return future.get();
}

void GrainRunner::run() {
  for (;;) {
    std::vector<std::function<void()>> commands;
    bool stopping = false;
    {
      std::unique_lock lock(mutex_);
      commands.swap(commands_);
      stopping = stop_;
    }
    for (auto& command : commands) command();
    if (stopping) break;

    bool got_frame = false;
    FrameTick tick = poll_();
    if (tick.frame && !tick.error.empty()) tick.frame.reset();
    if (tick.error.empty()) last_error_.clear();
    if (tick.frame) {
      if (!last_seq_ || *last_seq_ != tick.frame->seq) {
        last_seq_ = tick.frame->seq;
        got_frame = true;
        GrainReport report = pipeline_.process(*tick.frame);
        if (recording_ && !recording_error_) {
          const Result<void> added = recording_->add(*tick.frame, report, events_);
          if (!added) recording_error_ = added.error();
        }
        events_.clear();
        Shown shown;
        shown.frame = std::move(*tick.frame);
        shown.report = std::move(report);
        post(std::move(shown));
      }
    } else if (!tick.error.empty() && tick.error != last_error_) {
      last_error_ = tick.error;
      Shown shown;
      shown.error = std::move(tick.error);
      post(std::move(shown));
    }

    if (!got_frame) {
      std::unique_lock lock(mutex_);
      cv_.wait_for(lock, kPoll, [this] { return stop_ || !commands_.empty(); });
    }
  }
  if (recording_) {
    (void)recording_->finish();
    recording_.reset();
  }
}

}  // namespace pychron::grains
