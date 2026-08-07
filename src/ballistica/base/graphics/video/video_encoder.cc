// Released under the MIT License. See LICENSE for details.

#include "ballistica/base/graphics/video/video_encoder.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>

#include "ballistica/core/core.h"
#include "ballistica/core/logging/logging.h"
#include "ballistica/shared/buildconfig/buildconfig_common.h"

#if BA_PLATFORM_WINDOWS
#include <fcntl.h>
#include <io.h>
#define POPEN _popen
#define PCLOSE _pclose
#else
#define POPEN popen
#define PCLOSE pclose
#endif

namespace ballistica::base {

// Actually test-encodes one tiny throwaway frame with a candidate encoder
// to see if it works on this machine (rather than just trusting that
// ffmpeg *lists* it -- a build can list h264_nvenc with no NVIDIA GPU
// present). Cheap (~64x64, 1 frame) and only ever run once per process.
static bool ProbeEncoderWorks(const std::string& encoder_name) {
  std::string cmd = "ffmpeg -y -hide_banner -loglevel error -f lavfi -i "
                     "color=black:s=64x64 -frames:v 1 -c:v " + encoder_name
                     + " -f null - 2>&1";
  FILE* p = POPEN(cmd.c_str(), "r");
  if (!p) return false;
  char buf[256];
  while (fread(buf, 1, sizeof(buf), p) > 0) {
  }  // drain output so the process can exit cleanly
  return PCLOSE(p) == 0;
}

// Picks the fastest working H.264 encoder available on this machine,
// checked in priority order (NVIDIA -> Intel -> AMD -> Apple -> CPU
// fallback). Result is cached for the lifetime of the process since
// hardware doesn't change mid-run and each probe costs real time.
static auto PickBestEncoder() -> const std::string& {
  static const std::string kChosen = [] {
    static const char* kCandidates[] = {"h264_nvenc", "h264_qsv", "h264_amf",
                                        "h264_videotoolbox"};
    for (const char* c : kCandidates) {
      if (ProbeEncoderWorks(c)) {
        if (core::g_core && core::g_core->logging) {
          core::g_core->logging->Log(LogName::kBa, LogLevel::kInfo,
                                     std::string("VideoEncoder: using hardware encoder ") + c);
        }
        return std::string(c);
      }
    }
    if (core::g_core && core::g_core->logging) {
      core::g_core->logging->Log(
          LogName::kBa, LogLevel::kInfo,
          "VideoEncoder: no working hardware encoder found, using libx264 (CPU)");
    }
    return std::string("libx264");
  }();
  return kChosen;
}

// Builds the "-c:v ... <encoder-specific speed/quality flags>" portion of
// the ffmpeg command for whichever encoder was picked.
static auto BuildEncoderArgs(const std::string& encoder, int bitrate) -> std::string {
  const std::string br = std::to_string(bitrate);
  const std::string br2 = std::to_string(bitrate * 2);
  if (encoder == "h264_nvenc") {
    return "-c:v h264_nvenc -preset p1 -tune ll -rc vbr -b:v " + br
           + " -maxrate " + br + " -bufsize " + br2;
  }
  if (encoder == "h264_qsv") {
    return "-c:v h264_qsv -preset veryfast -b:v " + br;
  }
  if (encoder == "h264_amf") {
    return "-c:v h264_amf -quality speed -b:v " + br;
  }
  if (encoder == "h264_videotoolbox") {
    return "-c:v h264_videotoolbox -realtime 1 -b:v " + br;
  }
  // Software fallback -- tuned for max speed over max compression, since
  // this needs to run acceptably on everyday/low-end CPUs.
  return "-c:v libx264 -preset ultrafast -tune fastdecode -b:v " + br
         + " -maxrate " + br + " -bufsize " + br2;
}

// Cross-Platform Video Encoder implementation with FFmpeg pipe & direct file
// fallback. All potentially-blocking disk/pipe writes happen on a dedicated
// background thread so that EncodeFrame() (called from the render thread)
// never stalls waiting on FFmpeg. Pixel buffers are pooled and recycled to
// avoid a heap allocation every frame.
class VideoEncoderFallback : public VideoEncoder {
 public:
  VideoEncoderFallback() = default;
  ~VideoEncoderFallback() override { Close(); }

  auto Open(const std::string& output_path, int capture_width,
            int capture_height, int fps, int bitrate, int output_width,
            int output_height) -> bool override {
    output_path_ = output_path;

    // libx264/most encoders require even width/height for yuv420p.
    width_ = capture_width > 1 ? (capture_width - (capture_width % 2)) : 2;
    height_ = capture_height > 1 ? (capture_height - (capture_height % 2)) : 2;
    fps_ = fps > 0 ? fps : 30;
    bitrate_ = bitrate > 0 ? bitrate : 5000000;
    frame_bytes_ = static_cast<size_t>(width_) * static_cast<size_t>(height_) * 4;
    frames_encoded_ = 0;
    frames_queued_ = 0;
    stop_writer_ = false;

    int out_w = output_width > 1 ? (output_width - (output_width % 2)) : 0;
    int out_h = output_height > 1 ? (output_height - (output_height % 2)) : 0;
    bool needs_scale = out_w > 0 && out_h > 0 && (out_w != width_ || out_h != height_);

    // Pick the fastest H.264 encoder that actually works on this machine
    // (hardware if available, tuned software fallback otherwise) and
    // launch the FFmpeg pipeline to encode the raw RGBA frame stream.
    // -loglevel error keeps FFmpeg's own chatter out of the caller's
    // progress output.
    const std::string& encoder = PickBestEncoder();
    std::string inner_cmd =
        "ffmpeg -y -loglevel error -f rawvideo -pixel_format rgba -video_size "
        + std::to_string(width_) + "x" + std::to_string(height_)
        + " -framerate " + std::to_string(fps_)
        + " -i pipe:0 -an";
    if (needs_scale) {
      inner_cmd += " -vf scale=" + std::to_string(out_w) + ":" + std::to_string(out_h);
    }
    inner_cmd += " -pix_fmt yuv420p " + BuildEncoderArgs(encoder, bitrate_)
               + " -movflags +faststart \"" + output_path_ + "\"";
    // On Windows, _popen runs via cmd.exe /c; when the inner command
    // contains quoted arguments, the entire string must itself be wrapped
    // in outer quotes so that cmd.exe interprets it as a single compound
    // command rather than splitting on the inner quote boundaries.
#if BA_PLATFORM_WINDOWS
    std::string cmd = "\"" + inner_cmd + "\"";
#else
    std::string cmd = inner_cmd;
#endif
    pipe_ = POPEN(cmd.c_str(), "wb");

#if BA_PLATFORM_WINDOWS
    if (pipe_) {
      _setmode(_fileno(pipe_), _O_BINARY);
    }
#endif

    if (!pipe_) {
      // Fallback: write raw video stream directly to file.
      file_ = fopen(output_path_.c_str(), "wb");
      if (core::g_core && core::g_core->logging) {
        core::g_core->logging->Log(
            LogName::kBa, LogLevel::kWarning,
            "VideoEncoder: could not launch ffmpeg, falling back to raw "
            "output (no MP4 encoding) for: " + output_path_);
      }
    }

    is_open_ = (pipe_ != nullptr || file_ != nullptr);
    if (is_open_) {
      writer_thread_ = std::thread([this] { WriterLoop(); });
    }

    if (core::g_core && core::g_core->logging) {
      core::g_core->logging->Log(
          LogName::kBa, LogLevel::kInfo,
          "Opened VideoEncoder output stream for: " + output_path_ + " ("
              + std::to_string(width_) + "x" + std::to_string(height_) + "@"
              + std::to_string(fps_) + "fps, "
              + std::to_string(bitrate_ / 1000) + "kbps)");
    }
    return is_open_;
  }

  auto AcquireFrameBuffer() -> uint8_t* override {
    std::unique_lock<std::mutex> lock(pool_mutex_);
    // Return a recycled buffer immediately if one is available.
    if (!free_buffers_.empty()) {
      uint8_t* buf = free_buffers_.back();
      free_buffers_.pop_back();
      return buf;
    }
    // Allocate a fresh buffer if we are below the soft cap.
    // We never block here: this may be called from the render thread and
    // stalling it would freeze the engine.
    if (all_buffers_.size() < kMaxPooledBuffers) {
      all_buffers_.push_back(std::make_unique<uint8_t[]>(frame_bytes_));
      return all_buffers_.back().get();
    }
    // At soft cap: wait briefly (non-blocking spin with a yield) so that
    // the writer thread can catch up and free a buffer.  We release the
    // lock so the writer can acquire pool_mutex_ in ReleaseBuffer.
    lock.unlock();
    while (true) {
      std::this_thread::yield();
      lock.lock();
      if (!free_buffers_.empty()) {
        uint8_t* buf = free_buffers_.back();
        free_buffers_.pop_back();
        return buf;
      }
      if (all_buffers_.size() < kMaxPooledBuffers) {
        all_buffers_.push_back(std::make_unique<uint8_t[]>(frame_bytes_));
        return all_buffers_.back().get();
      }
      lock.unlock();
    }
  }

  auto EncodeFrame(const uint8_t* rgba_data, int64_t frame_index,
                   int repeat_count = 1) -> bool override {
    if (!is_open_ || rgba_data == nullptr || repeat_count < 1) return false;
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      write_queue_.push_back(
          QueueItem{const_cast<uint8_t*>(rgba_data), frame_index, repeat_count});
    }
    frames_queued_ += repeat_count;
    queue_cv_.notify_one();
    return true;
  }

  void Close() override {
    if (!is_open_) return;
    is_open_ = false;

    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      stop_writer_ = true;
    }
    queue_cv_.notify_all();
    if (writer_thread_.joinable()) {
      // Drain all queued fwrite() calls, then the writer exits.
      writer_thread_.join();
    }

    // Null the handles under pool_mutex_ so the (now-joined) writer
    // thread's local snapshot guard is consistent -- and so that any
    // lingering AcquireFrameBuffer call (which also holds pool_mutex_)
    // cannot see a dangling handle.  We capture them first to pass to
    // the detached finalization thread.
    FILE* pipe;
    FILE* file;
    {
      std::lock_guard<std::mutex> lock(pool_mutex_);
      pipe = pipe_;
      file = file_;
      pipe_ = nullptr;
      file_ = nullptr;
    }
    std::string output_path = output_path_;
    int64_t total_frames = frames_encoded_.load();

    // Closing the ffmpeg pipe (PCLOSE) waits for the ffmpeg process to
    // fully exit, which can block for a nontrivial amount of wall-clock
    // time while it finalizes/remuxes the MP4 (esp. with +faststart).
    // Hand it off to a detached thread that touches only the raw FILE*
    // handles (never `this`), safe even if the encoder is destroyed
    // right after Close() returns.
    std::thread([pipe, file, output_path, total_frames]() {
      if (pipe) PCLOSE(pipe);
      if (file) fclose(file);
      if (core::g_core && core::g_core->logging) {
        core::g_core->logging->Log(
            LogName::kBa, LogLevel::kInfo,
            "Finalized MP4 export (" + std::to_string(total_frames)
                + " frames written to " + output_path + ")");
      }
    }).detach();
  }

  auto frames_encoded() const -> int64_t override { return frames_encoded_.load(); }
  auto frames_queued() const -> int64_t override { return frames_queued_.load(); }

 private:
  struct QueueItem {
    uint8_t* buffer;
    int64_t frame_index;
    int repeat_count{1};
  };

  // Runs on a dedicated background thread for the lifetime of the encoder.
  // This is the only place that touches the FFmpeg pipe / output file, so
  // slow disk or encoder throughput never stalls the caller of EncodeFrame.
  void WriterLoop() {
    for (;;) {
      QueueItem item{};
      {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        queue_cv_.wait(lock,
                       [this] { return stop_writer_ || !write_queue_.empty(); });
        if (write_queue_.empty()) {
          if (stop_writer_) return;
          continue;
        }
        item = write_queue_.front();
        write_queue_.pop_front();
      }

      // Take a local snapshot of the destination handle under pool_mutex_
      // so we don't race against Close() nulling pipe_/file_.
      FILE* dst;
      {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        dst = pipe_ ? pipe_ : file_;
      }
      if (dst && item.buffer) {
        for (int r = 0; r < item.repeat_count; ++r) {
          size_t written = fwrite(item.buffer, 1, frame_bytes_, dst);
          if (written < frame_bytes_ && ferror(dst)) clearerr(dst);
          frames_encoded_++;
        }
      }
      ReleaseBuffer(item.buffer);
    }
  }

  void ReleaseBuffer(uint8_t* buf) {
    {
      std::lock_guard<std::mutex> lock(pool_mutex_);
      free_buffers_.push_back(buf);
    }
    pool_cv_.notify_one();
  }

  static constexpr size_t kMaxPooledBuffers = 24;

  std::mutex pool_mutex_;
  std::condition_variable pool_cv_;
  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::thread writer_thread_;
  bool stop_writer_{false};

  std::vector<std::unique_ptr<uint8_t[]>> all_buffers_;
  std::vector<uint8_t*> free_buffers_;
  std::deque<QueueItem> write_queue_;

  std::string output_path_;
  int width_{1920};
  int height_{1080};
  int fps_{60};
  int bitrate_{5000000};
  size_t frame_bytes_{0};
  std::atomic<int64_t> frames_encoded_{0};
  std::atomic<int64_t> frames_queued_{0};
  bool is_open_{false};
  FILE* pipe_{nullptr};
  FILE* file_{nullptr};
};

auto VideoEncoder::Create() -> std::unique_ptr<VideoEncoder> {
  return std::make_unique<VideoEncoderFallback>();
}

}  // namespace ballistica::base
