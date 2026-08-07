// Released under the MIT License. See LICENSE for details.

#ifndef BALLISTICA_BASE_GRAPHICS_VIDEO_VIDEO_ENCODER_H_
#define BALLISTICA_BASE_GRAPHICS_VIDEO_VIDEO_ENCODER_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ballistica::base {

/// Abstract C++ video encoder interface for exporting raw frame buffers
/// into an MP4 container file across all platforms (Windows, macOS, Linux, Android, iOS).
///
/// Implementations are expected to perform the actual (potentially slow)
/// file/pipe I/O on a background thread so that callers of EncodeFrame()
/// never block on encoder throughput.
class VideoEncoder {
 public:
  virtual ~VideoEncoder() = default;

  /// Open destination MP4 video file and initialize encoder stream.
  /// capture_width/capture_height must match the actual size of the
  /// pixel buffers that will be passed to EncodeFrame(). If
  /// output_width/output_height are given and differ, the encoder scales
  /// to that size internally (via ffmpeg) -- capture and final output
  /// resolution don't need to match.
  virtual auto Open(const std::string& output_path, int capture_width,
                    int capture_height, int fps, int bitrate = 5000000,
                    int output_width = 0, int output_height = 0) -> bool = 0;

  /// Get a reusable RGBA buffer (width*height*4 bytes, as clamped/rounded
  /// by Open()) to fill with pixel data before calling EncodeFrame().
  /// Buffers are recycled internally; do not free the returned pointer.
  virtual auto AcquireFrameBuffer() -> uint8_t* = 0;

  /// Hand off a filled buffer (previously returned by AcquireFrameBuffer)
  /// for encoding. Ownership of the buffer transfers to the encoder;
  /// the caller must not touch it again. This call does not block on
  /// disk/pipe I/O -- it just enqueues the frame for a background
  /// writer thread.
  virtual auto EncodeFrame(const uint8_t* rgba_data, int64_t frame_index,
                           int repeat_count = 1) -> bool = 0;

  /// Finalize video stream: waits for the background writer thread to
  /// flush all queued frames, then closes the output MP4 container.
  virtual void Close() = 0;

  /// Number of frames actually written to the output file so far.
  virtual auto frames_encoded() const -> int64_t = 0;

  /// Number of frames handed to EncodeFrame so far (includes ones still
  /// sitting in the write queue, not yet flushed to disk).
  virtual auto frames_queued() const -> int64_t = 0;

  /// Factory method to instantiate the optimal video encoder for the target platform.
  static auto Create() -> std::unique_ptr<VideoEncoder>;
};

}  // namespace ballistica::base

#endif  // BALLISTICA_BASE_GRAPHICS_VIDEO_VIDEO_ENCODER_H_
