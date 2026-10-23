// This file is part of PicoMenu (picoFace), GNU GPL v3 or later.
// The .z80 to .tap converter (pico/z80tap.h) on a PC: z80tap_host game.z80 game.tap
// Build: g++ -std=c++17 -O1 -I../../pico z80tap_host.cpp -o z80tap_host
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include "snapload.h"
#include "z80tap.h"

struct HostFile {
    FILE* f;
    int read(void* p, uint32_t n) { return (int) fread(p, 1, n, f); }
};
struct HostSink {
    FILE* f;
    bool write(const void* p, size_t n) { return fwrite(p, 1, n, f) == n; }
};

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s game.z80 game.tap [name]\n", argv[0]); return 2; }
    FILE* in = fopen(argv[1], "rb");
    if (!in) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    fseek(in, 0, SEEK_END); long size = ftell(in); fseek(in, 0, SEEK_SET);
    std::vector<uint8_t> buf(snapload::BUFFER128_SIZE), io(4096);
    HostFile hf{ in };
    snapload::Kind k = snapload::load_z80(hf, (int32_t) size, buf.data(), io.data(), (uint32_t) io.size());
    fclose(in);
    if (k == snapload::KIND_ERROR) { fprintf(stderr, "not a .z80\n"); return 1; }
    z80tap::Snap s{};
    s.hdr = buf.data();
    s.is128 = k == snapload::KIND_PAGED;
    if (s.is128) for (int b = 0; b < 8; b++) s.bank[b] = snapload::bank_memory(buf.data(), b);
    else s.ram48 = buf.data() + 30;
    std::vector<uint8_t> lc(z80tap::LC_SIZE);
    static z80tap::Plan plan;
    FILE* out = fopen(argv[2], "wb");
    HostSink sink{ out };
    const char* why = z80tap::write_tap(sink, s, argc > 3 ? argv[3] : "game", lc.data(), plan);
    fclose(out);
    if (why) { fprintf(stderr, "not converted: %s\n", why); return 1; }
    int loads = 0, fills = 0, pages = 0;
    for (int i = 0; i < plan.count; i++) { int t = plan.ops[i].type; loads += t == 1; fills += t == 2; pages += t == 3; }
    printf("%s: %s, loader at %04X, stub at %04X, %d loads, %d fills, %d pages\n", argv[2], s.is128 ? "128K" : "48K",
           plan.r, plan.stub, loads, fills, pages);
    return 0;
}
