/* Host test of the index reader, MGVF writer and the player's pacing, seek,
 * tailing and end of file, on a synthetic stream with B frames and a fake
 * decoder that reorders like a DPB. Fake time: every step gets `now`. */
#undef NDEBUG
#include "../src/fit.h"
#include "../src/index.h"
#include "../src/mgvf.h"
#include "../src/player.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace ytdec;

namespace {

const uint32_t W = 64, H = 36;   /* output */
const int DUR = 33;

/* Decoding order of a GOP of 12 with B pairs: I0 P3 B1 B2 P6 B4 B5 ... */
std::vector<int> decode_order(int n)
{
    std::vector<int> o;
    for (int g = 0; g < n; g += 12) {
        o.push_back(g);
        for (int k = g + 3; k < g + 12; k += 3) {
            o.push_back(k);
            o.push_back(k - 2);
            o.push_back(k - 1);
        }
        o.push_back(g + 10);
        o.push_back(g + 11);
    }
    o.erase(std::remove_if(o.begin(), o.end(), [n](int f) { return f >= n; }), o.end());
    return o;
}

struct Stream {
    std::vector<uint8_t> video;
    std::vector<IndexRec> recs;
};

/* frame f's access unit: [SPS PPS] slice, the frame number in the payload. A
 * key without SPS (every 24th frame from 12): an open-GOP I frame. */
Stream make_stream(int n)
{
    Stream s;
    for (int f : decode_order(n)) {
        IndexRec r;
        r.pts_ms = (uint32_t)(f * DUR);
        r.pos = s.video.size();
        const bool key = f % 12 == 0, sps = f % 24 == 0;
        auto nal = [&](uint8_t t) {
            const uint8_t sc[] = {0, 0, 0, 1, t, (uint8_t)(f >> 8), (uint8_t)f, 0x80};
            s.video.insert(s.video.end(), sc, sc + sizeof sc);
        };
        if (sps) {
            nal(0x67);
            nal(0x68);
        }
        nal(key ? (sps ? 0x65 : 0x61) : 0x41);
        r.size = (uint32_t)(s.video.size() - r.pos);
        r.key = key;
        s.recs.push_back(r);
    }
    return s;
}

void put16(std::vector<uint8_t> &b, uint16_t v) { b.push_back((uint8_t)v), b.push_back((uint8_t)(v >> 8)); }
void put32(std::vector<uint8_t> &b, uint32_t v) { put16(b, (uint16_t)v), put16(b, (uint16_t)(v >> 16)); }

void write_file(const std::string &p, const std::vector<uint8_t> &b)
{
    FILE *f = fopen(p.c_str(), "wb");
    assert(f);
    fwrite(b.data(), 1, b.size(), f);
    fclose(f);
}

const uint32_t kVp9 = 'V' | 'P' << 8 | '9' << 16 | ' ' << 24;   /* MFX_CODEC_VP9 */

std::vector<uint8_t> index_bytes(const Stream &s, size_t n, uint32_t codec = 0)
{
    std::vector<uint8_t> b = {'M', 'G', 'Y', 'I'};
    put16(b, 1), put16(b, 640), put16(b, 360), put16(b, 30), put16(b, 1);
    put16(b, 0);
    put32(b, (uint32_t)n);
    put32(b, codec);
    b.resize(32, 0);
    for (size_t i = 0; i < n; i++) {
        const IndexRec &r = s.recs[i];
        put32(b, r.pts_ms);
        put32(b, (uint32_t)r.pos), put32(b, (uint32_t)(r.pos >> 32));
        put32(b, r.size);
        b.push_back(r.key);
    }
    return b;
}

/* Feeds back what it is given, in display order after a DPB of 2. */
class FakeDecoder : public VideoDecoder {
public:
    std::vector<int64_t> held;
    uint32_t fw = W, fh = H;   /* frame size; a test changes it mid-stream */
    std::vector<uint8_t> y = std::vector<uint8_t>(4 * W * H), uv = std::vector<uint8_t>(2 * W * H, 128);
    bool started = false;
    int fed = 0, pictures = 0, flushes = 0;   /* flushes: MSDK sessions ended */

    void out(const Sink &cb, bool picture)
    {
        std::sort(held.begin(), held.end());
        const int64_t pts = held.front();
        held.erase(held.begin());
        memset(y.data(), 16 + (int)(pts / DUR) % 200, y.size());
        woc::VideoFrame f = {fw, fh, 0, pts, picture ? y.data() : nullptr, picture ? uv.data() : nullptr, fw};
        if (picture) pictures++;
        cb(f);
    }
    bool feed(const uint8_t *au, size_t len, int64_t pts, bool picture, const Sink &cb, std::string *) override
    {
        fed++;
        if (!started) {
            if (!(nal_types(au, len) & (1u << 7)))
                return true;   /* waits for an SPS, as MSDK's decoder does */
            started = true;
        }
        held.push_back(pts);
        if (held.size() > 2)
            out(cb, picture);
        return true;
    }
    void flush(const Sink &cb) override
    {
        flushes++;
        while (!held.empty())
            out(cb, true);
        started = false;
    }
};

/* VP9 frames in display order (no reordering): byte 0 is the frame header,
 * key 0x80 / inter 0x84 (frame_type bit). mode: ok, fail (every feed), silent. */
Stream make_vp9(int n)
{
    Stream s;
    for (int f = 0; f < n; f++) {
        IndexRec r;
        r.pts_ms = (uint32_t)(f * DUR);
        r.pos = s.video.size();
        r.key = f % 12 == 0;
        const uint8_t fr[] = {(uint8_t)(r.key ? 0x80 : 0x84), (uint8_t)(f >> 8), (uint8_t)f};
        s.video.insert(s.video.end(), fr, fr + sizeof fr);
        r.size = sizeof fr;
        s.recs.push_back(r);
    }
    return s;
}

class Vp9Fake : public VideoDecoder {
public:
    std::string mode = "ok";
    std::vector<uint8_t> y = std::vector<uint8_t>(W * H, 16), uv = std::vector<uint8_t>(W * H / 2, 128);
    bool started = false;
    int fed = 0;
    uint32_t codec = 0;
    bool set_codec(uint32_t c, std::string *) override { codec = c; return c == kVp9; }
    bool feed(const uint8_t *au, size_t, int64_t pts, bool picture, const Sink &cb, std::string *err) override
    {
        fed++;
        if (mode == "fail") {
            *err = "MFXVideoDECODE_DecodeHeader: MFX_ERR_UNSUPPORTED (-3)";
            return false;
        }
        started = started || au[0] == 0x80;
        if (mode == "silent" || !started)
            return true;
        woc::VideoFrame f = {W, H, 0, pts, picture ? y.data() : nullptr, picture ? uv.data() : nullptr, W};
        cb(f);
        return true;
    }
    void flush(const Sink &) override { started = false; }
};

struct Rig {
    std::string dir;
    Mgvf out;
    FakeDecoder dec;
    std::vector<std::string> events;
    Player *p = nullptr;
    int64_t now = 1000000;
    std::vector<int64_t> shown_pts;

    explicit Rig(const std::string &d, VideoDecoder *other = nullptr, uint32_t rw = W, uint32_t rh = H) : dir(d)
    {
        std::string err, warn;
        bool ok = out.open((d + "/shm").c_str(), rw, rh, 3, &err, &warn);
        assert(ok);
        p = new Player(d, other ? other : &dec, &out, [this](const std::string &j) { events.push_back(j); },
                       [](const std::string &) {}, [] { return 0.0; });
    }
    ~Rig() { delete p; }
    /* runs fake time forward by ms, calling step as main would */
    void run(int ms)
    {
        const int64_t end = now + ms;
        while (now < end) {
            const uint64_t before = p->shown;
            int w = p->step(now);
            if (p->shown != before)
                shown_pts.push_back(p->last_shown_pts);
            now += w < 1 ? 1 : w;
        }
    }
    int count(const char *ev)
    {
        int n = 0;
        for (auto &e : events)
            n += e.find(std::string("\"ev\":\"") + ev + "\"") != std::string::npos;
        return n;
    }
    void cmd(const std::string &c) { p->command(c, now); }
    void clock(int64_t elapsed) { cmd("clock " + std::to_string(elapsed) + " " + std::to_string(now)); }
};

std::string tmpdir()
{
    char t[] = "/tmp/ytdec_test.XXXXXX";
    assert(mkdtemp(t));
    return t;
}

void test_nv12_and_mgvf(const std::string &d)
{
    Mgvf m;
    std::string err, warn;
    assert(!m.open((d + "/bad").c_str(), 15, 16, 3, &err, &warn));
    assert(!m.open((d + "/bad").c_str(), 2048, 2048 + 2, 3, &err, &warn));
    assert(m.open((d + "/m").c_str(), 16, 16, 3, &err, &warn));
    struct stat st;
    assert(stat((d + "/m").c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
    assert((size_t)st.st_size == 64 + 3 * 16 * 16 * 3 / 2);
    uint8_t y[16 * 2 * 16], uv[16 * 2 * 8];
    for (int i = 0; i < (int)sizeof y; i++) y[i] = (uint8_t)i;
    for (int i = 0; i < (int)sizeof uv; i++) uv[i] = (uint8_t)(i % 2 ? 200 : 100);
    uint8_t *dst = m.begin();
    nv12_to_i420(y, uv, 32, 16, 16, dst);   /* pitch 32: half of each row is padding */
    assert(dst[0] == 0 && dst[16] == 32 && dst[16 * 16] == 100 && dst[16 * 16 + 64] == 200);
    m.commit();
    m.close();
    int fd = open((d + "/m").c_str(), O_RDONLY);
    MgvfHead h;
    assert(pread(fd, &h, sizeof h, 0) == (ssize_t)sizeof h);
    close(fd);
    assert(h.magic == kMgvfMagic && h.version == 1 && h.w == 16 && h.h == 16 && h.nslot == 3);
    assert(h.seq == 1 && h.slot == 1 && h.matrix == 1 && h.full == 0);
    printf("  mgvf + nv12_to_i420 ok\n");
}

/* fit.h: what VPP makes of the car's 480p stream on the 1920x720 screen */
void test_fit()
{
    using woc::fit_frame;
    /* fit, today: the whole rect, the 16:9 picture 1280x720 in its middle */
    woc::Fit f = fit_frame(0, 0, 854, 480, 1, 1, 1920, 720, false, false);
    assert(f.w == 1920 && f.h == 720 && f.pic_x == 320 && f.pic_y == 0 && f.pic_w == 1280 && f.pic_h == 720);
    assert(f.src_w == 854 && f.src_h == 480);
    /* --no-upscale: the picture alone at the source's size */
    f = fit_frame(0, 0, 854, 480, 1, 1, 1920, 720, false, true);
    assert(f.w == 854 && f.h == 480 && f.pic_x == 0 && f.pic_y == 0 && f.pic_w == 854 && f.pic_h == 480);
    assert(f.src_x == 0 && f.src_y == 0 && f.src_w == 854 && f.src_h == 480);
    /* fill: the 8:3 centre band, 854x320 from row 80; without the flag scaled up to 1920x720 */
    f = fit_frame(0, 0, 854, 480, 0, 0, 1920, 720, true, true);
    assert(f.w == 854 && f.h == 320 && f.src_y == 80 && f.src_h == 320 && f.src_w == 854);
    f = fit_frame(0, 0, 854, 480, 0, 0, 1920, 720, true, false);
    assert(f.w == 1920 && f.h == 720 && f.pic_w == 1920 && f.src_h == 320);
    /* 1080p: scaled down to the picture's place, 1280x720, no bars */
    f = fit_frame(0, 0, 1920, 1080, 1, 1, 1920, 720, false, true);
    assert(f.w == 1280 && f.h == 720 && f.src_w == 1920 && f.src_h == 1080);
    /* 720p (Wi-Fi): its own size; a 2.39:1 film fills (crops under 15 %) at the source's resolution */
    f = fit_frame(0, 0, 1280, 720, 1, 1, 1920, 720, false, true);
    assert(f.w == 1280 && f.h == 720);
    f = fit_frame(0, 0, 1280, 536, 1, 1, 1920, 720, false, true);
    assert(f.w == 1280 && f.h == 480 && f.src_y == 28 && f.src_h == 480);   /* 8:3, rows cropped */
    /* the player's box when the layout is not full: 1576x432 */
    f = fit_frame(0, 0, 854, 480, 1, 1, 1576, 432, false, true);
    assert(f.w == 768 && f.h == 432);
    printf("  fit / fill / no-upscale geometry ok\n");
}

/* --no-upscale: the MGVF file takes the decoder's frame size (opened at the
 * rect), and follows a change mid-stream with old-size frames still queued */
void test_frame_size(const std::string &d, const Stream &s)
{
    write_file(d + "/video.h264", s.video);
    write_file(d + "/index.bin", index_bytes(s, s.recs.size()));
    Rig r(d, nullptr, 2 * W, 2 * H);   /* the rect, larger than the frames */
    auto head = [&](MgvfHead *h) {
        int fd = open((d + "/shm").c_str(), O_RDONLY);
        assert(fd >= 0 && pread(fd, h, sizeof *h, 0) == (ssize_t)sizeof *h);
        struct stat st;
        assert(fstat(fd, &st) == 0);
        uint8_t y0 = 0;   /* the newest frame's first luma byte */
        assert(pread(fd, &y0, 1, (off_t)(64 + (size_t)h->slot * h->w * h->h * 3 / 2)) == 1);
        close(fd);
        assert(y0 == 16 + (int)(r.p->last_shown_pts / DUR) % 200);
        return (size_t)st.st_size;
    };
    MgvfHead h;
    r.clock(0);
    r.run(500);
    assert(head(&h) == 64 + 3 * W * H * 3 / 2 && h.w == W && h.h == H && h.seq == r.p->shown);
    const uint64_t before = r.p->shown;
    r.dec.fw = 3 * W / 2, r.dec.fh = 3 * H / 2;   /* a new SPS: 96x54 */
    r.run(500);
    assert(head(&h) == 64 + 3 * (3 * W / 2) * (3 * H / 2) * 3 / 2 && h.w == 3 * W / 2 && h.h == 3 * H / 2);
    assert(h.seq == r.p->shown && r.p->shown >= before + 14 && r.p->dropped == 0 && !r.p->fatal);
    printf("  frame size follows the decoder: %ux%u, then %ux%u mid-stream\n", W, H, h.w, h.h);
}

void test_index(const std::string &d, const Stream &s)
{
    write_file(d + "/video.h264", s.video);
    std::vector<uint8_t> b = index_bytes(s, s.recs.size());
    b.resize(b.size() - 5);   /* the last record half written */
    write_file(d + "/index.bin", b);
    Index idx;
    std::string err;
    assert(idx.open(d + "/index.bin", &err));
    assert(idx.w == 640 && idx.h == 360 && idx.fps_num == 30 && idx.recs.size() == s.recs.size() - 1);
    write_file(d + "/index.bin", index_bytes(s, s.recs.size()));
    assert(idx.refresh() && idx.recs.size() == s.recs.size());
    for (size_t i = 0; i < s.recs.size(); i++)
        assert(idx.recs[i].pts_ms == s.recs[i].pts_ms && idx.recs[i].pos == s.recs[i].pos && idx.recs[i].key == s.recs[i].key);
    int vfd = open((d + "/video.h264").c_str(), O_RDONLY);
    /* frame 12 is a key without SPS: a seek to 13 frames lands on frame 0 */
    assert(idx.recs[idx.seek_point(13 * DUR, vfd)].pts_ms == 0);
    assert(idx.recs[idx.seek_point(30 * DUR, vfd)].pts_ms == 24 * DUR);
    assert(idx.seek_point(0, vfd) == 0);
    close(vfd);
    printf("  index ok (%zu records)\n", idx.recs.size());
}

void test_playback(const std::string &d)
{
    Rig r(d);
    /* no clock: frame 0 and hold */
    r.run(500);
    assert(r.count("ready") == 1 && r.p->shown == 1 && r.p->last_shown_pts == 0 && r.out.seq() == 1);
    /* clock from 0: real time, no drops, pts in order */
    r.clock(0);
    r.run(1000);
    assert(r.p->dropped == 0);
    assert(r.p->shown >= 30 && r.p->shown <= 32);
    for (size_t i = 1; i < r.shown_pts.size(); i++)
        assert(r.shown_pts[i] == r.shown_pts[i - 1] + DUR);
    const int64_t at = r.p->last_shown_pts;
    assert(at >= 1000 - DUR && at <= 1000 + DUR);
    /* the audio jumps 500 ms ahead: the late frames are dropped, then on time */
    r.clock(at + 500);
    r.run(1000);
    assert(r.p->dropped >= 10);
    assert(std::abs((long)(r.p->last_shown_pts - (at + 1500))) <= 2 * DUR);
    /* pause: nothing more */
    r.cmd("pause");
    uint64_t n = r.p->shown;
    r.run(1000);
    assert(r.p->shown == n);
    /* seek while paused: the target frame, then hold */
    r.cmd("seek 3000");
    r.run(300);
    assert(r.p->shown == n + 1);
    assert(r.p->last_shown_pts >= 3000 && r.p->last_shown_pts < 3000 + DUR);
    r.cmd("resume");
    r.run(500);
    assert(r.p->last_shown_pts >= 3500 - DUR && r.p->last_shown_pts <= 3500 + DUR);
    /* video 0: nothing written, the clock runs on; video 1 picks up at the clock */
    r.cmd("video 0");
    n = r.p->shown;
    uint32_t seq = r.out.seq();
    r.run(1000);
    assert(r.p->shown == n && r.out.seq() == seq);
    r.cmd("video 1");
    r.run(200);
    assert(r.p->last_shown_pts >= 4700 - DUR && r.p->last_shown_pts <= 4700 + DUR);
    /* stats every 2 s */
    assert(r.count("stats") >= 2);
    /* to the end: eof once, then a seek back plays again */
    r.run(10000);
    assert(r.count("eof") == 1);
    assert(r.p->last_shown_pts == 299 * DUR);
    r.cmd("seek 0");
    r.run(200);
    assert(r.p->eof_sent == false && r.p->last_shown_pts <= 200 + DUR);
    printf("  playback ok: shown %llu dropped %llu decoded %llu\n", (unsigned long long)r.p->shown,
           (unsigned long long)r.p->dropped, (unsigned long long)r.p->decoded);
}

/* idle: 60 s paused (or video 0) flushes the decoder, which ends its MSDK
 * session; the picture and the process stay; resume seeks to the paused
 * position, a seek or video 1 seeks anyway */
void test_idle(const std::string &d)
{
    const int64_t idle = Player::kIdleMs;
    Rig r(d);
    r.run(300);
    r.clock(0);
    r.run(2000);
    /* paused under kIdleMs, then resumed: the session stays */
    r.cmd("pause");
    int f = r.dec.flushes;
    r.run(10000);
    r.cmd("resume");
    r.run(500);
    assert(r.dec.flushes == f && r.p->last_shown_pts >= 2500 - DUR && r.p->last_shown_pts <= 2500 + DUR);
    /* paused kIdleMs: closed once; a second pause does not re-arm it */
    r.cmd("pause");
    const int64_t at = r.p->last_shown_pts;
    const uint32_t seq = r.out.seq();
    r.run(30000);
    r.cmd("pause");
    r.run((int)idle - 30000 - 500);
    assert(r.dec.flushes == f);
    const int fed = r.dec.fed, stats = r.count("stats");
    const uint64_t shown = r.p->shown;
    r.run(1000);
    assert(r.dec.flushes == f + 1);
    r.run(10000);
    assert(r.dec.flushes == f + 1 && r.dec.fed == fed && r.p->shown == shown && r.out.seq() == seq);
    assert(r.count("stats") > stats && r.p->fatal == 0);   /* alive: stats go on */
    /* resume: one seek (a new session from the keyframe before), nothing shown
     * before the paused position, then on the clock */
    r.cmd("resume");
    const size_t from = r.shown_pts.size();
    r.run(500);
    assert(r.dec.flushes == f + 2 && r.dec.fed > fed && r.shown_pts.size() > from);
    for (size_t i = from; i < r.shown_pts.size(); i++)
        assert(r.shown_pts[i] >= at);
    assert(r.p->last_shown_pts >= at + 500 - 2 * DUR && r.p->last_shown_pts <= at + 500 + DUR);
    /* seek then resume while closed: one reopen */
    r.cmd("pause");
    r.run((int)idle + 500);
    f = r.dec.flushes;
    assert(f >= 1);
    r.cmd("seek 6000");
    r.cmd("resume");
    r.run(500);
    assert(r.dec.flushes == f + 1);
    assert(r.p->last_shown_pts >= 6500 - DUR && r.p->last_shown_pts <= 6500 + DUR);
    /* a seek while paused shows its frame, then idle again from the seek */
    r.cmd("pause");
    r.run(40000);
    f = r.dec.flushes;
    r.cmd("seek 3000");
    r.run(300);
    assert(r.p->last_shown_pts >= 3000 && r.p->last_shown_pts < 3000 + DUR);
    r.run((int)idle - 1000);
    assert(r.dec.flushes == f + 1);   /* the seek's own */
    r.run(1000);
    assert(r.dec.flushes == f + 2);
    r.cmd("resume");
    r.run(500);
    assert(r.p->last_shown_pts >= 3500 - DUR && r.p->last_shown_pts <= 3500 + DUR);
    /* video 0: closed at once, nothing fed; video 1 at the clock */
    f = r.dec.flushes;
    r.cmd("video 0");
    assert(r.dec.flushes == f + 1);
    const int fed0 = r.dec.fed;
    r.run(1000);
    assert(r.dec.fed == fed0);
    r.cmd("video 1");
    r.run(200);
    assert(r.dec.flushes == f + 2 && r.p->last_shown_pts >= 4700 - DUR && r.p->last_shown_pts <= 4700 + DUR);
    /* eof: its flush ended the session; paused after it, no second close, and
     * resume neither seeks nor sends eof again; a seek back plays */
    r.run(10000);
    assert(r.count("eof") == 1 && r.p->last_shown_pts == 299 * DUR && !r.dec.started);
    f = r.dec.flushes;
    r.cmd("pause");
    r.run((int)idle + 1000);
    r.cmd("resume");
    r.run(1000);
    assert(r.dec.flushes == f && r.count("eof") == 1);
    r.cmd("seek 0");
    r.run(200);
    assert(!r.p->eof_sent && r.p->last_shown_pts <= 200 + DUR);
    printf("  idle close and resume ok\n");
}

/* paused before the video came (ytd.js sends pause at spawn): kIdleMs from
 * ready, so its first frame shows, then the close */
void test_idle_before_ready(const std::string &d, const Stream &s)
{
    write_file(d + "/index.bin", index_bytes(s, s.recs.size()));
    Rig r(d);
    r.cmd("pause");
    r.run((int)Player::kIdleMs + 1000);
    assert(r.count("ready") == 0);
    write_file(d + "/video.h264", s.video);
    r.run(300);
    assert(r.count("ready") == 1 && r.p->shown == 1 && r.p->last_shown_pts == 0);
    const int f = r.dec.flushes;
    r.run((int)Player::kIdleMs);
    assert(r.dec.flushes == f + 1 && r.p->shown == 1);
    printf("  idle close when paused before ready ok\n");
}

/* paused at a seek whose video comes more than kIdleMs later: the session stays
 * open until that frame is shown, then closes */
void test_idle_owed_frame(const std::string &d, const Stream &s)
{
    write_file(d + "/index.bin", index_bytes(s, s.recs.size()));
    write_file(d + "/video.h264.part", std::vector<uint8_t>());
    Rig r(d);
    r.cmd("seek 3000");
    r.cmd("pause");
    r.run((int)Player::kIdleMs + 1000);
    assert(r.p->shown == 0);
    const int f = r.dec.flushes;   /* still open: a frame is owed */
    write_file(d + "/video.h264.part", s.video);
    r.run(300);
    assert(r.p->shown == 1 && r.p->last_shown_pts >= 3000 && r.p->last_shown_pts < 3000 + DUR);
    assert(r.dec.flushes == f + 1);   /* idle long enough: closed right after the frame */
    r.run((int)Player::kIdleMs + 100);
    assert(r.dec.flushes == f + 1 && r.p->shown == 1);
    printf("  idle waits for the owed paused frame ok\n");
}

/* the fetcher's writes: index first, then video.h264.part growing, then renamed */
void test_tail(const std::string &d, const Stream &s)
{
    const size_t half = s.recs.size() / 2;
    write_file(d + "/index.bin", index_bytes(s, half));
    Rig r(d);
    r.run(300);
    assert(r.count("ready") == 0);   /* no video yet */
    std::vector<uint8_t> v(s.video.begin(), s.video.begin() + (long)s.recs[half / 2].pos);
    write_file(d + "/video.h264.part", v);
    r.clock(0);
    r.run(2000);
    assert(r.count("ready") == 1 && r.p->shown > 0 && r.count("eof") == 0);
    write_file(d + "/video.h264.part", s.video);   /* same path: our descriptor sees the new bytes */
    write_file(d + "/index.bin", index_bytes(s, s.recs.size()));
    r.run(2500);
    assert(r.count("eof") == 0);   /* still .part and growing a moment ago */
    rename((d + "/video.h264.part").c_str(), (d + "/video.h264").c_str());
    r.run(10000);
    assert(r.count("eof") == 1 && r.p->last_shown_pts == 299 * DUR);
    printf("  tail ok: shown %llu dropped %llu\n", (unsigned long long)r.p->shown, (unsigned long long)r.p->dropped);
}

/* .part never renamed (the fetcher stalled or died): no eof, however long; eof once renamed */
void test_eof_no_rename(const std::string &d, const Stream &s)
{
    write_file(d + "/index.bin", index_bytes(s, s.recs.size()));
    write_file(d + "/video.h264.part", s.video);
    Rig r(d);
    r.cmd("seek 9000");
    r.clock(9000);
    r.run(8000);
    assert(r.count("eof") == 0);
    rename((d + "/video.h264.part").c_str(), (d + "/video.h264").c_str());
    r.run(500);
    assert(r.count("eof") == 1);
    printf("  no eof while .part, eof after the rename ok\n");
}

/* a VP9 index (codec "VP9 "): keyframes are seek points without any SPS; a
 * decoder that refuses the codec, fails its first frame or puts out nothing
 * makes the player fatal (exit 4) */
void test_vp9()
{
    const Stream s = make_vp9(300);
    std::string d = tmpdir();
    write_file(d + "/video.h264", s.video);
    write_file(d + "/index.bin", index_bytes(s, s.recs.size(), kVp9));
    {
        Index idx;
        std::string err;
        assert(idx.open(d + "/index.bin", &err) && idx.codec == kVp9);
        int vfd = open((d + "/video.h264").c_str(), O_RDONLY);
        assert(idx.recs[idx.seek_point(13 * DUR, vfd)].pts_ms == 12 * DUR);
        close(vfd);
    }
    {
        Rig r(d);   /* the H.264-only fake: refuses VP9, nothing fed */
        r.run(300);
        assert(r.p->fatal == Player::kCodecFailed && r.count("error") == 1 && r.count("ready") == 0 && r.dec.fed == 0);
    }
    {
        Vp9Fake v;
        Rig r(d, &v);
        r.run(300);
        assert(v.codec == kVp9 && r.p->fatal == 0 && r.p->shown == 1 && r.p->last_shown_pts == 0);
        r.clock(0);
        r.run(1000);
        assert(r.p->fatal == 0 && r.p->dropped == 0 && r.p->shown >= 30);
        r.cmd("pause");
        r.cmd("seek 5000");   /* frame 151: from key 144 */
        r.run(300);
        assert(r.p->last_shown_pts >= 5000 && r.p->last_shown_pts < 5000 + DUR);
        r.cmd("resume");
        r.run(12000);
        assert(r.count("eof") == 1 && r.p->last_shown_pts == 299 * DUR && r.p->fatal == 0);
    }
    for (const char *mode : {"fail", "silent"}) {
        Vp9Fake v;
        v.mode = mode;
        Rig r(d, &v);
        r.clock(0);
        r.run(2000);
        assert(r.p->fatal == Player::kCodecFailed && r.count("error") == 1);
        assert(v.fed == (v.mode == "fail" ? 1 : Player::kCodecProbe));
    }
    printf("  vp9 index, seek and codec failure ok\n");
}

}  // namespace

int main()
{
    const Stream s = make_stream(300);
    test_nv12_and_mgvf(tmpdir());
    test_fit();
    test_frame_size(tmpdir(), s);
    test_index(tmpdir(), s);
    {
        std::string d = tmpdir();
        write_file(d + "/video.h264", s.video);
        write_file(d + "/index.bin", index_bytes(s, s.recs.size()));
        test_playback(d);
        test_idle(d);
    }
    test_tail(tmpdir(), s);
    test_idle_before_ready(tmpdir(), s);
    test_idle_owed_frame(tmpdir(), s);
    test_eof_no_rename(tmpdir(), s);
    test_vp9();
    printf("player_test: all passed\n");
    return 0;
}
