// Kite Engine - CSS animations (@keyframes) and transitions.
//
// The cascade computes a "base" style for every element; the controller
// remembers it and, on every tick, rewrites the animated properties of
// Node::style with interpolated values.
#ifndef KITE_PAGE_ANIMATION_H
#define KITE_PAGE_ANIMATION_H

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "css/resolver.h"
#include "css/style.h"
#include "dom/node.h"

namespace kite {

// Easing function (cubic-bezier, steps or linear).
struct TimingFunction {
  enum Kind { kCubic, kSteps, kLinear };
  Kind kind = kCubic;
  float x1 = 0.25f, y1 = 0.1f, x2 = 0.25f, y2 = 1;  // "ease"
  int steps = 1;
  int stepPosition = 1;  // 0 jump-start, 1 jump-end, 2 jump-none, 3 jump-both
  static TimingFunction Parse(const std::string& css);
  float Apply(float t) const;
};

// Milliseconds from an arbitrary epoch (monotonic).
double AnimationClockMs();
// Tests: replaces the clock (null restores the real one).
void SetAnimationClockForTesting(double (*clock)());

class AnimationController {
 public:
  enum TickResult { kNothing = 0, kRepaint = 1, kRelayout = 2 };
  struct Context {
    float rootFontSize = 16;
    float viewportW = 800, viewportH = 600;
    std::string baseUrl;
  };

  AnimationController();
  ~AnimationController();

  void Clear();
  // Remembers the values shown right now (transition start values).
  void BeforeRestyle(Document* doc);
  // Picks up the freshly computed styles, starts transitions and
  // animations and applies the values for |now|.
  void AfterRestyle(Document* doc, const StyleResolver& resolver, const Context& ctx, double now);
  // Applies the values for |now|.
  TickResult Tick(double now);
  // True while something changes over time.
  bool Active() const;
  // With instant mode every finite animation/transition shows its end state
  // right away (headless tools without a frame clock).
  void SetInstant(bool on) { instant_ = on; }
  // animationstart / animationiteration / animationend / transitionend.
  std::vector<std::pair<Node*, std::string> > TakeEvents();

 private:
  struct Animation;
  struct Transition;
  struct Entry;
  TickResult Apply(double now);
  std::map<Node*, std::unique_ptr<Entry> > entries_;
  std::map<Node*, std::unique_ptr<ComputedStyle> > before_;
  std::vector<std::pair<Node*, std::string> > events_;
  bool instant_ = false;
};

}  // namespace kite

#endif
