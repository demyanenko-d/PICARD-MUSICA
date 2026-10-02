// SPDX-License-Identifier: MIT
#include "testing.h"

#include <cstdio>
#include <vector>

#include "host_protocol_link.h"
#include "player/load/bus_byte_source.h"
#include "player/protocol/host_protocol.h"

using namespace soundsinth;
using player::protocol::HostProtocol;

namespace {

// Настоящий player::load::BusByteSource поверх HostProtocol и стороны хоста
// (testing_host::Link): крайние размеры файла и неровные шаги чтения, как у
// BinaryReader на полях переменного размера.
struct Host {
    testing_host::Link link;
    const uint8_t* data                             = nullptr;
    uint32_t size                                   = 0;
    uint32_t windows                                = 0;
    uint8_t scratch[HostProtocol::kDataBufferBytes] = {};

    static void read_window(void* user, uint32_t offset, uint16_t len, uint8_t* dst) {
        auto* self = static_cast<Host*>(user);
        ++self->windows;
        for (uint16_t i = 0; i < len; ++i)
            dst[i] = offset + i < self->size ? self->data[offset + i] : 0;
    }

    static void pump(void* user) {
        auto* self = static_cast<Host*>(user);
        self->link.p->poll();
        testing_host::host_step(self->link, self, &read_window, self->scratch);
    }
};

struct Rig {
    HostProtocol protocol;
    Host host;
    player::load::BusByteSource bus;

    Rig(const std::vector<uint8_t>& file) : bus(protocol, static_cast<uint32_t>(file.size()), &Host::pump, &host) {
        host.link.attach(protocol);
        host.data        = file.data();
        host.size        = static_cast<uint32_t>(file.size());
        const uint32_t n = host.size;
        host.link.command(HostProtocol::kHcStart, static_cast<uint8_t>(n), static_cast<uint8_t>(n >> 8), static_cast<uint8_t>(n >> 16),
                          static_cast<uint8_t>(n >> 24), /*sector=512*/ 2, 0);
    }
};

std::vector<uint8_t> make_file(uint32_t size) {
    std::vector<uint8_t> f(size);
    for (uint32_t i = 0; i < size; ++i)
        f[i] = static_cast<uint8_t>(i * 37 + 11);
    return f;
}

// Шаги 1..777 по кругу и одно чтение на весь файл: байты те же, окон не
// больше размер / 4096 + 1, медленное - только хвост, все с начала сектора.
void check_size(uint32_t size) {
    const std::vector<uint8_t> file = make_file(size);
    {
        Rig r(file);
        formats::ByteSource bs = r.bus.as_byte_source();
        std::vector<uint8_t> got(size);
        uint32_t pos = 0, step = 1, short_reads = 0;
        while (pos < size) {
            const uint32_t n = step < size - pos ? step : size - pos;
            if (bs.read(bs.self, got.data() + pos, n) != n) ++short_reads;
            pos  += n;
            step  = (step % 777) + 1;
        }
        CHECK_EQ(short_reads, 0u);
        CHECK(got == file);
        CHECK(r.host.windows <= size / HostProtocol::kDataBufferBytes + 1);
        CHECK(r.host.link.slow <= 1u);
        CHECK_EQ(r.host.link.unaligned, 0u);
    }
    {
        Rig r(file);
        formats::ByteSource bs = r.bus.as_byte_source();
        std::vector<uint8_t> got(size);
        CHECK_EQ(bs.read(bs.self, got.data(), size), size);
        CHECK(got == file);
    }
}

void test_sizes_and_uneven_steps() {
    std::printf("test_bus_byte_source_sizes_and_uneven_steps\n");
    for (const uint32_t size : {1u, 511u, 512u, 4095u, 4096u, 4097u, 1084u + 1024u + 777u})
        check_size(size);
}

// Перемотка внутри окна - без нового запроса; на конец файла - чтение 0;
// за конец - отказ.
void test_seek_edges() {
    std::printf("test_bus_byte_source_seek_edges\n");
    const std::vector<uint8_t> file = make_file(6000);
    Rig r(file);
    formats::ByteSource bs = r.bus.as_byte_source();
    uint8_t buf[16];
    CHECK_EQ(bs.read(bs.self, buf, 16), 16u);
    const uint32_t windows = r.host.windows;
    CHECK(bs.seek(bs.self, 15));
    CHECK_EQ(bs.read(bs.self, buf, 1), 1u);
    CHECK_EQ(buf[0], file[15]);
    CHECK_EQ(r.host.windows, windows);
    CHECK(bs.seek(bs.self, 6000));
    CHECK_EQ(bs.read(bs.self, buf, 1), 0u);
    CHECK(!bs.seek(bs.self, 6001));
}

} // namespace

void run_bus_byte_source_tests() {
    test_sizes_and_uneven_steps();
    test_seek_edges();
}
