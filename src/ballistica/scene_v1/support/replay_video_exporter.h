// Released under the MIT License. See LICENSE for details.

#ifndef BALLISTICA_SCENE_V1_SUPPORT_REPLAY_VIDEO_EXPORTER_H_
#define BALLISTICA_SCENE_V1_SUPPORT_REPLAY_VIDEO_EXPORTER_H_

#include <memory>
#include <string>

#include "ballistica/base/graphics/video/video_encoder.h"
#include "ballistica/scene_v1/support/client_session_replay.h"

namespace ballistica::scene_v1 {

/// Orchestrates rendering a Ballistica Replay (.brp) file into an MP4 video
/// file on all supported platforms (Windows, macOS, Linux, Android, iOS).
///
/// Export runs entirely in the background, driven by the logic thread's
/// event loop and a dedicated encoder writer thread: simulation stepping,
/// frame capture, and file I/O are all decoupled so the export can proceed
/// as fast as the machine allows without blocking or freezing the app.
/// Progress is printed to stdout as it runs.
class ReplayVideoExporter {
 public:
  ReplayVideoExporter(std::string replay_path, std::string output_mp4_path,
                      int width = 1920, int height = 1080, int fps = 60,
                      int bitrate = 5000000);
  ~ReplayVideoExporter();

  /// Kick off the export. Returns true if the export was successfully
  /// started (this does not mean it has finished -- export happens
  /// asynchronously; see StartExportAsync).
  auto Export() -> bool;

  /// Launch the export process asynchronously on the logic thread event
  /// loop. Non-blocking: returns immediately while frames are captured
  /// and encoded in the background.
  static void StartExportAsync(std::string replay_path, std::string output_mp4_path,
                               int width = 1920, int height = 1080, int fps = 60,
                               int bitrate = 5000000);

 private:
  std::string replay_path_;
  std::string output_mp4_path_;
  int width_;
  int height_;
  int fps_;
  int bitrate_;
};

}  // namespace ballistica::scene_v1

#endif  // BALLISTICA_SCENE_V1_SUPPORT_REPLAY_VIDEO_EXPORTER_H_
