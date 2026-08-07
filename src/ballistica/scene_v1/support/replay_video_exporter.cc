#include "ballistica/scene_v1/support/replay_video_exporter.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

#include "ballistica/base/app_adapter/app_adapter.h"
#include "ballistica/base/base.h"
#include "ballistica/base/graphics/gl/gl_sys.h"
#include "ballistica/base/graphics/graphics.h"
#include "ballistica/base/logic/logic.h"
#include "ballistica/classic/support/classic_app_mode.h"
#include "ballistica/core/core.h"
#include "ballistica/core/logging/logging.h"
#include "ballistica/core/platform/platform.h"
#include "ballistica/scene_v1/node/globals_node.h"
#include "ballistica/scene_v1/support/client_session_replay.h"
#include "ballistica/scene_v1/support/scene.h"
#include "ballistica/scene_v1/scene_v1.h"
#include "ballistica/shared/foundation/event_loop.h"
#include "ballistica/shared/foundation/object.h"

namespace ballistica::scene_v1 {

// Drives the export frame-by-frame on the logic thread, with GPU readbacks
// dispatched to the main/render thread. The object is kept alive entirely
// through shared_ptr captures in every queued lambda — no external owner is
// needed once Start() has successfully queued the first callback.
class ReplayVideoExporterState
    : public std::enable_shared_from_this<ReplayVideoExporterState> {
 public:
  ReplayVideoExporterState(std::string replay_path, std::string output_path,
                           int width, int height, int fps, int bitrate)
      : replay_path_(std::move(replay_path)),
        output_mp4_path_(std::move(output_path)),
        width_(width),
        height_(height),
        fps_(fps > 0 ? fps : 30),
        bitrate_(bitrate) {}

  // Must be called from the logic thread immediately after construction.
  void Start() {
    auto* appmode = classic::ClassicAppMode::GetActiveOrThrow();
    try {
      appmode->LaunchReplaySession(replay_path_);
    } catch (const std::exception& e) {
      LogError("Failed to load replay file: " + replay_path_
               + " (" + e.what() + ")");
      return;
    }

    auto* session =
        dynamic_cast<ClientSessionReplay*>(appmode->GetForegroundSession());
    if (!session) {
      LogError("Failed to load replay file (no ClientSessionReplay): "
               + replay_path_);
      return;
    }
    if (!session->is_file_open()) {
      LogError("Failed to load replay file (file not open): " + replay_path_);
      return;
    }
    replay_session_ = session;

    // Tell the session to end (not loop) when it hits EOF.
    session->set_exporting(true);

    LogInfo("Starting video export: " + replay_path_ + " -> " + output_mp4_path_);
    start_time_ = std::chrono::steady_clock::now();
    last_progress_print_ = start_time_;

    // Query the true OpenGL viewport size on the render thread before
    // opening the encoder -- using a size bigger than the actual framebuffer
    // would give garbage pixels in the captured region.
    auto self_ptr = shared_from_this();
    if (g_base && g_base->app_adapter) {
      g_base->app_adapter->PushMainThreadCall(
          [self_ptr]() { self_ptr->OpenEncoderOnMainThread(); });
    } else {
      LogError("No app adapter available; cannot start capture.");
    }
  }

 private:
  // -----------------------------------------------------------------------
  // Render-thread entry point (called once before any frames are captured).
  // -----------------------------------------------------------------------
  void OpenEncoderOnMainThread() {
    int capture_w = width_;
    int capture_h = height_;
#if BA_ENABLE_OPENGL
    GLint viewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, viewport);
    if (viewport[2] > 0 && viewport[3] > 0) {
      capture_w = viewport[2];
      capture_h = viewport[3];
    }
#endif
    // H.264/yuv420p requires even dimensions.
    capture_w = capture_w > 1 ? (capture_w - (capture_w % 2)) : 2;
    capture_h = capture_h > 1 ? (capture_h - (capture_h % 2)) : 2;
    capture_width_ = capture_w;
    capture_height_ = capture_h;

    encoder_ = base::VideoEncoder::Create();
    if (!encoder_->Open(output_mp4_path_, capture_width_, capture_height_,
                        fps_, bitrate_, width_, height_)) {
      LogError("Failed to open video encoder for: " + output_mp4_path_);
      encoder_.reset();
    }

    // Hand control back to the logic thread.
    auto self_ptr = shared_from_this();
    if (g_base && g_base->logic && g_base->logic->event_loop()) {
      g_base->logic->event_loop()->PushCall([self_ptr]() {
        if (!self_ptr->encoder_) {
          // Encoder failed: return to main menu cleanly.
          try {
            classic::ClassicAppMode::GetActiveOrThrow()->RunMainMenu();
          } catch (...) {
          }
          return;
        }
        self_ptr->StepNextFrame();
      });
    }
  }

  // -----------------------------------------------------------------------
  // Logic-thread: queue the next step on the event loop.
  // -----------------------------------------------------------------------
  void ScheduleNextStep() {
    auto self_ptr = shared_from_this();
    if (g_base && g_base->logic && g_base->logic->event_loop()) {
      g_base->logic->event_loop()->PushCall(
          [self_ptr]() { self_ptr->StepNextFrame(); });
    }
  }

  // -----------------------------------------------------------------------
  // Logic-thread: advance simulation by one frame interval and queue capture.
  // -----------------------------------------------------------------------
  void StepNextFrame() {
    if (!replay_session_.exists() || !encoder_) return;
    if (replay_session_->shutting_down()) {
      Finalize();
      return;
    }

    bool eof = !replay_session_->is_file_open();

    replay_session_->Update(kGameStepMilliseconds, kGameStepSeconds);

    if (replay_session_->shutting_down()) {
      Finalize();
      return;
    }

    // Real-time per sim step = step_ms / game_speed.
    // epic mode: 8/0.32=25ms/step; normal: 8/1.0=8ms/step.
    float game_speed = 1.0f;
    if (auto* appmode = classic::ClassicAppMode::GetActive()) {
      if (auto* scene = appmode->GetForegroundScene()) {
        if (auto* gn = scene->globals_node()) {
          game_speed = gn->slow_motion() ? 0.32f : 1.0f;
        }
      }
    }
    real_accum_ms_ += static_cast<double>(kGameStepMilliseconds) / game_speed;

    const double frame_ms = 1000.0 / fps_;
    int n_frames = static_cast<int>(real_accum_ms_ / frame_ms);
    real_accum_ms_ -= n_frames * frame_ms;

    MaybePrintProgress();

    if (n_frames <= 0 && !eof) {
      ScheduleNextStep();
      return;
    }

    auto self_ptr = shared_from_this();
    bool do_eof = eof;
    // Capture one GL frame and encode it n_frames times (for slow-mo, n>1).
    CaptureFrames(n_frames > 0 ? n_frames : 1, [self_ptr, do_eof]() {
      if (do_eof) {
        self_ptr->Finalize();
      } else {
        self_ptr->ScheduleNextStep();
      }
    });
  }

  // Print progress to stdout at most once every 250 ms.
  // Must only be called from the logic thread.
  void MaybePrintProgress() {
    if (!encoder_) return;
    auto now = std::chrono::steady_clock::now();
    if (now - last_progress_print_ < std::chrono::milliseconds(250)) return;
    last_progress_print_ = now;

    double elapsed_s =
        std::chrono::duration<double>(now - start_time_).count();
    double sim_seconds = static_cast<double>(frame_idx_) / fps_;
    double speed = elapsed_s > 0.0 ? sim_seconds / elapsed_s : 0.0;

    std::printf(
        "\rExporting replay: %lld frames captured, %lld encoded to disk "
        "(%.1fs of replay processed, %.2fx realtime)   ",
        static_cast<long long>(frame_idx_),
        static_cast<long long>(encoder_->frames_encoded()),
        sim_seconds, speed);
    std::fflush(stdout);
  }


  // Capture one GL frame and encode it n_frames times.
  void CaptureFrames(int n_frames, std::function<void()> on_done) {
    if (!g_base || !g_base->app_adapter) {
      on_done();
      return;
    }
    auto self_ptr = shared_from_this();
    const int w = capture_width_;
    const int h = capture_height_;
    const int64_t base_idx = frame_idx_;
    frame_idx_ += n_frames;

    g_base->app_adapter->PushMainThreadCall(
        [self_ptr, w, h, base_idx, n_frames, on_done]() {
          self_ptr->DoCaptureOnMainThread(w, h, base_idx, n_frames);
          if (g_base && g_base->logic && g_base->logic->event_loop()) {
            g_base->logic->event_loop()->PushCall([on_done]() { on_done(); });
          }
        });
  }

  void DoCaptureOnMainThread(int w, int h, int64_t base_index, int n_frames) {
    if (!encoder_ || w <= 0 || h <= 0) return;
#if BA_ENABLE_OPENGL
    uint8_t* buf = encoder_->AcquireFrameBuffer();
    if (buf) {
      glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf);
      const size_t row_bytes = static_cast<size_t>(w) * 4;
      if (row_scratch_.size() < row_bytes) row_scratch_.resize(row_bytes);
      for (int y = 0; y < h / 2; ++y) {
        uint8_t* top = buf + y * row_bytes;
        uint8_t* bot = buf + (h - 1 - y) * row_bytes;
        std::memcpy(row_scratch_.data(), top, row_bytes);
        std::memcpy(top, bot, row_bytes);
        std::memcpy(bot, row_scratch_.data(), row_bytes);
      }
      // Single call with repeat_count; encoder writes it N times before
      // releasing the buffer — safe, no use-after-free.
      encoder_->EncodeFrame(buf, base_index, n_frames);
    }
#endif
  }

  void Finalize() {
    std::printf("\n");
    std::fflush(stdout);

    if (encoder_) {
      encoder_->Close();
      encoder_.reset();
    }

    LogInfo("Export capture complete (" + std::to_string(frame_idx_)
            + " frames) -- finishing MP4 write in the background: "
            + output_mp4_path_);
    // End() was already called on the session (which pushed the main-menu
    // relaunch); don't call RunMainMenu() here or it fires twice causing
    // double-Reset_ and the session-count / controller errors.
  }

  // -----------------------------------------------------------------------
  static void LogInfo(const std::string& msg) {
    if (core::g_core && core::g_core->logging) {
      core::g_core->logging->Log(LogName::kBa, LogLevel::kInfo,
                                 "[Replay Export] " + msg);
    }
  }
  static void LogError(const std::string& msg) {
    if (core::g_core && core::g_core->logging) {
      core::g_core->logging->Log(LogName::kBa, LogLevel::kError,
                                 "[Replay Export] " + msg);
    }
  }

  // -----------------------------------------------------------------------
  std::string replay_path_;
  std::string output_mp4_path_;
  int width_;         // requested output width
  int height_;        // requested output height
  int capture_width_{0};   // actual GL framebuffer width
  int capture_height_{0};  // actual GL framebuffer height
  int fps_;
  int bitrate_;
  int64_t frame_idx_{0};
  double real_accum_ms_{0.0};  // our own clock accumulator (not game time)
  std::chrono::steady_clock::time_point start_time_;
  std::chrono::steady_clock::time_point last_progress_print_;
  // row_scratch_ is only ever accessed from the render thread (one capture
  // at a time) so no locking is required.
  std::vector<uint8_t> row_scratch_;
  Object::Ref<ClientSessionReplay> replay_session_;
  std::unique_ptr<base::VideoEncoder> encoder_;
};

// ---------------------------------------------------------------------------

ReplayVideoExporter::ReplayVideoExporter(std::string replay_path,
                                         std::string output_mp4_path,
                                         int width, int height, int fps,
                                         int bitrate)
    : replay_path_(std::move(replay_path)),
      output_mp4_path_(std::move(output_mp4_path)),
      width_(width),
      height_(height),
      fps_(fps),
      bitrate_(bitrate) {}

ReplayVideoExporter::~ReplayVideoExporter() = default;

auto ReplayVideoExporter::Export() -> bool {
  StartExportAsync(replay_path_, output_mp4_path_, width_, height_, fps_,
                   bitrate_);
  return true;
}

void ReplayVideoExporter::StartExportAsync(std::string replay_path,
                                           std::string output_mp4_path,
                                           int width, int height, int fps,
                                           int bitrate) {
  auto state = std::make_shared<ReplayVideoExporterState>(
      std::move(replay_path), std::move(output_mp4_path), width, height, fps,
      bitrate);
  state->Start();
  // state's shared_ptr ref drops here, but if Start() successfully queued
  // any callbacks those lambdas each hold their own shared_ptr via
  // shared_from_this(), so the object remains alive as long as needed.
}

}  // namespace ballistica::scene_v1
