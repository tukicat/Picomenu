// This file is part of picoFace (GNU GPL v3 or later; see LICENSE).
// PC tests of the ZXDB downloads' pieces that don't need the network: netget.h (URLs, the
// request, responses: lengths, chunked, redirects, cut short), zxdb.h (search and entry
// answers in the API's flat form, the file URLs) and unzip.h (a zip's game written out).
//   nettest <dir with test files>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include "netget.h"
#include "zxdb.h"
#include "unzip.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

struct PcFile {
    FILE* f = nullptr;
    bool open(const char* p, const char* mode) { f = fopen(p, mode); return f != nullptr; }
    int read(void* b, size_t n) { return (int) fread(b, 1, n, f); }
    int write(const void* b, size_t n) { return (int) fwrite(b, 1, n, f); }
    bool seek(uint32_t at) { return fseek(f, (long) at, SEEK_SET) == 0; }
    int size() { long p = ftell(f); fseek(f, 0, SEEK_END); long s = ftell(f); fseek(f, p, SEEK_SET); return (int) s; }
    void close() { if (f) fclose(f); f = nullptr; }
};

static std::string body;
static bool collect(const uint8_t* p, size_t n, void*) { body.append((const char*) p, n); return true; }

// a response fed in pieces of every size from 1 up
static bool run(const char* resp, size_t piece, netget::Reader& r) {
    body.clear();
    r.begin(collect, nullptr);
    size_t n = strlen(resp);
    for (size_t i = 0; i < n; i += piece) {
        size_t k = n - i < piece ? n - i : piece;
        if (!r.feed((const uint8_t*) resp + i, k)) return false;
    }
    return true;
}

static std::string slurp(const std::string& p) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return "";
    std::string s;
    char b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof(b), f)) > 0) s.append(b, n);
    fclose(f);
    return s;
}

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : ".";
    // URLs
    netget::Url u;
    CHECK(u.parse("https://api.zxinfo.dk/v5/search/titles/x?mode=tiny") && u.tls && u.port == 443 &&
        strcmp(u.host, "api.zxinfo.dk") == 0 && strcmp(u.path, "/v5/search/titles/x?mode=tiny") == 0);
    CHECK(u.parse("http://localhost:8080") && !u.tls && u.port == 8080 && strcmp(u.path, "/") == 0);
    CHECK(!u.parse("ftp://x/y"));
    char req[600];
    CHECK(u.parse("https://spectrumcomputing.co.uk/zxdb/a.zip") && netget::request(u, req, sizeof(req)) > 0);
    CHECK(strstr(req, "GET /zxdb/a.zip HTTP/1.1\r\nHost: spectrumcomputing.co.uk\r\nUser-Agent: PicoMenu/2.6.1") == req);
    char out[600];
    CHECK(netget::resolve(u, "/x/y", out, sizeof(out)) && strcmp(out, "https://spectrumcomputing.co.uk/x/y") == 0);
    CHECK(netget::encode("Manic Miner&co", out, sizeof(out), false) && strcmp(out, "Manic%20Miner%26co") == 0);
    // responses: a length, chunked, a redirect, no length (ends when closed), cut short
    for (size_t piece = 1; piece < 40; piece++) {
        netget::Reader r;
        CHECK(run("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nX: y\r\n\r\nhelloEXTRA", piece, r) && r.done() && body == "hello");
        CHECK(run("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nWiki\r\n6;x=1\r\npedia \r\n0\r\n\r\n", piece, r) && r.done() && body == "Wikipedia ");
        CHECK(run("HTTP/1.1 302 Found\r\nLocation: https://ia800.us.archive.org/x.zip\r\nContent-Length: 3\r\n\r\nabc", piece, r) &&
            r.done() && r.redirect() && strcmp(r.location(), "https://ia800.us.archive.org/x.zip") == 0 && body.empty());
        CHECK(run("HTTP/1.0 200 OK\r\n\r\nall of it", piece, r) && !r.done() && r.closed() && body == "all of it");
        CHECK(run("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort", piece, r) && !r.closed() && r.failed());
        CHECK(run("HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nnot found", piece, r) && r.done() && r.status() == 404 && body.empty());
    }
    // a search, as the API answers in flat form (tests/search.txt)
    std::string flat = slurp(dir + "/search.txt");
    CHECK(!flat.empty());
    zxdb::Search s;
    zxdb::Flat fl;
    fl.begin(&s);
    for (size_t i = 0; i < flat.size(); i += 7) fl.feed((const uint8_t*) flat.data() + i, flat.size() - i < 7 ? flat.size() - i : 7);
    fl.end();
    CHECK(s.n == 3 && s.total == 3);
    CHECK(strcmp(s.hits[0].id, "0002259") == 0 && strcmp(s.hits[0].title, "Green Beret") == 0 && strcmp(s.hits[0].year, "1986") == 0);
    CHECK(strcmp(s.hits[0].house, "Imagine Software Ltd") == 0 && zxdb::available(s.hits[0].avail));
    CHECK(strcmp(s.hits[0].scr, "/zxdb/sinclair/entries/0002259/GreenBeret.scr") == 0);
    CHECK(strcmp(s.hits[1].title, "Green Beret (Caf? edition)") == 0 && !zxdb::available(s.hits[1].avail) && s.hits[1].scr[0] == '\0');
    CHECK(strcmp(s.hits[2].scr, "/pub/sinclair/screens/load/g/scr/GreenBeret2.scr") == 0);
    // an entry (tests/entry.txt): its tape and snapshot files only
    flat = slurp(dir + "/entry.txt");
    zxdb::Entry e;
    fl.begin(&e);
    fl.feed((const uint8_t*) flat.data(), flat.size());
    fl.end();
    CHECK(zxdb::tidy_files(e) == 2);
    CHECK(strcmp(e.files[0].path, "/pub/sinclair/games/g/GreenBeret.tzx.zip") == 0 && strcmp(e.files[0].kind, "Tape image") == 0);
    CHECK(strcmp(e.files[1].path, "/zxdb/sinclair/entries/0002259/GreenBeret.z80.zip") == 0);
    CHECK(strcmp(e.info.title, "Green Beret") == 0 && strcmp(e.info.house, "Imagine Software Ltd") == 0 && zxdb::available(e.info.avail));
    CHECK(zxdb::file_url(e.files[1].path, out, sizeof(out)) && strcmp(out, "https://spectrumcomputing.co.uk/zxdb/sinclair/entries/0002259/GreenBeret.z80.zip") == 0);
    CHECK(zxdb::file_url(e.files[0].path, out, sizeof(out)) && strstr(out, "https://archive.org/download/World_of_Spectrum_June_2017_Mirror/World%20of%20Spectrum%20June%202017%20Mirror.zip/World%20of%20Spectrum%20June%202017%20Mirror/sinclair/games/g/GreenBeret.tzx.zip") == out);
    CHECK(zxdb::search_url("Green Beret", 1, out, sizeof(out)) && strcmp(out, "https://api.zxinfo.dk/v5/search/titles/Green%20Beret?mode=tiny&size=10&offset=1&output=flat&contenttype=SOFTWARE") == 0);
    char term[64];
    zxdb::name_to_search("Green_Beret_1986ImagineSpeedLock_2", term, sizeof(term));
    CHECK(strcmp(term, "Green Beret") == 0);
    zxdb::name_to_search("Out Run (128K)", term, sizeof(term));
    CHECK(strcmp(term, "Out Run") == 0);
    // a zip: deflated and stored members (tests/game.zip: notes.txt, then GAME.TZX deflated)
    for (const char* zipname : { "/game.zip", "/stored.zip" }) {
        PcFile zin, zout;
        CHECK(zin.open((dir + zipname).c_str(), "rb"));
        unzip::Member m;
        static uint8_t window[unzip::WINDOW];
        static unzip::Work w;
        CHECK(unzip::find(zin, m, window, sizeof(window)) && strcmp(m.name, "GAME.TZX") == 0);
        CHECK(zout.open("/tmp/nettest.out", "wb"));
        CHECK(unzip::extract(zin, m, zout, w, window));
        zout.close();
        zin.close();
        CHECK(slurp("/tmp/nettest.out") == slurp(dir + "/game.tzx"));
    }
    // a big one: past the inflater's 32 KB window (made by run.sh)
    {
        PcFile zin, zout;
        if (zin.open((dir + "/big.zip").c_str(), "rb")) {
            unzip::Member m;
            static uint8_t window[unzip::WINDOW];
            static unzip::Work w;
            CHECK(unzip::find(zin, m, window, sizeof(window)) && strcmp(m.name, "Big Game.tzx") == 0 && m.method == 8);
            CHECK(zout.open("/tmp/nettest.out", "wb"));
            CHECK(unzip::extract(zin, m, zout, w, window));
            zout.close();
            zin.close();
            CHECK(slurp("/tmp/nettest.out") == slurp(dir + "/big.tzx"));
        }
    }
    printf("%s (%d failed)\n", fails == 0 ? "all passed" : "FAILED", fails);
    return fails != 0;
}
