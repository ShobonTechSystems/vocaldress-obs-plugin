// SPDX-License-Identifier: GPL-2.0-or-later
// VocalDress の声（OBS の音声ソース）
// Copyright (C) 2026 Shobon Tech Systems
//
// This program is free software; you can redistribute it and/or modify it under the terms of the GNU General
// Public License as published by the Free Software Foundation; either version 2 of the License, or (at your
// option) any later version. This program is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License (LICENSE in this folder) for more details.
//
// VocalDress のアプリが共有メモリ（shared/voice-link、MIT）に書いた声を読み、obs_source_output_audio で OBS へ出す。
// VERIFY-04 の試作（probes/verify04/obs-plugin）を製品にしたもの（docs/design/OBS_PLUGIN_RELEASE.md）。
// 専用のスレッドが 5 ms おきに起き、たまったブロックをすべて出す。各ブロックの timestamp は、最初のブロックを
// 送り手が書いた時刻に結び付け、以後はサンプルの位置から続けて作り（kv::voicelink::Timeline）、決まった遅れ（20 ms）を足す。
// OBS のログには、送り手とつながった・途切れた・戻ったときと、1 分おきの要約だけを書く。
// このプラグインが読むのは libobs と shared/voice-link だけ（アプリのほかのコードは入れない。docs/ARCHITECTURE.md）。
#include <obs-module.h>
#include <util/platform.h>

#include "kv_voice_link_shm.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <timeapi.h>
#endif

#ifndef VOCALDRESS_VOICE_VERSION
#define VOCALDRESS_VOICE_VERSION "0.0.0"
#endif

OBS_DECLARE_MODULE()

namespace {

using namespace kv::voicelink;

constexpr const char* kSourceId = "vocaldress_voice";
constexpr std::uint64_t kOffsetNs = kv::voicelink::kPluginOffsetNs;  // 書いた時刻に足す遅れ（目標の量、2 ブロック）
constexpr std::uint32_t kTargetBlocks = 2;
constexpr std::uint64_t kSummaryNs = 60'000'000'000;  // 要約をログに書く間隔
constexpr std::uint64_t kGapNs = 1'000'000'000;       // これだけ受け取らなければ「途切れた」
constexpr std::uint64_t kReopenNs = 500'000'000;      // 送り手がいないとき、開き直しを試す間隔

struct Voice {
    obs_source_t* source = nullptr;
    std::atomic<bool> stop{false};
    std::thread worker;

    void run();
};

void Voice::run() {
#ifdef _WIN32
    timeBeginPeriod(1);
#endif
    SharedRegion region;
    std::unique_ptr<Reader> reader;
    std::vector<float> block(kMaxChannels * 8192);
    DelayWindow delays;
    Timeline timeline;
    std::uint64_t lastOpenTry = 0, lastSummary = os_gettime_ns(), lastBlockAt = 0;
    std::uint64_t late = 0, blocksInWindow = 0, reportedRebases = 0;
    ReaderStats reported;
    bool announcedWaiting = false, receiving = false;

    while (!stop.load()) {
        const std::uint64_t now = os_gettime_ns();
        if (!reader && now - lastOpenTry >= kReopenNs) {
            lastOpenTry = now;
            if (region.open()) {
                reader = std::make_unique<Reader>(region.data(), region.size(), kTargetBlocks);
                blog(LOG_INFO, "[vocaldress-voice] アプリとつながりました");
            } else if (!announcedWaiting) {
                blog(LOG_INFO, "[vocaldress-voice] アプリを待っています");
                announcedWaiting = true;
            }
        }
        if (reader) {
            std::uint64_t stamp = 0;
            while (reader->read(block.data(), block.size(), stamp) == Reader::Status::Block) {
                const Format& f = reader->format();
                obs_source_audio audio = {};
                audio.data[0] = reinterpret_cast<const uint8_t*>(block.data());
                audio.frames = f.blockFrames;
                audio.speakers = f.channels == 1 ? SPEAKERS_MONO : SPEAKERS_STEREO;
                audio.format = AUDIO_FORMAT_FLOAT;
                audio.samples_per_sec = f.sampleRate;
                audio.timestamp = timeline.place(reader->lastIndex(), stamp, reader->lastWasJump(), f) + kOffsetNs;
                const std::uint64_t outAt = os_gettime_ns();
                obs_source_output_audio(source, &audio);
                delays.add(double(outAt > stamp ? outAt - stamp : 0) / 1e6);
                if (outAt > audio.timestamp) ++late;  // 付けた時刻より遅く渡した（OBS 側で詰まる・飛ぶおそれ）
                ++blocksInWindow;
                lastBlockAt = outAt;
                if (!receiving) {
                    receiving = true;
                    blog(LOG_INFO, "[vocaldress-voice] 声を受け取っています");
                }
            }
        }
        const std::uint64_t t = os_gettime_ns();
        if (receiving && t - lastBlockAt >= kGapNs) {
            receiving = false;
            blog(LOG_INFO, "[vocaldress-voice] 声が途切れました（アプリが止まったか、送るのをやめた）");
        }
        if (t - lastSummary >= kSummaryNs) {
            lastSummary = t;
            if (reader && blocksInWindow > 0) {
                const ReaderStats& s = reader->stats();
                const auto sum = delays.summarize();
                blog(LOG_INFO,
                     "[vocaldress-voice] 1分: 受け取り %llu ブロック, 追い越し +%llu, 合わせ直し +%llu, 時刻の結び付け直し +%llu, "
                     "受け渡し(ms) 最小 %.2f / 中央 %.2f / 最大 %.2f, 間に合わない %llu",
                     (unsigned long long)blocksInWindow, (unsigned long long)(s.overruns - reported.overruns),
                     (unsigned long long)(s.resyncs - reported.resyncs),
                     (unsigned long long)(timeline.rebases() - reportedRebases), sum.min, sum.median, sum.max,
                     (unsigned long long)late);
                reported = s;
                reportedRebases = timeline.rebases();
            }
            delays.clear();
            late = 0;
            blocksInWindow = 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
#ifdef _WIN32
    timeEndPeriod(1);
#endif
}

const char* getName(void*) { return "VocalDress の声"; }

void* create(obs_data_t*, obs_source_t* source) {
    auto* v = new Voice;
    v->source = source;
    v->worker = std::thread([v] { v->run(); });
    return v;
}

void destroy(void* data) {
    auto* v = static_cast<Voice*>(data);
    v->stop.store(true);
    if (v->worker.joinable()) v->worker.join();
    delete v;
}

}  // namespace

MODULE_EXPORT const char* obs_module_name(void) { return "VocalDress Voice"; }
MODULE_EXPORT const char* obs_module_description(void) {
    return "Receives the processed voice from the VocalDress app as an audio source.";
}

bool obs_module_load(void) {
    obs_source_info info = {};
    info.id = kSourceId;
    info.type = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_AUDIO;
    info.get_name = getName;
    info.create = create;
    info.destroy = destroy;
    obs_register_source(&info);
    blog(LOG_INFO, "[vocaldress-voice] 版 %s を読み込みました（受け渡しの約束の版 %u）", VOCALDRESS_VOICE_VERSION, kVersion);
    return true;
}
