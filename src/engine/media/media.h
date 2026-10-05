// Kite Engine - <audio> and <video> playback.
//
// Every media element gets a MediaPlayer that demuxes and decodes on its own
// thread (FFmpeg's decoders, LGPL, linked statically when the build found
// them), converts video frames to BGRA itself and feeds audio to a platform
// AudioSink (waveOut on Windows). The UI thread only reads status, events
// and the latest frame; the renderer shows frames through "kite-media:<id>"
// image URLs, like canvases.
#ifndef KITE_MEDIA_MEDIA_H
#define KITE_MEDIA_MEDIA_H

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/geometry.h"
#include "image/image.h"

namespace kite {

class Node;

// Platform audio output for 16-bit interleaved stereo PCM.
class AudioSink {
 public:
  virtual ~AudioSink() {}
  virtual bool Open(int sampleRate) = 0;
  // Queues |frames| stereo frames (the data is copied). Never blocks.
  virtual void Write(const int16_t* samples, int frames) = 0;
  // Frames played since Open() or the last Flush().
  virtual long long Played() = 0;
  virtual void SetPaused(bool paused) = 0;
  // Drops everything queued and restarts the Played() counter at 0.
  virtual void Flush() = 0;
};
typedef AudioSink* (*AudioSinkFactory)();
// Installs the platform audio output. Without one (tests, kite-dump) audio
// runs against a silent clock.
void SetAudioSinkFactory(AudioSinkFactory f);
AudioSink* CreateNullAudioSink();

struct MediaStatus {
  double currentTime = 0;
  double duration = 0;       // NaN = unknown, infinity = live stream
  double bufferedEnd = 0;    // seconds known to be downloaded
  int readyState = 0;        // HAVE_NOTHING .. HAVE_ENOUGH_DATA
  int networkState = 0;      // NETWORK_EMPTY .. NETWORK_NO_SOURCE
  int error = 0;             // MediaError code (0 = none)
  bool paused = true;
  bool ended = false;
  bool seeking = false;
  bool started = false;      // play() was called at least once
  bool hasAudio = false;
  bool hasVideo = false;
  int videoWidth = 0, videoHeight = 0;
  std::string errorMessage;
};

class MediaPlayer {
 public:
  // |owner| identifies the document the element belongs to (event routing).
  MediaPlayer(const std::string& url, const std::string& referrer, bool audioOnly, Node* element,
              const void* owner);
  ~MediaPlayer();
  MediaPlayer(const MediaPlayer&) = delete;
  MediaPlayer& operator=(const MediaPlayer&) = delete;

  int id() const { return id_; }
  const std::string& url() const { return url_; }
  Node* element() const { return element_; }
  const void* owner() const { return owner_; }

  // Initial state without volumechange events.
  void Init(bool muted, double volume, bool loop);
  void Play();
  void Pause();
  void Seek(double seconds);
  void SetVolume(double v);
  void SetMuted(bool m);
  void SetLoop(bool l);
  void SetPlaybackRate(double r);
  double volume() const { return volume_; }
  bool muted() const { return muted_; }
  double playbackRate() const { return rate_; }

  MediaStatus Status() const;
  // DOM events queued since the last call ("loadedmetadata", "play", ...).
  std::vector<std::string> TakeEvents();
  bool HasEvents() const;
  // Latest decoded video frame (UI thread only; valid until the next call).
  const DecodedImage* Frame(unsigned* version);

  struct Shared;

 private:
  int id_;
  std::string url_;
  Node* element_;
  const void* owner_;
  double volume_ = 1, rate_ = 1;
  bool muted_ = false;
  std::shared_ptr<Shared> shared_;
  DecodedImage shown_;
  unsigned shownVersion_ = 0;
};

// Registry for the renderer and the bindings.
MediaPlayer* FindMediaPlayer(int id);
std::string MediaUrl(int id);
const DecodedImage* MediaFrameForUrl(const std::string& url, unsigned* version);
// Bumped whenever any player shows a new frame or changes state.
unsigned MediaGeneration();

// The player of a media element, created (or replaced when the source
// changed) on demand. |baseUrl| resolves relative sources. Returns null if
// the element has no usable source.
MediaPlayer* EnsureMediaPlayer(Node* el, const std::string& baseUrl, const void* owner);
// Source the element would play: src attribute or the first playable
// <source> child (unresolved).
std::string MediaSourceAttr(Node* el);
void DestroyMediaPlayer(int id);
// Events of all players belonging to |owner|.
std::vector<std::pair<Node*, std::string> > TakeMediaEvents(const void* owner);
bool MediaEventsPending(const void* owner);
// Starts muted autoplay elements and creates players for videos that show
// their first frame (preload != none). Returns true if a player was created.
bool UpdateMediaElements(Node* root, const std::string& baseUrl, const void* owner);

// Built-in controls (controls attribute): geometry shared by the painter
// and the click handling, in the element's content box coordinates.
struct MediaControls {
  Rect bar, play, time, track, mute;
};
MediaControls LayoutMediaControls(const Rect& content, bool audio);
std::string FormatMediaTime(double seconds);
// A click at (x, y) inside the content box of a media element with
// controls. Returns true if it hit a control (play/pause, seek, mute).
bool MediaControlsClick(Node* el, const Rect& content, float x, float y);

bool MediaSupported();  // built with the decoders
// HTMLMediaElement.canPlayType(): "", "maybe" or "probably".
std::string MediaCanPlayType(const std::string& type);

// Pixel conversion helper (exposed for tests): one 8-bit 4:2:0 frame.
void YuvToBgra(const uint8_t* const planes[3], const int strides[3], int width, int height,
               int chromaShiftX, int chromaShiftY, bool bt709, bool fullRange, uint32_t* out);

}  // namespace kite

#endif
