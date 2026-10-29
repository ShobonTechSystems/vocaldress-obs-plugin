// SPDX-License-Identifier: GPL-2.0-or-later
// VocalDress の声：Linux の libobs でプラグインを読み込み、共有メモリに書いた声が OBS のソースから出てくるかを見る試し。
//   vocaldress_voice_smoke <プラグインの .so>
// 自分で書き手になり、1 kHz・振幅 0.5 の音を 10 ms ごとに 2 秒書く。届いたフレームの数と最大の振幅を確かめる。
// 共有メモリは本番の名前を使うので、アプリが動いている PC では動かさない。Windows の OBS の代わりにはならない。
#include <obs.h>

#include "kv_voice_link_shm.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace {
std::atomic<unsigned long long> frames{0};
std::atomic<int> peakMilli{0};

void onAudio(void*, obs_source_t*, const audio_data* a, bool muted) {
    if (muted) return;
    frames += a->frames;
    const float* l = reinterpret_cast<const float*>(a->data[0]);
    int p = peakMilli.load();
    for (uint32_t i = 0; l != nullptr && i < a->frames; ++i) p = std::max(p, int(std::fabs(l[i]) * 1000));
    peakMilli.store(p);
}

int fail(const char* what) {
    std::fprintf(stderr, "失敗: %s\n", what);
    return 1;
}
}  // namespace

int main(int argc, char** argv) {
    using namespace kv::voicelink;
    if (argc < 2) { std::fprintf(stderr, "使い方: vocaldress_voice_smoke <plugin.so>\n"); return 2; }

    SharedRegion region;
    if (!region.create()) return fail("共有メモリを作れない");
    Format f;  // 48 kHz・2 ch・480 フレーム
    Writer writer(region.data(), region.size(), f, std::uint32_t(nowNs() % 1000000 + 1));
    if (!writer.ok()) return fail("書き手を作れない");

    if (!obs_startup("en-US", nullptr, nullptr)) return fail("obs_startup");
    obs_audio_info ai = {48000, SPEAKERS_STEREO};
    if (!obs_reset_audio(&ai)) return fail("obs_reset_audio");
    obs_module_t* m = nullptr;
    if (obs_open_module(&m, argv[1], nullptr) != MODULE_SUCCESS || !obs_init_module(m)) return fail("プラグインを読み込めない");
    const char* name = obs_source_get_display_name("vocaldress_voice");
    if (name == nullptr || std::strcmp(name, "VocalDress の声") != 0) return fail("ソースの表示名が違う");
    obs_source_t* s = obs_source_create("vocaldress_voice", "voice", nullptr, nullptr);
    if (s == nullptr) return fail("ソースを作れない");
    obs_set_output_source(0, s);
    obs_source_add_audio_capture_callback(s, onAudio, nullptr);

    // 10 ms ごとに 1 ブロック書く（2 秒、200 ブロック）
    std::vector<float> block(std::size_t(f.blockFrames) * f.channels);
    const double step = 2.0 * 3.14159265358979323846 * 1000.0 / f.sampleRate;
    double phase = 0.0;
    const auto start = std::chrono::steady_clock::now();
    for (int b = 0; b < 200; ++b) {
        for (std::uint32_t i = 0; i < f.blockFrames; ++i) {
            const float v = float(0.5 * std::sin(phase));
            phase += step;
            block[i * 2] = v;
            block[i * 2 + 1] = v;
        }
        writer.writeBlock(block.data(), nowNs());
        std::this_thread::sleep_until(start + std::chrono::milliseconds(10 * (b + 1)));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    obs_source_remove_audio_capture_callback(s, onAudio, nullptr);
    obs_set_output_source(0, nullptr);
    obs_source_release(s);
    obs_shutdown();
    region.close();
    SharedRegion::remove(SharedRegion::kDefaultName);

    const double peak = peakMilli.load() / 1000.0;
    std::printf("届いたフレーム %llu（書いたのは 96000）、最大の振幅 %.3f\n", frames.load(), peak);
    // 開き直しは 500 ms おきなので、最初の最大 0.5 秒ぶんは届かなくてよい。半分（1 秒ぶん）届けば合格
    if (frames.load() < 48000) return fail("届いた量が少ない");
    if (peak < 0.45 || peak > 0.55) return fail("振幅が書いたものと違う");
    std::printf("合格\n");
    return 0;
}
