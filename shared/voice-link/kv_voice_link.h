// SPDX-License-Identifier: MIT（このフォルダの LICENSE）
// VocalDress の声の受け渡し：アプリ（送り手）から OBS のプラグイン（読み手）へ声を渡す約束（形は PROTOCOL.md）。
// VERIFY-04（docs/verify/VERIFY-04.md）の試作から、製品のプラグインと共用にした（docs/design/OBS_PLUGIN_RELEASE.md）。
// ヘッダーだけの部品で、libobs にも OS にも依存しない（共有メモリを開くのは kv_voice_link_shm.h）。
// 書き手1つ・読み手1つのリング。書き手は決まった長さのブロック（既定 480 フレーム＝10 ms）ごとに、
// 中身を書いてから「書いた時刻」を置き、最後に書いた位置を release で進める。読み手は acquire で読む。
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace kv::voicelink {

constexpr std::uint32_t kMagic = 0x4C56564Bu;  // "KVVL"
constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kMaxChannels = 2;
// プラグインが、ブロックに付いた時刻に足して OBS に渡す遅れ（目標の量、2 ブロック）。
// アプリは OBS 側のズレの計算にこの値を使う（docs/design/OBS_AUTO_OFFSET.md）
constexpr std::uint64_t kPluginOffsetNs = 20'000'000;

// 送り手が決める形式。読み手はこれを共有メモリの先頭から読む
struct Format {
    std::uint32_t sampleRate = 48000;
    std::uint32_t channels = 2;
    std::uint32_t blockFrames = 480;  // 10 ms
    std::uint32_t blocks = 50;        // リングの長さ（既定 0.5 秒ぶん）

    bool valid() const {
        return sampleRate >= 8000 && sampleRate <= 384000 && channels >= 1 && channels <= kMaxChannels &&
               blockFrames >= 16 && blockFrames <= 8192 && blocks >= 4 && blocks <= 4096;
    }
};

// 共有メモリの先頭。続けて「書いた時刻」（uint64、ブロックごと）、サンプル（float、ch 交互）が並ぶ
struct Header {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t sampleRate;
    std::uint32_t channels;
    std::uint32_t blockFrames;
    std::uint32_t blocks;
    std::atomic<std::uint32_t> epoch;     // 送り手の起動ごとに変わる番号。0 は「準備中」
    std::uint32_t reserved;
    std::atomic<std::uint64_t> written;   // 書き終えたブロックの数（増え続ける）
};
// プロセスをまたいで使う atomic は、lock-free（＝ロックを持たず、CPU の原子命令だけで読み書きする）で、
// 自然な境界にそろっていること。x64 の MSVC・GCC・Clang では、64 ビットの atomic の load/store は
// そろった 8 バイトへの mov（seq_cst の store は xchg）になり、Interlocked*64 と同じくプロセス間で原子的。
// 共有メモリの先頭はページ境界なので、Header の中の位置がそろっていれば足りる
static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "共有メモリには lock-free の 64 ビット atomic が要る");
static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "共有メモリには lock-free の 32 ビット atomic が要る");
static_assert(sizeof(std::atomic<std::uint64_t>) == 8 && alignof(std::atomic<std::uint64_t>) == 8, "64 ビット atomic の大きさと境界");
static_assert(sizeof(std::atomic<std::uint32_t>) == 4, "32 ビット atomic の大きさ");
// リングのサンプルと書いた時刻も atomic（relaxed）で読み書きする。書き手が一周してきて同じ場所を書き換えても、
// C++ のデータ競合にならない（いわゆる seqlock の形。写したものが使えるかは、写したあとの確認で決める）
static_assert(std::atomic<float>::is_always_lock_free && sizeof(std::atomic<float>) == sizeof(float),
              "共有メモリのサンプルは lock-free で float と同じ大きさの atomic<float>");
static_assert(offsetof(Header, epoch) % 4 == 0 && offsetof(Header, written) % 8 == 0, "atomic の位置がそろっている");
static_assert(sizeof(Header) == 40 && sizeof(Header) % 8 == 0, "約束の版 1 の先頭は 40 バイト。続く uint64 の並びをそろえる");

inline std::size_t stampsOffset() { return sizeof(Header); }
inline std::size_t samplesOffset(const Format& f) { return stampsOffset() + sizeof(std::uint64_t) * f.blocks; }
inline std::size_t bytesFor(const Format& f) {
    return samplesOffset(f) + sizeof(float) * std::size_t(f.blocks) * f.blockFrames * f.channels;
}
// 共有メモリの大きさ（どの形式でも入る大きさ。読み手は送り手の形式を知る前に開くので、固定にする）
constexpr std::size_t kRegionBytes = 4u * 1024u * 1024u;

class Writer {
public:
    // mem は bytes 以上の大きさで、8 バイトにそろっていること。epoch は 0 以外で、起動ごとに変える
    Writer(void* mem, std::size_t bytes, const Format& f, std::uint32_t epoch) : format_(f) {
        ok_ = mem != nullptr && f.valid() && epoch != 0 && bytesFor(f) <= bytes;
        if (!ok_) return;
        header_ = static_cast<Header*>(mem);
        stamps_ = reinterpret_cast<std::atomic<std::uint64_t>*>(static_cast<char*>(mem) + stampsOffset());
        samples_ = reinterpret_cast<std::atomic<float>*>(static_cast<char*>(mem) + samplesOffset(f));
        // 読み手が途中の形式を使わないよう、まず「準備中」にしてから書き換え、最後に番号を置く
        header_->epoch.store(0, std::memory_order_release);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        header_->magic = kMagic;
        header_->version = kVersion;
        header_->sampleRate = f.sampleRate;
        header_->channels = f.channels;
        header_->blockFrames = f.blockFrames;
        header_->blocks = f.blocks;
        header_->reserved = 0;
        header_->written.store(0, std::memory_order_relaxed);
        header_->epoch.store(epoch, std::memory_order_release);
    }

    bool ok() const { return ok_; }
    const Format& format() const { return format_; }

    // ch 交互の blockFrames フレームを1ブロック書く。timeNs は書いた時刻（OBS の os_gettime_ns と同じ時計）
    void writeBlock(const float* interleaved, std::uint64_t timeNs) {
        if (!ok_) return;
        const std::uint64_t n = header_->written.load(std::memory_order_relaxed);
        const std::size_t slot = std::size_t(n % format_.blocks);
        const std::size_t perBlock = std::size_t(format_.blockFrames) * format_.channels;
        // 前のブロックの公開（written の store）を、このブロックの中身より先に読み手へ見せる。読み手が新しい中身を
        // 1つでも読んだら、そのあとの確認（acquire フェンスのあとの written）は必ずこの公開以後の値を読む
        std::atomic_thread_fence(std::memory_order_release);
        std::atomic<float>* dst = samples_ + slot * perBlock;
        for (std::size_t i = 0; i < perBlock; ++i) dst[i].store(interleaved[i], std::memory_order_relaxed);
        stamps_[slot].store(timeNs, std::memory_order_relaxed);
        header_->written.store(n + 1, std::memory_order_release);
    }

private:
    Format format_;
    bool ok_ = false;
    Header* header_ = nullptr;
    std::atomic<std::uint64_t>* stamps_ = nullptr;
    std::atomic<float>* samples_ = nullptr;
};

struct ReaderStats {
    std::uint64_t blocks = 0;     // 受け取ったブロック
    std::uint64_t overruns = 0;   // 追い越された（読むのが遅れてリングが一周した）回数
    std::uint64_t resyncs = 0;    // 送り手の起動し直し・形式の変化で合わせ直した回数
};

class Reader {
public:
    enum class Status {
        NotReady,  // 送り手がいない・準備中・約束の版が違う
        Empty,     // 新しいブロックがまだない
        Block,     // 1ブロック受け取った
    };

    // targetBlocks：合わせ直したとき、最新から何ブロック手前から読み始めるか
    Reader(const void* mem, std::size_t bytes, std::uint32_t targetBlocks = 2)
        : mem_(static_cast<const char*>(mem)), bytes_(bytes), target_(targetBlocks) {}

    // 1ブロックを out（blockFrames × channels 個以上）に写す。形式は format() で分かる。
    // 写したあとにも番号と書いた位置を確かめ、写している間に書き換えられたものは捨てる（Empty を返す）
    Status read(float* out, std::size_t outCapacity, std::uint64_t& stampNs) {
        if (mem_ == nullptr || bytes_ < sizeof(Header)) return Status::NotReady;
        const Header* h = reinterpret_cast<const Header*>(mem_);
        const std::uint32_t epoch = h->epoch.load(std::memory_order_acquire);
        if (epoch == 0 || h->magic != kMagic || h->version != kVersion) return Status::NotReady;
        if (epoch != epoch_) {
            Format f;
            f.sampleRate = h->sampleRate;
            f.channels = h->channels;
            f.blockFrames = h->blockFrames;
            f.blocks = h->blocks;
            // 形式を読んでいる間に送り手が起動し直していたら、次の回でやり直す
            if (h->epoch.load(std::memory_order_acquire) != epoch) return Status::NotReady;
            if (!f.valid() || bytesFor(f) > bytes_) return Status::NotReady;
            if (epoch_ != 0) ++stats_.resyncs;
            format_ = f;
            epoch_ = epoch;
            synced_ = false;
        }
        const std::size_t perBlock = std::size_t(format_.blockFrames) * format_.channels;
        if (outCapacity < perBlock) return Status::NotReady;

        const std::uint64_t written = h->written.load(std::memory_order_acquire);
        if (!synced_ || written < next_) {  // 書いた位置が戻った＝送り手が同じ番号で起動し直した（念のため）
            if (synced_) ++stats_.resyncs;
            next_ = written > target_ ? written - target_ : 0;
            synced_ = true;
            jump_ = true;
        }
        if (written - next_ > format_.blocks - 1) {  // 一周された。最新から目標の量だけ手前へ飛ぶ
            ++stats_.overruns;
            next_ = written - std::min<std::uint64_t>(target_, written);
            jump_ = true;
        }
        if (next_ >= written) return Status::Empty;

        const std::size_t slot = std::size_t(next_ % format_.blocks);
        const auto* stamps = reinterpret_cast<const std::atomic<std::uint64_t>*>(mem_ + stampsOffset());
        const auto* samples = reinterpret_cast<const std::atomic<float>*>(mem_ + samplesOffset(format_));
        const std::atomic<float>* src = samples + slot * perBlock;
        for (std::size_t i = 0; i < perBlock; ++i) out[i] = src[i].load(std::memory_order_relaxed);
        stampNs = stamps[slot].load(std::memory_order_relaxed);
        // 写している間に書き手がこのブロックまで一周してきたら、写したものは混ざっている。捨てて最新へ飛ぶ
        std::atomic_thread_fence(std::memory_order_acquire);
        const std::uint64_t after = h->written.load(std::memory_order_relaxed);
        if (h->epoch.load(std::memory_order_relaxed) != epoch_ || after < next_ ||
            after - next_ > format_.blocks - 1) {
            return Status::Empty;  // 次の回で合わせ直す（番号が変われば resync、一周なら overrun）
        }
        lastIndex_ = next_;
        lastWasJump_ = jump_;
        jump_ = false;
        ++next_;
        ++stats_.blocks;
        return Status::Block;
    }

    const Format& format() const { return format_; }
    const ReaderStats& stats() const { return stats_; }
    // 最後に受け取ったブロックの番号（送り手が起動してから何番目か）。サンプルの位置は lastIndex() × blockFrames
    std::uint64_t lastIndex() const { return lastIndex_; }
    // 最後に受け取ったブロックが、前に受け取ったブロックと続いていない（最初・合わせ直し・追い越しのあと）
    bool lastWasJump() const { return lastWasJump_; }
    std::uint64_t backlog() const {  // まだ読んでいないブロックの数（試験と記録用）
        if (mem_ == nullptr || !synced_) return 0;
        const std::uint64_t w = reinterpret_cast<const Header*>(mem_)->written.load(std::memory_order_acquire);
        return w > next_ ? w - next_ : 0;
    }

private:
    const char* mem_;
    std::size_t bytes_;
    std::uint32_t target_;
    std::uint32_t epoch_ = 0;
    Format format_;
    bool synced_ = false;
    std::uint64_t next_ = 0;
    bool jump_ = true;
    std::uint64_t lastIndex_ = 0;
    bool lastWasJump_ = false;
    ReaderStats stats_;
};

// OBS に渡す timestamp を、サンプルの位置から続けて作る。
// 最初のブロック（と、続いていないブロック）で「ブロックの番号 → 書いた時刻」を結び付け、以後は
// 番号 × blockFrames ÷ sampleRate で進める。送り手が書くのが揺れても、続いた音には隙間も重なりもない時刻が付く。
// 書いた時刻とのずれが maxDriftNs を超えたら（送り手が長く止まった・時計がずれた）結び付け直す
class Timeline {
public:
    explicit Timeline(std::uint64_t maxDriftNs = 50'000'000) : maxDrift_(maxDriftNs) {}

    // ブロック index（書いた時刻 stampNs）の時刻。jump は Reader::lastWasJump()
    std::uint64_t place(std::uint64_t index, std::uint64_t stampNs, bool jump, const Format& f) {
        if (!has_ || jump || index < baseIndex_ || f.sampleRate != rate_) {
            rebase(index, stampNs, f);
            return stampNs;
        }
        const std::uint64_t t = at(index, f);
        const std::uint64_t diff = t > stampNs ? t - stampNs : stampNs - t;
        if (diff > maxDrift_) {
            ++drifts_;
            rebase(index, stampNs, f);
            return stampNs;
        }
        return t;
    }
    std::uint64_t rebases() const { return rebases_; }  // 結び付けた回数（最初の1回を含む）
    std::uint64_t drifts() const { return drifts_; }    // そのうち、ずれが大きくて結び付け直した回数

private:
    void rebase(std::uint64_t index, std::uint64_t stampNs, const Format& f) {
        has_ = true;
        baseIndex_ = index;
        baseStamp_ = stampNs;
        rate_ = f.sampleRate;
        ++rebases_;
    }
    std::uint64_t at(std::uint64_t index, const Format& f) const {
        const std::uint64_t frames = (index - baseIndex_) * f.blockFrames;
        return baseStamp_ + frames / f.sampleRate * 1000000000ull + frames % f.sampleRate * 1000000000ull / f.sampleRate;
    }

    std::uint64_t maxDrift_;
    bool has_ = false;
    std::uint64_t baseIndex_ = 0, baseStamp_ = 0;
    std::uint32_t rate_ = 0;
    std::uint64_t rebases_ = 0, drifts_ = 0;
};

// 受け渡しの遅れ（ms）を集め、最小・中央値・最大を出す。プラグインが 5 秒おきにログへ書く
class DelayWindow {
public:
    void add(double ms) { values_.push_back(ms); }
    std::size_t count() const { return values_.size(); }
    struct Summary { double min = 0, median = 0, max = 0; std::size_t count = 0; };
    Summary summarize() const {
        Summary s;
        s.count = values_.size();
        if (values_.empty()) return s;
        std::vector<double> v = values_;
        std::sort(v.begin(), v.end());
        s.min = v.front();
        s.max = v.back();
        s.median = v.size() % 2 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]);
        return s;
    }
    void clear() { values_.clear(); }

private:
    std::vector<double> values_;
};

}  // namespace kv::voicelink
