// AVIF decoding: a small HEIF (ISO/IEC 23008-12) container parser plus the
// dav1d AV1 decoder. Supports still images with alpha, grids, rotation and
// mirroring, 8/10/12 bit and all chroma layouts.
#include "image/avif.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>

#include <dav1d/dav1d.h>

namespace kite {

namespace {

uint32_t Be(const uint8_t* p, int n) {
  uint32_t v = 0;
  for (int i = 0; i < n; ++i) v = (v << 8) | p[i];
  return v;
}

struct Box {
  uint32_t type;
  const uint8_t* data;  // payload
  size_t size;
};

uint32_t Tag(const char* t) { return Be((const uint8_t*)t, 4); }

// Splits [p, p + n) into boxes.
bool ReadBoxes(const uint8_t* p, size_t n, std::vector<Box>& out) {
  size_t pos = 0;
  while (pos + 8 <= n) {
    uint64_t size = Be(p + pos, 4);
    uint32_t type = Be(p + pos + 4, 4);
    size_t header = 8;
    if (size == 1) {
      if (pos + 16 > n) return false;
      size = ((uint64_t)Be(p + pos + 8, 4) << 32) | Be(p + pos + 12, 4);
      header = 16;
    } else if (size == 0) {
      size = n - pos;
    }
    if (size < header || size > n - pos) return false;
    Box b;
    b.type = type;
    b.data = p + pos + header;
    b.size = (size_t)size - header;
    out.push_back(b);
    pos += (size_t)size;
  }
  return true;
}

const Box* Find(const std::vector<Box>& boxes, const char* type) {
  uint32_t t = Tag(type);
  for (size_t i = 0; i < boxes.size(); ++i)
    if (boxes[i].type == t) return &boxes[i];
  return 0;
}

class Reader {
 public:
  Reader(const uint8_t* p, size_t n) : p_(p), n_(n), pos_(0) {}
  bool Read(int bytes, uint32_t& v) {
    if (bytes == 0) {
      v = 0;
      return true;
    }
    if (bytes > 4) {  // 8-byte fields: keep the low 32 bits (files < 4 GB)
      if (pos_ + bytes > n_) return false;
      v = Be(p_ + pos_ + bytes - 4, 4);
      pos_ += bytes;
      return true;
    }
    if (pos_ + bytes > n_) return false;
    v = Be(p_ + pos_, bytes);
    pos_ += bytes;
    return true;
  }
  bool Skip(size_t n) {
    if (pos_ + n > n_) return false;
    pos_ += n;
    return true;
  }
  bool CString(std::string& s) {
    s.clear();
    while (pos_ < n_ && p_[pos_]) s += (char)p_[pos_++];
    if (pos_ >= n_) return false;
    ++pos_;
    return true;
  }
  size_t pos() const { return pos_; }

 private:
  const uint8_t* p_;
  size_t n_, pos_;
};

struct Extent {
  uint32_t offset, length;
};

struct Item {
  uint32_t type = 0;
  int construction = 0;  // 0 file offset, 1 idat
  uint32_t baseOffset = 0;
  std::vector<Extent> extents;
  std::vector<int> props;  // indices into the property list
  std::vector<uint32_t> dimg;  // grid tiles (item ids, in order)
  uint32_t auxlFor = 0;        // alpha plane of this item
};

struct Property {
  uint32_t type = 0;
  const uint8_t* data = 0;
  size_t size = 0;
};

struct Container {
  const uint8_t* file;
  size_t fileSize;
  const uint8_t* idat = 0;
  size_t idatSize = 0;
  uint32_t primary = 0;
  std::map<uint32_t, Item> items;
  std::vector<Property> props;

  bool ItemData(uint32_t id, std::vector<uint8_t>& out) const {
    std::map<uint32_t, Item>::const_iterator it = items.find(id);
    if (it == items.end()) return false;
    out.clear();
    const Item& item = it->second;
    for (size_t i = 0; i < item.extents.size(); ++i) {
      uint64_t off = (uint64_t)item.baseOffset + item.extents[i].offset;
      uint64_t len = item.extents[i].length;
      const uint8_t* base = item.construction == 1 ? idat : file;
      size_t size = item.construction == 1 ? idatSize : fileSize;
      if (!base || off > size) return false;
      if (len == 0) len = size - off;  // "to the end"
      if (len > size - off || out.size() + len > 64u * 1024 * 1024) return false;
      out.insert(out.end(), base + off, base + off + len);
    }
    return !out.empty();
  }

  const Property* Prop(uint32_t id, const char* type) const {
    std::map<uint32_t, Item>::const_iterator it = items.find(id);
    if (it == items.end()) return 0;
    uint32_t t = Tag(type);
    for (size_t i = 0; i < it->second.props.size(); ++i) {
      int k = it->second.props[i];
      if (k >= 0 && k < (int)props.size() && props[k].type == t) return &props[k];
    }
    return 0;
  }
};

bool ParseMeta(const Box& meta, Container& c) {
  if (meta.size < 4) return false;
  std::vector<Box> boxes;
  if (!ReadBoxes(meta.data + 4, meta.size - 4, boxes)) return false;  // FullBox
  if (const Box* pitm = Find(boxes, "pitm")) {
    if (pitm->size < 6) return false;
    int version = pitm->data[0];
    c.primary = version == 0 ? Be(pitm->data + 4, 2) : (pitm->size >= 8 ? Be(pitm->data + 4, 4) : 0);
  }
  if (const Box* idat = Find(boxes, "idat")) {
    c.idat = idat->data;
    c.idatSize = idat->size;
  }
  // Item types.
  if (const Box* iinf = Find(boxes, "iinf")) {
    if (iinf->size < 6) return false;
    int version = iinf->data[0];
    size_t hdr = version == 0 ? 6 : 8;
    std::vector<Box> entries;
    if (hdr > iinf->size || !ReadBoxes(iinf->data + hdr, iinf->size - hdr, entries)) return false;
    for (size_t i = 0; i < entries.size(); ++i) {
      const Box& e = entries[i];
      if (e.type != Tag("infe") || e.size < 4) continue;
      int v = e.data[0];
      if (v < 2) continue;
      Reader r(e.data + 4, e.size - 4);
      uint32_t id, protection, type;
      if (!r.Read(v == 2 ? 2 : 4, id) || !r.Read(2, protection) || !r.Read(4, type)) continue;
      c.items[id].type = type;
    }
  }
  // Locations.
  if (const Box* iloc = Find(boxes, "iloc")) {
    if (iloc->size < 6) return false;
    int version = iloc->data[0];
    Reader r(iloc->data + 4, iloc->size - 4);
    uint32_t a, b, count;
    if (!r.Read(1, a) || !r.Read(1, b)) return false;
    int offsetSize = a >> 4, lengthSize = a & 15, baseSize = b >> 4, indexSize = version >= 1 ? (b & 15) : 0;
    if (!r.Read(version < 2 ? 2 : 4, count)) return false;
    for (uint32_t i = 0; i < count && i < 10000; ++i) {
      uint32_t id, method = 0, dref, base, extents;
      if (!r.Read(version < 2 ? 2 : 4, id)) return false;
      if (version >= 1 && !r.Read(2, method)) return false;
      if (!r.Read(2, dref) || !r.Read(baseSize, base) || !r.Read(2, extents)) return false;
      Item& item = c.items[id];
      item.construction = method & 15;
      item.baseOffset = base;
      for (uint32_t k = 0; k < extents && k < 1000; ++k) {
        uint32_t idx, off, len;
        if (!r.Read(indexSize, idx) || !r.Read(offsetSize, off) || !r.Read(lengthSize, len)) return false;
        Extent ex = {off, len};
        item.extents.push_back(ex);
      }
    }
  }
  // Properties.
  if (const Box* iprp = Find(boxes, "iprp")) {
    std::vector<Box> sub;
    if (!ReadBoxes(iprp->data, iprp->size, sub)) return false;
    if (const Box* ipco = Find(sub, "ipco")) {
      std::vector<Box> list;
      if (!ReadBoxes(ipco->data, ipco->size, list)) return false;
      for (size_t i = 0; i < list.size(); ++i) {
        Property p;
        p.type = list[i].type;
        p.data = list[i].data;
        p.size = list[i].size;
        c.props.push_back(p);
      }
    }
    for (size_t s = 0; s < sub.size(); ++s) {
      if (sub[s].type != Tag("ipma") || sub[s].size < 8) continue;
      int version = sub[s].data[0], flags = sub[s].data[3];
      Reader r(sub[s].data + 4, sub[s].size - 4);
      uint32_t count;
      if (!r.Read(4, count)) return false;
      for (uint32_t i = 0; i < count && i < 10000; ++i) {
        uint32_t id, n;
        if (!r.Read(version < 1 ? 2 : 4, id) || !r.Read(1, n)) return false;
        for (uint32_t k = 0; k < n; ++k) {
          uint32_t v;
          if (!r.Read(flags & 1 ? 2 : 1, v)) return false;
          uint32_t index = v & (flags & 1 ? 0x7FFF : 0x7F);
          if (index > 0) c.items[id].props.push_back((int)index - 1);
        }
      }
    }
  }
  // References (alpha planes, grid tiles).
  if (const Box* iref = Find(boxes, "iref")) {
    if (iref->size < 4) return false;
    int version = iref->data[0];
    std::vector<Box> refs;
    if (!ReadBoxes(iref->data + 4, iref->size - 4, refs)) return false;
    for (size_t i = 0; i < refs.size(); ++i) {
      Reader r(refs[i].data, refs[i].size);
      uint32_t from, n;
      int idSize = version == 0 ? 2 : 4;
      if (!r.Read(idSize, from) || !r.Read(2, n)) continue;
      for (uint32_t k = 0; k < n && k < 4096; ++k) {
        uint32_t to;
        if (!r.Read(idSize, to)) break;
        if (refs[i].type == Tag("dimg")) c.items[from].dimg.push_back(to);
        else if (refs[i].type == Tag("auxl")) c.items[from].auxlFor = to;
      }
    }
  }
  return true;
}

// Decoded AV1 frame.
struct Frame {
  int w = 0, h = 0, bpc = 8, layout = 0;
  int matrix = 6, fullRange = 0;
  std::vector<uint16_t> planes[3];  // Y, U, V (full sample values)
  int cw = 0, ch = 0;                // chroma plane size
};

void NoFree(const uint8_t*, void*) {}

void PictureToFrame(const Dav1dPicture& pic, Frame& out) {
  out.w = pic.p.w;
  out.h = pic.p.h;
  out.bpc = pic.p.bpc;
  out.layout = pic.p.layout;
  if (pic.seq_hdr) {
    out.matrix = pic.seq_hdr->mtrx;
    out.fullRange = pic.seq_hdr->color_range;
  }
  int ssx = out.layout == DAV1D_PIXEL_LAYOUT_I420 || out.layout == DAV1D_PIXEL_LAYOUT_I422;
  int ssy = out.layout == DAV1D_PIXEL_LAYOUT_I420;
  out.cw = (out.w + ssx) >> ssx;
  out.ch = (out.h + ssy) >> ssy;
  int planes = out.layout == DAV1D_PIXEL_LAYOUT_I400 ? 1 : 3;
  for (int p = 0; p < planes; ++p) {
    int pw = p ? out.cw : out.w, ph = p ? out.ch : out.h;
    ptrdiff_t stride = pic.stride[p ? 1 : 0];
    const uint8_t* src = (const uint8_t*)pic.data[p];
    out.planes[p].resize((size_t)pw * ph);
    for (int y = 0; y < ph; ++y) {
      const uint8_t* row = src + y * stride;
      uint16_t* dst = &out.planes[p][(size_t)y * pw];
      if (out.bpc == 8)
        for (int x = 0; x < pw; ++x) dst[x] = row[x];
      else
        for (int x = 0; x < pw; ++x) dst[x] = ((const uint16_t*)row)[x];
    }
  }
}

// A dav1d decoder fed one temporal unit (still item or sequence sample) at
// a time.
class Av1Stream {
 public:
  Av1Stream() : ctx_(0) {
    Dav1dSettings s;
    dav1d_default_settings(&s);
    s.n_threads = 1;
    s.max_frame_delay = 1;
    s.frame_size_limit = 8192 * 8192;
    if (dav1d_open(&ctx_, &s) < 0) ctx_ = 0;
  }
  ~Av1Stream() {
    if (ctx_) dav1d_close(&ctx_);
  }
  // Returns true if the unit produced a picture.
  bool Decode(const uint8_t* p, size_t n, Frame& out) {
    if (!ctx_ || !n) return false;
    Dav1dData data;
    memset(&data, 0, sizeof data);
    if (dav1d_data_wrap(&data, p, n, NoFree, 0) < 0) return false;
    bool got = false;
    for (int guard = 0; guard < 64; ++guard) {
      if (data.sz > 0) {
        int r = dav1d_send_data(ctx_, &data);
        if (r < 0 && r != DAV1D_ERR(EAGAIN)) break;
      }
      Dav1dPicture pic;
      memset(&pic, 0, sizeof pic);
      int r = dav1d_get_picture(ctx_, &pic);
      if (r == 0) {
        if (pic.p.w > 0 && pic.p.h > 0) {
          PictureToFrame(pic, out);
          got = true;
        }
        dav1d_picture_unref(&pic);
        if (data.sz == 0) break;
      } else if (r != DAV1D_ERR(EAGAIN) || (data.sz == 0 && guard > 4)) {
        break;
      }
    }
    if (data.sz > 0) dav1d_data_unref(&data);
    return got;
  }

 private:
  Dav1dContext* ctx_;
};

bool DecodeAv1(const std::vector<uint8_t>& obu, Frame& out) {
  Av1Stream s;
  return s.Decode(obu.data(), obu.size(), out);
}

// Matrix coefficients (H.273): returns Kr, Kb.
void Coefficients(int matrix, float& kr, float& kb) {
  switch (matrix) {
    case 1: kr = 0.2126f; kb = 0.0722f; break;  // BT.709
    case 4: kr = 0.30f; kb = 0.11f; break;      // FCC
    case 9: case 10: kr = 0.2627f; kb = 0.0593f; break;  // BT.2020
    case 7: kr = 0.212f; kb = 0.087f; break;    // SMPTE 240M
    default: kr = 0.299f; kb = 0.114f; break;   // BT.601 (also "unspecified")
  }
}

// Converts to premultiplied BGRA (alpha from |alpha| if given).
void ToRgb(const Frame& f, const Frame* alpha, int matrixOverride, int rangeOverride, std::vector<uint32_t>& out) {
  int matrix = matrixOverride >= 0 ? matrixOverride : f.matrix;
  bool full = rangeOverride >= 0 ? rangeOverride != 0 : f.fullRange != 0;
  float kr, kb;
  Coefficients(matrix, kr, kb);
  float kg = 1 - kr - kb;
  float maxv = (float)((1 << f.bpc) - 1);
  float scale = (float)(1 << (f.bpc - 8));
  bool mono = f.layout == DAV1D_PIXEL_LAYOUT_I400;
  int ssx = f.cw < f.w ? 1 : 0, ssy = f.ch < f.h ? 1 : 0;
  out.resize((size_t)f.w * f.h);
  for (int y = 0; y < f.h; ++y) {
    for (int x = 0; x < f.w; ++x) {
      float Y = f.planes[0][(size_t)y * f.w + x];
      float U = 0, V = 0;
      if (!mono) {
        size_t ci = (size_t)(y >> ssy) * f.cw + (x >> ssx);
        U = f.planes[1][ci];
        V = f.planes[2][ci];
      }
      float yn, un, vn;
      if (full) {
        yn = Y / maxv;
        un = (U - (maxv + 1) / 2) / maxv;
        vn = (V - (maxv + 1) / 2) / maxv;
      } else {
        yn = (Y - 16 * scale) / (219 * scale);
        un = (U - 128 * scale) / (224 * scale);
        vn = (V - 128 * scale) / (224 * scale);
      }
      float r, g, b;
      if (mono) {
        r = g = b = yn;
      } else if (matrix == 0) {  // identity: GBR
        g = Y / maxv;
        b = U / maxv;
        r = V / maxv;
      } else {
        r = yn + (2 - 2 * kr) * vn;
        b = yn + (2 - 2 * kb) * un;
        g = (yn - kr * r - kb * b) / kg;
      }
      float a = 1;
      if (alpha && alpha->w == f.w && alpha->h == f.h) {
        float av = alpha->planes[0][(size_t)y * f.w + x];
        float amax = (float)((1 << alpha->bpc) - 1), ascale = (float)(1 << (alpha->bpc - 8));
        a = alpha->fullRange ? av / amax : (av - 16 * ascale) / (219 * ascale);
        a = std::max(0.0f, std::min(1.0f, a));
      }
      unsigned A = (unsigned)(a * 255 + 0.5f);
      unsigned R = (unsigned)(std::max(0.0f, std::min(1.0f, r)) * a * 255 + 0.5f);
      unsigned G = (unsigned)(std::max(0.0f, std::min(1.0f, g)) * a * 255 + 0.5f);
      unsigned B = (unsigned)(std::max(0.0f, std::min(1.0f, b)) * a * 255 + 0.5f);
      out[(size_t)y * f.w + x] = (A << 24) | (R << 16) | (G << 8) | B;
    }
  }
}

// Decodes one coded item (av01) or a grid of them into |px|.
bool DecodeItem(const Container& c, uint32_t id, int matrix, int range, const Frame* alphaPlane, int& w,
                int& h, std::vector<uint32_t>& px, Frame* keepFrame) {
  std::map<uint32_t, Item>::const_iterator it = c.items.find(id);
  if (it == c.items.end()) return false;
  if (it->second.type == Tag("av01")) {
    std::vector<uint8_t> obu;
    Frame f;
    if (!c.ItemData(id, obu) || !DecodeAv1(obu, f)) return false;
    w = f.w;
    h = f.h;
    if (keepFrame) {
      *keepFrame = f;
      return true;
    }
    ToRgb(f, alphaPlane, matrix, range, px);
    return true;
  }
  if (it->second.type == Tag("grid")) {
    std::vector<uint8_t> g;
    if (!c.ItemData(id, g) || g.size() < 8) return false;
    int large = g[1] & 1;
    int rows = g[2] + 1, cols = g[3] + 1;
    size_t need = 4 + (large ? 8 : 4);
    if (g.size() < need) return false;
    uint32_t ow = Be(&g[4], large ? 4 : 2), oh = Be(&g[4 + (large ? 4 : 2)], large ? 4 : 2);
    const std::vector<uint32_t>& tiles = it->second.dimg;
    if ((int)tiles.size() != rows * cols || ow == 0 || oh == 0 || (uint64_t)ow * oh > 8192ull * 8192) return false;
    w = (int)ow;
    h = (int)oh;
    px.assign((size_t)w * h, 0);
    for (int r = 0; r < rows; ++r)
      for (int col = 0; col < cols; ++col) {
        int tw, th;
        std::vector<uint32_t> tile;
        if (!DecodeItem(c, tiles[r * cols + col], matrix, range, 0, tw, th, tile, 0)) return false;
        for (int y = 0; y < th; ++y) {
          int dy = r * th + y;
          if (dy >= h) break;
          for (int x = 0; x < tw; ++x) {
            int dx = col * tw + x;
            if (dx >= w) break;
            px[(size_t)dy * w + dx] = tile[(size_t)y * tw + x];
          }
        }
      }
    return true;
  }
  return false;
}

}  // namespace

bool LooksLikeAvif(const std::string& data) {
  if (data.size() < 16 || data.compare(4, 4, "ftyp") != 0) return false;
  size_t size = Be((const uint8_t*)data.data(), 4);
  if (size < 16 || size > data.size()) return false;
  for (size_t p = 8; p + 4 <= size; p += 4) {
    if (p == 12) continue;  // minor version
    std::string brand = data.substr(p, 4);
    if (brand == "avif" || brand == "avis") return true;
  }
  return false;
}

bool DecodeAvif(const std::string& data, int& width, int& height, std::vector<uint32_t>& pixels, bool& hasAlpha) {
  if (!LooksLikeAvif(data)) return false;
  std::vector<Box> top;
  if (!ReadBoxes((const uint8_t*)data.data(), data.size(), top)) return false;
  const Box* meta = Find(top, "meta");
  if (!meta) return false;  // animated-only (avis without a still item)
  Container c;
  c.file = (const uint8_t*)data.data();
  c.fileSize = data.size();
  if (!ParseMeta(*meta, c) || !c.primary) return false;

  // Colour information from 'colr' (nclx) overrides the AV1 headers.
  int matrix = -1, range = -1;
  if (const Property* colr = c.Prop(c.primary, "colr")) {
    if (colr->size >= 11 && Be(colr->data, 4) == Tag("nclx")) {
      matrix = (int)Be(colr->data + 8, 2);
      range = colr->data[10] >> 7;
    }
  }
  // Alpha plane: an auxiliary item referencing the primary one.
  Frame alpha;
  bool haveAlpha = false;
  for (std::map<uint32_t, Item>::const_iterator it = c.items.begin(); it != c.items.end(); ++it) {
    if (it->second.auxlFor != c.primary) continue;
    const Property* auxc = c.Prop(it->first, "auxC");
    if (!auxc || auxc->size < 5) continue;
    std::string urn((const char*)auxc->data + 4, auxc->size - 4);
    if (urn.find("alpha") == std::string::npos) continue;
    int aw, ah;
    std::vector<uint32_t> unused;
    if (DecodeItem(c, it->first, -1, -1, 0, aw, ah, unused, &alpha)) haveAlpha = true;
    break;
  }
  int w, h;
  std::vector<uint32_t> px;
  if (!DecodeItem(c, c.primary, matrix, range, haveAlpha ? &alpha : 0, w, h, px, 0)) return false;

  // Clean aperture is ignored; rotation (irot) and mirroring (imir) apply.
  if (const Property* imir = c.Prop(c.primary, "imir")) {
    if (imir->size >= 1) {
      bool vertical = (imir->data[0] & 1) == 0;  // axis 0: top-bottom flip
      std::vector<uint32_t> m(px.size());
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          m[(size_t)y * w + x] = vertical ? px[(size_t)(h - 1 - y) * w + x] : px[(size_t)y * w + (w - 1 - x)];
      px.swap(m);
    }
  }
  if (const Property* irot = c.Prop(c.primary, "irot")) {
    int turns = irot->size >= 1 ? (irot->data[0] & 3) : 0;  // anti-clockwise quarter turns
    for (int t = 0; t < turns; ++t) {
      std::vector<uint32_t> r(px.size());
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) r[(size_t)(w - 1 - x) * h + y] = px[(size_t)y * w + x];
      px.swap(r);
      std::swap(w, h);
    }
  }
  width = w;
  height = h;
  pixels.swap(px);
  hasAlpha = haveAlpha;
  return true;
}

namespace {

struct Track {
  uint32_t id = 0, timescale = 1000, handler = 0, auxlFor = 0;
  bool av01 = false;
  std::vector<uint8_t> config;  // av1C configOBUs
  int matrix = -1, range = -1;
  std::vector<std::pair<uint32_t, uint32_t> > samples;  // file offset, size
  std::vector<uint32_t> durations;
};

std::vector<Box> Children(const Box& b, size_t skip = 0) {
  std::vector<Box> out;
  if (b.size >= skip) ReadBoxes(b.data + skip, b.size - skip, out);
  return out;
}

bool ParseStbl(const Box& stbl, Track& t) {
  std::vector<Box> boxes = Children(stbl);
  if (const Box* stsd = Find(boxes, "stsd")) {
    std::vector<Box> entries = Children(*stsd, 8);
    if (!entries.empty() && entries[0].type == Tag("av01") && entries[0].size >= 78) {
      t.av01 = true;
      std::vector<Box> sub = Children(entries[0], 78);
      if (const Box* av1c = Find(sub, "av1C"))
        if (av1c->size > 4) t.config.assign(av1c->data + 4, av1c->data + av1c->size);
      if (const Box* colr = Find(sub, "colr"))
        if (colr->size >= 11 && Be(colr->data, 4) == Tag("nclx")) {
          t.matrix = (int)Be(colr->data + 8, 2);
          t.range = colr->data[10] >> 7;
        }
    }
  }
  const Box* stsz = Find(boxes, "stsz");
  const Box* stsc = Find(boxes, "stsc");
  const Box* stco = Find(boxes, "stco");
  const Box* co64 = Find(boxes, "co64");
  if (!stsz || !stsc || (!stco && !co64) || stsz->size < 12 || stsc->size < 8) return false;
  uint32_t fixed = Be(stsz->data + 4, 4), count = Be(stsz->data + 8, 4);
  if (count > 100000 || (!fixed && stsz->size < 12 + (size_t)count * 4)) return false;
  std::vector<uint32_t> sizes(count);
  for (uint32_t i = 0; i < count; ++i) sizes[i] = fixed ? fixed : Be(stsz->data + 12 + i * 4, 4);
  std::vector<uint32_t> chunks;
  const Box* co = stco ? stco : co64;
  int osize = stco ? 4 : 8;
  if (co->size < 8) return false;
  uint32_t nchunks = Be(co->data + 4, 4);
  if (nchunks > 100000 || co->size < 8 + (size_t)nchunks * osize) return false;
  for (uint32_t i = 0; i < nchunks; ++i) chunks.push_back(Be(co->data + 8 + i * osize + (osize - 4), 4));
  uint32_t nsc = Be(stsc->data + 4, 4);
  if (nsc > 100000 || stsc->size < 8 + (size_t)nsc * 12) return false;
  size_t sample = 0;
  for (uint32_t c = 0; c < nchunks && sample < count; ++c) {
    uint32_t perChunk = 0;
    for (uint32_t e = 0; e < nsc; ++e)
      if (Be(stsc->data + 8 + e * 12, 4) <= c + 1) perChunk = Be(stsc->data + 8 + e * 12 + 4, 4);
    uint32_t off = chunks[c];
    for (uint32_t k = 0; k < perChunk && sample < count; ++k, ++sample) {
      t.samples.push_back(std::make_pair(off, sizes[sample]));
      off += sizes[sample];
    }
  }
  if (const Box* stts = Find(boxes, "stts")) {
    if (stts->size >= 8) {
      uint32_t n = Be(stts->data + 4, 4);
      for (uint32_t e = 0; e < n && stts->size >= 8 + (size_t)(e + 1) * 8; ++e) {
        uint32_t c = Be(stts->data + 8 + e * 8, 4), d = Be(stts->data + 12 + e * 8, 4);
        for (uint32_t k = 0; k < c && t.durations.size() < t.samples.size(); ++k) t.durations.push_back(d);
      }
    }
  }
  return !t.samples.empty();
}

bool ParseTrak(const Box& trak, Track& t) {
  std::vector<Box> boxes = Children(trak);
  if (const Box* tkhd = Find(boxes, "tkhd")) {
    size_t at = tkhd->size && tkhd->data[0] == 1 ? 20 : 12;
    if (tkhd->size >= at + 4) t.id = Be(tkhd->data + at, 4);
  }
  if (const Box* tref = Find(boxes, "tref")) {
    std::vector<Box> refs = Children(*tref);
    if (const Box* auxl = Find(refs, "auxl"))
      if (auxl->size >= 4) t.auxlFor = Be(auxl->data, 4);
  }
  const Box* mdia = Find(boxes, "mdia");
  if (!mdia) return false;
  std::vector<Box> m = Children(*mdia);
  if (const Box* mdhd = Find(m, "mdhd")) {
    size_t at = mdhd->size && mdhd->data[0] == 1 ? 20 : 12;
    if (mdhd->size >= at + 4) t.timescale = std::max(1u, Be(mdhd->data + at, 4));
  }
  if (const Box* hdlr = Find(m, "hdlr"))
    if (hdlr->size >= 12) t.handler = Be(hdlr->data + 8, 4);
  const Box* minf = Find(m, "minf");
  if (!minf) return false;
  std::vector<Box> mi = Children(*minf);
  const Box* stbl = Find(mi, "stbl");
  return stbl && ParseStbl(*stbl, t);
}

}  // namespace

bool DecodeAvifSequence(const std::string& data, AvifAnimation& out, size_t maxBytes) {
  if (!LooksLikeAvif(data)) return false;
  const uint8_t* file = (const uint8_t*)data.data();
  std::vector<Box> top;
  if (!ReadBoxes(file, data.size(), top)) return false;
  const Box* moov = Find(top, "moov");
  if (!moov) return false;
  std::vector<Box> mb = Children(*moov);
  std::vector<Track> tracks;
  for (size_t i = 0; i < mb.size(); ++i) {
    if (mb[i].type != Tag("trak")) continue;
    Track t;
    if (ParseTrak(mb[i], t) && t.av01) tracks.push_back(t);
  }
  const Track* color = 0;
  const Track* alpha = 0;
  for (size_t i = 0; i < tracks.size() && !color; ++i)
    if (!tracks[i].auxlFor) color = &tracks[i];
  if (!color) return false;
  for (size_t i = 0; i < tracks.size() && !alpha; ++i)
    if (tracks[i].auxlFor == color->id && tracks[i].samples.size() == color->samples.size()) alpha = &tracks[i];
  Av1Stream cs, as;
  Frame f, af;
  bool haveAlpha = false;
  size_t bytes = 0;
  for (size_t i = 0; i < color->samples.size(); ++i) {
    std::pair<uint32_t, uint32_t> s = color->samples[i];
    if ((uint64_t)s.first + s.second > data.size()) break;
    std::vector<uint8_t> unit;
    if (i == 0) unit = color->config;  // sequence header, in case samples lack it
    unit.insert(unit.end(), file + s.first, file + s.first + s.second);
    bool got = cs.Decode(unit.data(), unit.size(), f);
    if (alpha) {
      std::pair<uint32_t, uint32_t> a = alpha->samples[i];
      if ((uint64_t)a.first + a.second <= data.size()) {
        std::vector<uint8_t> au;
        if (i == 0) au = alpha->config;
        au.insert(au.end(), file + a.first, file + a.first + a.second);
        if (as.Decode(au.data(), au.size(), af)) haveAlpha = true;
      }
    }
    if (!got && out.frames.empty()) continue;
    if (got && out.frames.empty()) {
      out.width = f.w;
      out.height = f.h;
    }
    if (f.w != out.width || f.h != out.height) break;
    out.frames.push_back(std::vector<uint32_t>());
    ToRgb(f, haveAlpha ? &af : 0, color->matrix, color->range, out.frames.back());
    uint32_t d = i < color->durations.size() ? color->durations[i] : color->timescale / 10;
    out.delays.push_back((int)((uint64_t)d * 1000 / color->timescale));
    bytes += out.frames.back().size() * 4;
    if (bytes > maxBytes || out.frames.size() >= 2000) break;
  }
  out.hasAlpha = haveAlpha;
  return !out.frames.empty();
}

}  // namespace kite
