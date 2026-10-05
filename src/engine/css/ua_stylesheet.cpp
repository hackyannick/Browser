#include "css/resolver.h"

namespace kite {

// Default user agent stylesheet (based on the HTML spec's rendering section).
const char* StyleResolver::UserAgentCss() {
  return R"CSS(
html, address, blockquote, body, center, dialog, div, figure, figcaption,
footer, form, header, hr, legend, listing, main, p, plaintext, pre, search,
xmp, article, aside, h1, h2, h3, h4, h5, h6, hgroup, nav, section, details,
summary, fieldset, dl, dd, dt, ol, ul, menu, dir, optgroup, frameset, frame { display: block; }
head, link, meta, script, style, title, base, template, noembed, noframes,
area, datalist, param, rp, [hidden], dialog:not([open]), input[type=hidden],
embed[hidden], audio:not([controls]), track, source { display: none; }
html { color: #000; font-family: serif; font-size: 16px; line-height: normal; }
body { margin: 8px; }
p, blockquote, figure, listing, plaintext, pre, xmp, dl { margin-top: 1em; margin-bottom: 1em; }
blockquote, figure { margin-left: 40px; margin-right: 40px; }
address { font-style: italic; }
center { text-align: center; }
h1 { font-size: 2em; margin: 0.67em 0; font-weight: bold; }
h2 { font-size: 1.5em; margin: 0.83em 0; font-weight: bold; }
h3 { font-size: 1.17em; margin: 1em 0; font-weight: bold; }
h4 { font-size: 1em; margin: 1.33em 0; font-weight: bold; }
h5 { font-size: 0.83em; margin: 1.67em 0; font-weight: bold; }
h6 { font-size: 0.67em; margin: 2.33em 0; font-weight: bold; }
article h1, aside h1, nav h1, section h1 { font-size: 1.5em; margin: 0.83em 0; }
dd { margin-left: 40px; }
ol, ul, menu, dir { margin-top: 1em; margin-bottom: 1em; padding-left: 40px; }
ol ol, ol ul, ul ol, ul ul, menu ul, ul menu { margin-top: 0; margin-bottom: 0; }
ul, menu, dir { list-style-type: disc; }
ol { list-style-type: decimal; }
ul ul, ol ul { list-style-type: circle; }
ul ul ul, ol ul ul, ul ol ul, ol ol ul { list-style-type: square; }
ol[type="a"] { list-style-type: lower-alpha; }
ol[type="A"] { list-style-type: upper-alpha; }
ol[type="i"] { list-style-type: lower-roman; }
ol[type="I"] { list-style-type: upper-roman; }
ul[type="circle" i] { list-style-type: circle; }
ul[type="square" i] { list-style-type: square; }
ul[type="none" i] { list-style-type: none; }
li { display: list-item; }
pre, listing, plaintext, xmp, code, kbd, samp, tt { font-family: monospace; }
pre, listing, plaintext, xmp { white-space: pre; }
textarea { white-space: pre-wrap; }
code, kbd, samp, tt { font-size: 0.8125em; }
pre { font-size: 0.8125em; }
b, strong { font-weight: bold; }
i, cite, em, var, dfn { font-style: italic; }
u, ins { text-decoration: underline; }
s, strike, del { text-decoration: line-through; }
big { font-size: larger; }
small { font-size: smaller; }
sub { vertical-align: sub; font-size: smaller; }
sup { vertical-align: super; font-size: smaller; }
mark { background-color: yellow; color: black; }
a:any-link { color: #0000ee; text-decoration: underline; cursor: pointer; }
abbr[title], acronym[title] { text-decoration: underline; }
q::before { content: "\201C"; }
q::after { content: "\201D"; }
nobr { white-space: nowrap; }
wbr { display: inline; }
hr { color: gray; border-style: inset; border-width: 1px; margin: 0.5em auto; }
fieldset { margin-left: 2px; margin-right: 2px; border: 2px groove #c0c0c0; padding: 0.35em 0.75em 0.625em; }
legend { padding-left: 2px; padding-right: 2px; }
table { display: table; border-spacing: 2px; border-collapse: separate; box-sizing: border-box; text-indent: 0; }
caption { display: table-caption; text-align: center; }
colgroup { display: table-column-group; }
col { display: table-column; }
thead { display: table-header-group; vertical-align: middle; }
tbody { display: table-row-group; vertical-align: middle; }
tfoot { display: table-footer-group; vertical-align: middle; }
tr { display: table-row; vertical-align: inherit; }
td, th { display: table-cell; vertical-align: inherit; padding: 1px; }
th { font-weight: bold; text-align: center; }
td { text-align: inherit; }
img { display: inline; }
iframe { border: 2px inset #c0c0c0; width: 300px; height: 150px; }
video, canvas, object, embed { width: 300px; height: 150px; }
svg:not([width]) { width: 24px; }
svg:not([height]) { height: 24px; }
input, select, textarea, button { display: inline-block; font-family: sans-serif; font-size: 13.33px; color: black; letter-spacing: normal; word-spacing: normal; line-height: normal; text-transform: none; text-indent: 0; text-align: start; vertical-align: baseline; }
input, textarea { background-color: white; border: 2px inset #d4d0c8; padding: 1px 2px; cursor: text; }
input { width: 150px; }
textarea { width: 220px; height: 3.2em; font-family: monospace; }
select { background-color: white; border: 2px inset #d4d0c8; padding: 1px 2px; }
button, input[type=submit], input[type=button], input[type=reset] { background-color: #d4d0c8; border: 2px outset #d4d0c8; padding: 1px 6px; text-align: center; cursor: default; width: auto; }
input[type=checkbox], input[type=radio] { width: 13px; height: 13px; padding: 0; margin: 3px 3px 3px 4px; background-color: white; }
input[type=image] { border: none; padding: 0; width: auto; }
input[type=file] { width: 220px; }
input[type=range], input[type=color] { width: 120px; }
details > summary:first-of-type { display: list-item; list-style-type: disc; list-style-position: inside; cursor: pointer; }
details:not([open]) > :not(summary) { display: none; }
ruby { display: ruby; }
rt { font-size: 50%; }
marquee { display: inline-block; }
noscript { display: inline; }
)CSS";
}

}  // namespace kite
