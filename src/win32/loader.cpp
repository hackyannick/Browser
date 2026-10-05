// Background loading: every request runs on its own worker thread (created
// with _beginthreadex, available on Windows 2000) and reports back to the UI
// thread with PostMessage.
#include <process.h>

#include <deque>

#include "base/strings.h"
#include "image/svg.h"
#include "win32/app.h"

namespace kite {

namespace {

std::deque<FetchJob*> g_queue;
int g_active = 0;
const int kMaxActive = 6;

unsigned __stdcall Worker(void* arg) {
  FetchJob* job = static_cast<FetchJob*>(arg);
  job->response = Network::Get().Fetch(job->request);
  if (job->kind == FetchJob::kImage && job->response.ok && job->response.status < 400 &&
      !(job->request.cancel && job->request.cancel->cancelled())) {
    job->imageOk = DecodeImage(job->response.body, job->image);
    if (!job->imageOk && LooksLikeSvg(job->response.body)) {
      job->imageOk = RenderSvgDocument(job->response.body, 0, 0, 2.0f, job->image);
      job->image.density = 2.0f;
    }
    std::string().swap(job->response.body);  // free the compressed data early
  }
  if (!PostMessageW(job->notify, WM_KITE_FETCHED, 0, (LPARAM)job)) delete job;
  return 0;
}

void Launch(FetchJob* job) {
  ++g_active;
  unsigned id;
  HANDLE h = (HANDLE)_beginthreadex(0, 512 * 1024, Worker, job, 0, &id);
  if (h) {
    CloseHandle(h);
  } else {
    // Could not create a thread: report failure synchronously.
    job->response.error = "Thread konnte nicht gestartet werden";
    PostMessageW(job->notify, WM_KITE_FETCHED, 0, (LPARAM)job);
  }
}

}  // namespace

void StartFetch(FetchJob* job) {
  if (g_active < kMaxActive || job->kind == FetchJob::kDocument || job->kind == FetchJob::kDownload) {
    Launch(job);
    return;
  }
  if (job->kind == FetchJob::kStylesheet) g_queue.push_front(job);
  else g_queue.push_back(job);
}

void FetchFinished() {
  if (g_active > 0) --g_active;
  while (g_active < kMaxActive && !g_queue.empty()) {
    FetchJob* next = g_queue.front();
    g_queue.pop_front();
    if (next->request.cancel && next->request.cancel->cancelled()) {
      next->response.error = "Abgebrochen";
      PostMessageW(next->notify, WM_KITE_FETCHED, 0, (LPARAM)next);
      ++g_active;  // balanced by the FetchFinished() of this message
      continue;
    }
    Launch(next);
  }
}

}  // namespace kite
