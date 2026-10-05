// Kite Engine - Page: ties together DOM, styles, layout and painting for one
// document. The platform layer feeds it network data and draws its display
// list.
#ifndef KITE_PAGE_PAGE_H
#define KITE_PAGE_PAGE_H

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "css/resolver.h"
#include "dom/node.h"
#include "layout/box.h"
#include "layout/layout.h"
#include "net/url.h"
#include "page/animation.h"
#include "paint/display_list.h"
#include "script/script.h"

namespace kite {

struct WebFontRequest {
  std::string url;
  std::string family;  // CSS family name (lower-case)
  int weight;
  bool italic;
};

struct FormSubmission {
  std::string method;  // "GET" or "POST"
  std::string url;
  std::string body;
  std::string contentType;
};

class Page {
 public:
  Page(FontProvider* fonts, ImageProvider* images);
  ~Page();

  // Loads a document. |url| is the final URL (after redirects).
  void LoadHtml(const std::string& utf8, const std::string& url);
  void LoadPlainText(const std::string& utf8, const std::string& url);
  void LoadImageDocument(const std::string& url);

  const std::string& url() const { return url_; }
  // Same-document URL change (pushState, fragment navigation).
  void SetUrl(const std::string& url);
  FontProvider* fonts() { return fonts_; }
  ImageProvider* images() { return images_; }
  std::string Title() const;
  Document* document() { return doc_.get(); }

  // Stylesheets referenced via <link> / @import that still need fetching.
  std::vector<std::string> PendingStylesheets() const;
  void ProvideStylesheet(const std::string& url, const std::string& css, bool ok);
  bool HasPendingStylesheets() const { return !PendingStylesheets().empty(); }

  // Web fonts used by the document that still need loading (each is
  // returned once). Call after Restyle().
  std::vector<WebFontRequest> PendingFonts();
  void FontsChanged() { engine_.ClearFontCaches(); }

  // Recomputes styles (needed after stylesheets arrive or the viewport
  // width changes media query results).
  void Restyle(float viewportW, float viewportH);
  // Rebuilds the box tree and lays out + paints. Call after Restyle or when
  // images finish loading.
  void Relayout(float viewportW, float viewportH);
  // Re-runs only the painter (e.g. after a form control value changed).
  void Repaint();

  const DisplayList& display() const { return display_; }
  float ContentHeight() const { return display_.height; }
  float ContentWidth() const { return display_.width; }

  // Image URLs referenced by the current layout (img, backgrounds).
  std::vector<std::string> ReferencedImages() const;

  // Hit testing in document coordinates.
  // (x, y) in document coordinates; scroll offset in CSS px is needed for
  // position:fixed content.
  Node* HitTest(float x, float y, float scrollX = 0, float scrollY = 0) const;
  static Node* LinkFor(Node* n);
  std::string LinkUrl(Node* link) const;  // resolved href
  Url ResolveUrl(const std::string& ref) const;
  // Y coordinate of #fragment target, or -1.
  float AnchorPosition(const std::string& fragment) const;
  bool BoxRect(Node* n, Rect& out) const;

  // <meta http-equiv=refresh>
  int refreshDelay() const { return refreshDelay_; }
  const std::string& refreshUrl() const { return refreshUrl_; }

  // Forms.
  static Node* FormFor(Node* control);
  bool BuildFormSubmission(Node* form, Node* submitter, FormSubmission& out) const;
  static Node* DefaultSubmitButton(Node* form);

  // JavaScript. Call SetScripting before LoadHtml; |host| must outlive the page.
  void SetScripting(ScriptHost* host, bool enabled) {
    scriptHost_ = host;
    scriptingEnabled_ = enabled;
  }
  bool scriptingEnabled() const { return scriptingEnabled_ && scriptHost_; }
  ScriptEngine* script() { return script_.get(); }
  // External scripts that still need fetching (each URL returned once).
  std::vector<std::string> PendingScripts();
  void ProvideScript(const std::string& url, const std::string& source, bool ok);
  // Runs every script that is ready, in document order; fires
  // DOMContentLoaded/load once all parser-inserted scripts ran. Returns true
  // if anything executed.
  bool RunScripts();
  bool scriptsFinished() const { return loadFired_; }
  // Called by the bindings.
  void OnScriptInserted(Node* script);
  void RequestRepaint();
  void EnsureLayout();
  float viewportWidth() const { return lastViewportW_; }
  float viewportHeight() const { return lastViewportH_; }
  void QueueFormSubmission(const FormSubmission& s) { formQueue_.push_back(s); }
  bool TakeFormSubmission(FormSubmission& out);
  // After scripts changed the DOM: re-collect stylesheets, restyle, relayout.
  void ScriptMutated();

  // CSS animations and transitions. Tick returns what needs updating:
  // 0 nothing, 1 repaint (call Repaint), 2 relayout (call Relayout).
  bool AnimationsActive() const { return anim_.Active(); }
  int TickAnimations();
  // Headless use: show the end state of animations immediately.
  void SetAnimationsInstant(bool on) { anim_.SetInstant(on); }

  // Plain text of the whole document (for "find in page").
  std::vector<std::pair<Rect, std::string> > TextRuns() const;

 private:
  struct SheetEntry {
    bool isLink;
    std::string url;     // for links / imports
    std::string media;   // media attribute
    std::string css;
    bool loaded;
    bool failed;
    std::shared_ptr<Stylesheet> sheet;
    bool imported = false;  // via @import from a fetched sheet
  };
  void CollectDocumentInfo();
  void AddSheetsFromNode(Node* n);
  void CollectScripts(Node* n);
  void RecollectSheets();
  struct ScriptEntry {
    Node* node;
    std::string url;  // empty for inline scripts
    std::string source;
    bool parserInserted;
    bool loaded, failed, requested, done;
    int kind = 1;  // 1 classic script, 2 module, 3 import map
    std::string moduleName;  // modules: URL (inline: document URL + #module-N)
    bool registered = false;
  };
  bool MakeScriptEntry(Node* n, bool parserInserted, ScriptEntry& e);
  int inlineModules_ = 0;

  FontProvider* fonts_;
  ImageProvider* images_;
  std::string url_;
  Url baseUrl_;
  std::unique_ptr<Document> doc_;
  std::vector<SheetEntry> sheets_;
  StyleResolver resolver_;
  std::unique_ptr<LayoutBox> root_;
  LayoutEngine engine_;
  DisplayList display_;
  float lastViewportW_ = 800, lastViewportH_ = 600;
  std::set<std::string> requestedFonts_;
  int refreshDelay_;
  std::string refreshUrl_;
  ScriptHost* scriptHost_ = 0;
  bool scriptingEnabled_ = false;
  std::vector<ScriptEntry> scripts_;
  std::vector<FormSubmission> formQueue_;
  bool contentLoadedFired_ = false, loadFired_ = false;
  bool inScripts_ = false;
  AnimationController anim_;
  // Destroyed first: holds pointers into doc_.
  std::unique_ptr<ScriptEngine> script_;
};

}  // namespace kite

#endif
