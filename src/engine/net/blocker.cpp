#include "net/blocker.h"

#include "base/strings.h"
#include "net/url.h"

namespace kite {

namespace {

// Advertising, tracking and analytics hosts (and all their subdomains).
const char* const kBuiltinHosts[] = {
    // Google ads and analytics
    "doubleclick.net", "googlesyndication.com", "googleadservices.com", "google-analytics.com",
    "googletagmanager.com", "googletagservices.com", "adservice.google.com", "adservice.google.de",
    "pagead2.googlesyndication.com", "imasdk.googleapis.com", "2mdn.net", "admob.com",
    "app-measurement.com", "googleoptimize.com",
    // Microsoft, Amazon, Meta, X, LinkedIn, TikTok, Snap, Pinterest, Reddit
    "ads.microsoft.com", "bat.bing.com", "clarity.ms", "amazon-adsystem.com", "assoc-amazon.com",
    "connect.facebook.net", "facebook.com/tr", "ads.linkedin.com", "px.ads.linkedin.com", "snap.licdn.com",
    "analytics.twitter.com", "static.ads-twitter.com", "ads-api.twitter.com", "analytics.tiktok.com",
    "ads.tiktok.com", "sc-static.net", "tr.snapchat.com", "ct.pinterest.com", "s.pinimg.com",
    "events.redditmedia.com", "alb.reddit.com",
    // Ad exchanges and networks
    "adnxs.com", "adsrvr.org", "rubiconproject.com", "pubmatic.com", "openx.net", "openx.com",
    "casalemedia.com", "indexww.com", "criteo.com", "criteo.net", "taboola.com", "outbrain.com",
    "outbrainimg.com", "smartadserver.com", "adform.net", "adformdsp.net", "yieldlab.net", "yieldlove.com",
    "yieldlove-ad-serving.net", "stroeerdigitalgroup.de", "stroeerdigitalmedia.de", "adition.com",
    "adtech.de", "advertising.com", "adtechus.com", "teads.tv", "teads.com", "sharethrough.com",
    "triplelift.com", "3lift.com", "media.net", "contextweb.com", "spotxchange.com", "spotx.tv",
    "springserve.com", "lijit.com", "sovrn.com", "33across.com", "gumgum.com", "districtm.io",
    "bidswitch.net", "bidr.io", "adroll.com", "quantserve.com", "quantcount.com", "quantcast.com",
    "mathtag.com", "turn.com", "demdex.net", "everesttech.net", "omtrdc.net", "2o7.net",
    "agkn.com", "rlcdn.com", "tapad.com", "adsymptotic.com", "moatads.com", "doubleverify.com",
    "adsafeprotected.com", "serving-sys.com", "flashtalking.com", "innovid.com", "zemanta.com",
    "revcontent.com", "mgid.com", "adblade.com", "content.ad", "plista.com", "ligatus.com",
    "nativendo.de", "seedtag.com", "ogury.io", "ogury.co", "smaato.net", "inmobi.com", "applovin.com",
    "unityads.unity3d.com", "chartboost.com", "vungle.com", "ironsrc.com", "mopub.com", "adcolony.com",
    "propellerads.com", "popads.net", "popcash.net", "exoclick.com", "juicyads.com", "trafficjunky.net",
    "adsterra.com", "hilltopads.net", "clickadu.com", "admaven.com", "onclickads.net", "zedo.com",
    "undertone.com", "conversantmedia.com", "dotomi.com", "emxdgt.com", "lkqd.net", "aniview.com",
    "connatix.com", "primis.tech", "vidazoo.com", "adpushup.com", "ezoic.net", "ezojs.com",
    "media6degrees.com", "sitescout.com", "simpli.fi", "steelhousemedia.com", "adkernel.com",
    "betweendigital.com", "adhese.com", "improvedigital.com", "360yield.com", "admanmedia.com",
    "yahoo.com/ads", "ads.yahoo.com", "advertising.yahoo.com", "gemini.yahoo.com", "adtech.yahooinc.com",
    // Analytics and session recording
    "hotjar.com", "hotjar.io", "mouseflow.com", "fullstory.com", "crazyegg.com", "luckyorange.com",
    "inspectlet.com", "smartlook.com", "logrocket.io", "mixpanel.com", "segment.io", "segment.com",
    "amplitude.com", "heap.io", "heapanalytics.com", "kissmetrics.com", "chartbeat.com", "chartbeat.net",
    "parsely.com", "newrelic.com", "nr-data.net", "scorecardresearch.com", "comscore.com",
    "statcounter.com", "histats.com", "clicky.com", "getclicky.com", "woopra.com", "matomo.cloud",
    "etracker.com", "etracker.de", "webtrekk.net", "wt-safetag.com", "mapp.com", "econda-monitor.de",
    "ioam.de", "iocnt.net", "infonline.de", "xiti.com", "atinternet.com", "at-o.net", "gemius.pl",
    "sentry-cdn.com", "bugsnag.com", "branch.io", "app.link", "adjust.com", "appsflyer.com",
    "kochava.com", "singular.net", "optimizely.com", "vwo.com", "visualwebsiteoptimizer.com",
    "abtasty.com", "kameleoon.eu", "dynamicyield.com", "bluekai.com", "krxd.net", "exelator.com",
    "eyeota.net", "lotame.com", "crwdcntrl.net", "addthis.com", "sharethis.com", "addtoany.com",
    "onesignal.com", "pushcrew.com", "pushengage.com", "cleverpush.com", "subscribers.com",
    "trustpilot.com/tracking", "zopim.com", "intercomcdn.com/tracking", "licdn.com/li.lms-analytics",
    "yandex.ru/metrika", "mc.yandex.ru", "an.yandex.ru", "top-fwz1.mail.ru", "counter.yadro.ru",
    "tns-counter.ru", "hit.gemius.pl", "cdn.mxpnl.com", "ads.pubmatic.com", "securepubads.g.doubleclick.net",
    // Consent and anti-adblock scripts that only add weight
    "adblockanalytics.com", "blockadblock.com", "admiral.com", "getadmiral.com", "pagefair.com",
    "pagefair.net", "sourcepoint.com", "uabp.net",
};

std::string HostOf(const std::string& url) {
  Url u = Url::Parse(url);
  return u.valid() ? AsciiLower(u.host()) : std::string();
}

}  // namespace

ContentBlocker& ContentBlocker::Get() {
  static ContentBlocker* b = new ContentBlocker;
  return *b;
}

ContentBlocker::ContentBlocker() {
  for (size_t i = 0; i < sizeof kBuiltinHosts / sizeof kBuiltinHosts[0]; ++i) {
    std::string h = kBuiltinHosts[i];
    if (h.find('/') != std::string::npos) continue;  // path rules are not supported
    blocked_.insert(h);
  }
}

void ContentBlocker::SetEnabled(bool on) {
  MutexLock l(mu_);
  enabled_ = on;
}

bool ContentBlocker::enabled() {
  MutexLock l(mu_);
  return enabled_;
}

void ContentBlocker::AddRules(const std::string& text) {
  MutexLock l(mu_);
  std::vector<std::string> lines = Split(text, '\n');
  for (size_t i = 0; i < lines.size(); ++i) {
    std::string r = Trim(lines[i]);
    if (r.empty() || r[0] == '!' || r[0] == '#' || r[0] == '[') continue;
    bool allow = StartsWith(r, "@@");
    if (allow) r = r.substr(2);
    // Hosts-file lines: "0.0.0.0 ads.example.com".
    std::vector<std::string> parts = SplitWhitespace(r);
    if (parts.size() == 2 && (parts[0] == "0.0.0.0" || parts[0] == "127.0.0.1")) r = parts[1];
    if (StartsWith(r, "||")) r = r.substr(2);
    size_t end = r.find_first_of("^/$*|");
    if (end != std::string::npos) {
      // Only whole-host rules ("||host^" with nothing but options after it).
      if (r[end] != '^' || (end + 1 < r.size() && r[end + 1] != '$')) continue;
      r = r.substr(0, end);
    }
    r = AsciiLower(r);
    if (r.empty() || r.find('.') == std::string::npos || r.find_first_of(" \t*") != std::string::npos) continue;
    (allow ? allowed_ : blocked_).insert(r);
  }
}

bool ContentBlocker::Listed(const std::set<std::string>& list, const std::string& host) const {
  // The host itself or any parent domain.
  for (size_t pos = 0; pos != std::string::npos;) {
    if (list.count(host.substr(pos))) return true;
    pos = host.find('.', pos);
    if (pos != std::string::npos) ++pos;
  }
  return false;
}

std::string ContentBlocker::SiteOf(const std::string& hostIn) {
  std::string host = AsciiLower(hostIn);
  std::vector<std::string> labels = Split(host, '.');
  if (labels.size() <= 2) return host;
  // Numeric addresses stay whole.
  bool numeric = true;
  for (size_t i = 0; i < host.size(); ++i)
    if (!(host[i] == '.' || (host[i] >= '0' && host[i] <= '9'))) numeric = false;
  if (numeric) return host;
  size_t n = labels.size();
  const std::string& sld = labels[n - 2];
  // Second-level public suffixes such as co.uk, com.au, gv.at.
  static const char* const kSecond[] = {"co", "com", "net", "org", "gov", "edu", "ac", "or", "ne", "go", "gv", "ltd", "plc", 0};
  bool second = false;
  for (int i = 0; kSecond[i]; ++i)
    if (sld == kSecond[i]) second = true;
  size_t keep = second && labels[n - 1].size() == 2 ? 3 : 2;
  std::string out;
  for (size_t i = n - keep; i < n; ++i) out += (out.empty() ? "" : ".") + labels[i];
  return out;
}

bool ContentBlocker::ShouldBlock(const std::string& url, const std::string& pageUrl) {
  if (!StartsWith(url, "http:") && !StartsWith(url, "https:") && !StartsWith(url, "ws:") &&
      !StartsWith(url, "wss:"))
    return false;
  std::string host = HostOf(url);
  if (host.empty()) return false;
  std::string page = HostOf(pageUrl);
  if (!page.empty() && SiteOf(page) == SiteOf(host)) return false;  // first-party
  MutexLock l(mu_);
  if (!enabled_ || Listed(allowed_, host) || !Listed(blocked_, host)) return false;
  ++count_;
  return true;
}

long long ContentBlocker::blockedTotal() {
  MutexLock l(mu_);
  return count_;
}

}  // namespace kite
