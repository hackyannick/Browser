// Audio output for <audio>/<video> through the waveOut API (winmm), which
// Windows 2000 has. Used from the media decoder thread only.
#include <windows.h>
#include <mmsystem.h>

#include <deque>
#include <vector>

#include "media/media.h"

namespace kite {

namespace {

class WaveOutSink : public AudioSink {
 public:
  ~WaveOutSink() override {
    if (!wave_) return;
    waveOutReset(wave_);
    Reap(true);
    waveOutClose(wave_);
  }

  bool Open(int rate) override {
    WAVEFORMATEX f;
    ZeroMemory(&f, sizeof f);
    f.wFormatTag = WAVE_FORMAT_PCM;
    f.nChannels = 2;
    f.nSamplesPerSec = rate;
    f.wBitsPerSample = 16;
    f.nBlockAlign = 4;
    f.nAvgBytesPerSec = rate * 4;
    if (waveOutOpen(&wave_, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
      wave_ = 0;
      return false;
    }
    waveOutPause(wave_);
    paused_ = true;
    return true;
  }

  void Write(const int16_t* samples, int frames) override {
    if (!wave_ || frames <= 0) return;
    Reap(false);
    Block* b = new Block;
    b->data.assign(samples, samples + frames * 2);
    ZeroMemory(&b->hdr, sizeof b->hdr);
    b->hdr.lpData = (LPSTR)&b->data[0];
    b->hdr.dwBufferLength = (DWORD)(b->data.size() * sizeof(int16_t));
    if (waveOutPrepareHeader(wave_, &b->hdr, sizeof b->hdr) != MMSYSERR_NOERROR) {
      delete b;
      return;
    }
    if (waveOutWrite(wave_, &b->hdr, sizeof b->hdr) != MMSYSERR_NOERROR) {
      waveOutUnprepareHeader(wave_, &b->hdr, sizeof b->hdr);
      delete b;
      return;
    }
    blocks_.push_back(b);
    written_ += frames;
  }

  long long Played() override {
    if (!wave_) return 0;
    MMTIME t;
    ZeroMemory(&t, sizeof t);
    t.wType = TIME_SAMPLES;
    if (waveOutGetPosition(wave_, &t, sizeof t) != MMSYSERR_NOERROR) return 0;
    bool bytes = t.wType == TIME_BYTES;
    if (!bytes && t.wType != TIME_SAMPLES) return 0;
    DWORD raw = bytes ? t.u.cb : t.u.sample;
    // The 32-bit counter wraps (after a day at 48 kHz, sooner in bytes).
    if (raw < lastRaw_) ++wraps_;
    lastRaw_ = raw;
    unsigned long long pos = wraps_ * 0x100000000ULL + raw;
    long long p = (long long)(bytes ? pos / 4 : pos);
    if (p > written_) p = written_;
    return p;
  }

  void SetPaused(bool paused) override {
    if (!wave_ || paused == paused_) return;
    paused_ = paused;
    if (paused) waveOutPause(wave_);
    else waveOutRestart(wave_);
  }

  void Flush() override {
    if (!wave_) return;
    waveOutReset(wave_);  // marks every block done and rewinds the position
    Reap(true);
    if (paused_) waveOutPause(wave_);
    written_ = 0;
    lastRaw_ = 0;
    wraps_ = 0;
  }

 private:
  struct Block {
    WAVEHDR hdr;
    std::vector<int16_t> data;
  };

  void Reap(bool all) {
    while (!blocks_.empty()) {
      Block* b = blocks_.front();
      if (!all && !(b->hdr.dwFlags & WHDR_DONE)) break;
      if (all && !(b->hdr.dwFlags & WHDR_DONE)) {
        // waveOutReset finished it; wait for the flag briefly.
        for (int i = 0; i < 50 && !(b->hdr.dwFlags & WHDR_DONE); ++i) Sleep(1);
      }
      waveOutUnprepareHeader(wave_, &b->hdr, sizeof b->hdr);
      delete b;
      blocks_.pop_front();
    }
  }

  HWAVEOUT wave_ = 0;
  std::deque<Block*> blocks_;
  long long written_ = 0;
  DWORD lastRaw_ = 0;
  unsigned long long wraps_ = 0;
  bool paused_ = true;
};

AudioSink* CreateWaveOutSink() { return new WaveOutSink; }

}  // namespace

void InstallAudioOutput() { SetAudioSinkFactory(CreateWaveOutSink); }

}  // namespace kite
