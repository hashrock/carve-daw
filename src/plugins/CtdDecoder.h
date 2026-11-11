// The acoustic drum kit's sound engine, as it came from the web-acoustic-drum
// prototype (cpp/ctd_decoder.h there), unchanged but for this note: it renders
// the same samples as that prototype's JavaScript decoder, and keeping it
// verbatim keeps the two comparable. Its own comments are in Japanese, as the
// prototype's are. AcousticDrumsPlugin.h says how the app uses it.
//
// Carve Tiny Drums デコーダ（C++17, ヘッダオンリー, 依存なし）
//
// web/ctd-decoder.js の移植。同じ seed・同じ quality なら JS 版と同じ波形になる（浮動小数の丸め差を除く）。
//
//   auto kit = ctd::Kit::parse(fileBytes);    // tiny.ctd（CTD4。トランジェントは PCM）
//
//   // 発音: ボイスごとに少しずつ合成する（ヒット全体の波形は作らない）
//   ctd::HitVoice voice;
//   voice.reserve(kit);                        // 発音前に一度（以降の start/render はメモリ確保をしない）
//   ctd::RenderOptions opts;
//   opts.quality = ctd::Quality::Mid;          // リアルタイム再生の標準。書き出し（バウンス）は Quality::Full
//   voice.start(kit, inst, hit, opts);         // ノートオン
//   voice.render(buffer, numSamples);          // オーディオコールバックごと。done() で終了
//
// JUCE では HitVoice をボイスプールとして持って processBlock から render する想定。
// 常駐メモリはキットファイル（約1.7MB）だけ。ボイス1つの作業メモリは百KB程度。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace ctd {

// ---------------------------------------------------------------- キット
struct Hit {
    int n = 0;              // 長さ[サンプル]
    double ta = 0;          // トランジェント長[s]
    double lvl = 0;         // 先頭100msのRMS（ベロシティ→ゲイン計算用）
    double scale = 0;       // トランジェント（int16）の倍率
    int64_t pcm = 0;        // トランジェントの本体内オフセット
    int64_t modes = 0;
    int nModes = 0;
    double ampTop = 0, g = 0, tau = 0.05;
    int64_t noise = 0;
    double noiseTop = 0;
    int ref = 0;
};

struct Instrument {
    std::string id, name, choke;
    double ta = 0;
    int B = 0, J = 0;
    std::vector<int> edges;
    std::vector<std::vector<Hit>> layers;   // [ベロシティ層][ラウンドロビン]
    // 倍音バンク（シンバル・ハイハットの持続音を作る楽器固有の倍音）: 周波数[Hz]、突き出し[dB]、帯域ごとの倍音の割合
    std::vector<double> partialF, partialR, partialAlpha;
};

struct Alias {              // データを持たず、別の楽器をつまみの既定値付きで鳴らす楽器（タムの追加など）
    std::string id, name, of;
    std::map<std::string, double> opts;
};

struct Kit {
    int fs = 48000, nfft = 1024, hop = 256;
    double tf = 0.008, dMin = 0.2, dMax = 200, ampStep = 0.35, noiseStep = 0.4;
    std::vector<Instrument> instruments;
    std::vector<Alias> aliases;
    std::vector<uint8_t> data;              // ファイル全体
    size_t base = 0;                        // バイナリ本体の先頭

    const Instrument* find(const std::string& id) const {
        for (auto& i : instruments) if (i.id == id) return &i;
        return nullptr;
    }

    /** CTD4 を読む（形式は web/ctd-decoder.js の readBinaryHeader を参照） */
    static Kit parse(std::vector<uint8_t> bytes) {
        if (bytes.size() < 8) throw std::runtime_error("ctd: not a CTD file");
        if (std::memcmp(bytes.data(), "CTD4", 4) != 0) throw std::runtime_error("ctd: not a CTD4 file");
        uint32_t hlen;
        std::memcpy(&hlen, bytes.data() + 4, 4);    // リトルエンディアン前提
        if (8 + (size_t)hlen > bytes.size()) throw std::runtime_error("ctd: truncated header");
        const uint8_t* p = bytes.data() + 8;
        const uint8_t* end = p + hlen;
        auto need = [&](size_t n) { if ((size_t)(end - p) < n) throw std::runtime_error("ctd: truncated header"); };
        auto u8 = [&]() { need(1); return *p++; };
        auto u16 = [&]() { need(2); uint16_t v; std::memcpy(&v, p, 2); p += 2; return v; };
        auto u32 = [&]() { need(4); uint32_t v; std::memcpy(&v, p, 4); p += 4; return v; };
        auto f32 = [&]() { need(4); float v; std::memcpy(&v, p, 4); p += 4; return (double)v; };
        auto str = [&]() { const int n = u8(); need((size_t)n); std::string s((const char*)p, (size_t)n); p += n; return s; };
        Kit k;
        k.base = 8 + hlen;
        k.fs = (int)u32(); k.nfft = u16(); k.hop = u16();
        k.tf = f32(); k.dMin = f32(); k.dMax = f32(); k.ampStep = f32(); k.noiseStep = f32();
        const int nInst = u16(), nAlias = u16();
        for (int i = 0; i < nInst; i++) {
            Instrument in;
            in.id = str(); in.name = str(); in.choke = str();
            in.ta = f32(); in.B = u16(); in.J = u16();
            for (int b = 0; b <= in.B; b++) in.edges.push_back(u16());
            const int np = u16();
            for (int q = 0; q < np; q++) { in.partialF.push_back(u16() / 2.0); in.partialR.push_back(u8() / 4.0); }
            if (np) for (int b = 0; b < in.B; b++) in.partialAlpha.push_back(u8() / 255.0);
            const int nl = u8();
            for (int l = 0; l < nl; l++) {
                std::vector<Hit> layer;
                const int nh = u8();
                for (int h = 0; h < nh; h++) {
                    Hit t;
                    t.n = (int)u32(); t.ta = (double)u32() / k.fs; t.lvl = f32(); t.scale = f32(); t.pcm = u32();
                    t.modes = u32(); t.nModes = u16(); t.ampTop = f32(); t.g = f32(); t.tau = f32();
                    t.noise = u32(); t.noiseTop = f32(); t.ref = u16();
                    layer.push_back(t);
                }
                in.layers.push_back(std::move(layer));
            }
            k.instruments.push_back(std::move(in));
        }
        for (int a = 0; a < nAlias; a++) {
            Alias al;
            al.id = str(); al.name = str(); al.of = str();
            const int no = u8();
            for (int q = 0; q < no; q++) { std::string key = str(); al.opts[key] = f32(); }
            k.aliases.push_back(std::move(al));
        }
        k.data = std::move(bytes);
        return k;
    }
};

// ---------------------------------------------------------------- DSP
struct Rng {   // mulberry32（JS版と同じ乱数列）
    uint32_t a;
    explicit Rng(uint32_t seed) : a(seed) {}
    double next() {
        a += 0x6d2b79f5u;
        uint32_t t = (a ^ (a >> 15)) * (1u | a);
        t = (t + ((t ^ (t >> 7)) * (61u | t))) ^ t;
        return (double)(t ^ (t >> 14)) / 4294967296.0;
    }
};

constexpr double PI = 3.14159265358979323846;

// 逆FFT（in-place, radix-2, 1/N 込み）
class Ifft {
public:
    explicit Ifft(int n) : n_(n), cos_(n / 2), sin_(n / 2), rev_(n) {
        for (int i = 0; i < n / 2; i++) { cos_[i] = std::cos(2 * PI * i / n); sin_[i] = std::sin(2 * PI * i / n); }
        int bits = 0;
        while ((1 << bits) < n) bits++;
        for (int i = 0; i < n; i++) { uint32_t r = 0; for (int b = 0; b < bits; b++) r |= ((i >> b) & 1u) << (bits - 1 - b); rev_[i] = r; }
    }
    int size() const { return n_; }
    void run(double* re, double* im) const {
        for (int i = 0; i < n_; i++) { int j = (int)rev_[i]; if (j > i) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); } }
        for (int size = 2; size <= n_; size <<= 1) {
            int half = size >> 1, step = n_ / size;
            for (int i = 0; i < n_; i += size)
                for (int j = 0, k = 0; j < half; j++, k += step) {
                    double wr = cos_[k], wi = sin_[k];
                    int a = i + j, b = a + half;
                    double xr = re[b] * wr - im[b] * wi, xi = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - xr; im[b] = im[a] - xi; re[a] += xr; im[a] += xi;
                }
        }
        for (int i = 0; i < n_; i++) { re[i] /= n_; im[i] /= n_; }
    }
private:
    int n_;
    std::vector<double> cos_, sin_;
    std::vector<uint32_t> rev_;
};

// 位相の表（中間版のノイズと倍音バンク）
constexpr int PH_N = 4096;
inline const std::vector<double>& phTable(bool sine) {
    static const std::vector<double> c = [] { std::vector<double> v(PH_N); for (int i = 0; i < PH_N; i++) v[i] = std::cos(2 * PI * i / PH_N); return v; }();
    static const std::vector<double> s = [] { std::vector<double> v(PH_N); for (int i = 0; i < PH_N; i++) v[i] = std::sin(2 * PI * i / PH_N); return v; }();
    return sine ? s : c;
}

enum class Quality {
    Full,   // 厳密（書き出し向け）
    Mid,    // 中間版（リアルタイム再生の標準）: グライドは時定数の6倍で共振器へ、モードは強い24本（残りはアタック区間だけ）、
            // ノイズは 512 サンプルごと・表引き位相・等比振幅・実数逆FFT・-120dB 未満は飛ばす（倍音バンクの楽器は 256 のまま）
};

struct RenderOptions {
    uint32_t seed = 1;
    bool transient = true, modes = true, noise = true;
    double jitter = 0;       // モード振幅のランダム揺らぎ[dB]
    // ---- 音作り用（すべて既定値で元音どおり）
    double decay = 1;        // 余韻の長さの倍率（アタック区間より後ろだけに効く）
    double tune = 0;         // チューニング[半音]。長さは変わらない
    double modesGain = 0;    // 胴鳴り（モード）の音量[dB]
    double noiseGain = 0;    // ノイズ成分の音量[dB]
    double attack = 0;       // アタック（実録トランジェント）の音量[dB]
    double tilt = 0;         // 明るさ[dB/oct]（1kHz基準の傾き）
    Quality quality = Quality::Full;
};

/** エイリアスの既定値を重ねる。チューニングと dB 系は足し算、減衰は掛け算（利用者のつまみはその上に効く） */
inline RenderOptions withAlias(const Alias& al, RenderOptions o) {
    for (auto& [k, v] : al.opts) {
        if (k == "tune") o.tune += v;
        else if (k == "decay") o.decay *= v;
        else if (k == "modesGain") o.modesGain += v;
        else if (k == "noiseGain") o.noiseGain += v;
        else if (k == "attack") o.attack += v;
        else if (k == "tilt") o.tilt += v;
    }
    return o;
}

// ---------------------------------------------------------------- ストリーミング合成
// 1ヒットをブロック単位で少しずつ合成する。reserve() を発音前に一度呼んでおけば、start() / render() は
// メモリ確保をしない（オーディオスレッドで呼べる）。手順は web/ctd-decoder.js の HitVoice と1対1に対応させている。
class HitVoice {
public:
    static constexpr int BLOCK = 256;
    static constexpr int MID_MODES = 24;

    /** キット中で最大のモード数・FFT長・トランジェント長・倍音数に合わせて作業領域を確保する */
    void reserve(const Kit& kit) {
        size_t K = 0, ts = 0, J = 0, B = 0, P = 0;
        for (auto& in : kit.instruments) {
            for (auto& l : in.layers) for (auto& h : l) {
                K = std::max(K, (size_t)h.nModes);
                ts = std::max(ts, (size_t)std::lround(h.ta * kit.fs) + 1);
            }
            J = std::max(J, (size_t)in.J); B = std::max(B, (size_t)in.B); P = std::max(P, in.partialF.size());
        }
        const size_t N = (size_t)kit.nfft, nbs = N / 2;
        for (auto* v : { &mR, &mR2, &mC1, &mC2, &mY1, &mY2, &mCW, &mW, &mPH, &mEnv, &energy, &perm, &hY1, &hY2, &hC1, &hC2, &hCW })
            v->reserve(K);
        mLen.reserve(K); mSw.reserve(K); iperm.reserve(K); order.reserve(K); hOn.reserve(K);
        tI.reserve(ts);
        for (auto& r : rows) r.reserve(nbs + 1);
        re.reserve(N); im.reserve(N); ring.reserve(2 * N); win.reserve(N); xt.reserve(N);
        amp.reserve(nbs); ampQ.reserve(nbs); zr.reserve(nbs); zi.reserve(nbs);
        tg.reserve(J); centers.reserve(B);
        bandOf.reserve(nbs + 1); nsc.reserve(nbs + 1);
        pSumR.reserve(B); pT.reserve(B);
        for (auto* v : { &pInc, &pPos, &pShare, &pCorr }) v->reserve(P);
        pK.reserve(P); pBand.reserve(P);
        prepareFft((int)N);
        phTable(false); phTable(true);
    }

    void start(const Kit& kit, const Instrument& inst, const Hit& hit, const RenderOptions& opts = {}) {
        mid = opts.quality == Quality::Mid;
        fs = kit.fs; N = kit.nfft; baseHop = kit.hop;
        // 中間版はノイズのフレーム間隔を倍にする。ただし倍音バンクを持つ楽器は、倍音の位相のつながりを保つため元のまま
        hop = mid && inst.partialF.empty() ? kit.hop * 2 : kit.hop;
        rnd = Rng(opts.seed);
        const double decay = opts.decay;
        tilt = opts.tilt;
        ratio = opts.tune != 0 ? std::pow(2.0, opts.tune / 12) : 1.0;
        nf = (int)std::lround(kit.tf * fs);
        naS = (int)std::lround(hit.ta * fs);                                          // 保存されているトランジェント長
        na = ratio == 1 ? naS : std::max(nf + 1, (int)std::floor(naS / ratio));       // 再生時のトランジェント長
        const double ta = ratio == 1 ? hit.ta : (double)na / fs;
        n = (decay == 1 && ratio == 1) ? hit.n
            : std::max(na + nf, std::min(12 * fs, na + (int)std::lround((hit.n - naS) * decay)));
        pos = 0;
        const uint8_t* body = kit.data.data() + kit.base;

        // ---- Modes
        nm = 0; nmKeep = 0;
        glide = false;
        if (opts.modes && hit.nModes > 0) {
            const int K = hit.nModes;
            glide = hit.g != 0; g = hit.g; tau = hit.tau;
            for (auto* v : { &mR, &mR2, &mC1, &mC2, &mY1, &mY2, &mCW, &mW, &mPH, &mEnv, &energy }) v->assign(K, 0.0);
            mLen.assign(K, 0); mSw.assign(K, 0);
            const double lnR = std::log(kit.dMax / kit.dMin);
            int k = 0;
            for (int m = 0; m < K; m++) {
                const uint8_t* o = body + hit.modes + (size_t)m * 7;
                float f32;
                std::memcpy(&f32, o, 4);
                const double f0 = f32, f = f0 * ratio;
                double adb = hit.ampTop - (255 - o[4]) * kit.ampStep;
                if (opts.jitter != 0) adb += (rnd.next() * 2 - 1) * opts.jitter;
                adb += opts.modesGain + tiltDb(f0);
                const double A = std::pow(10.0, adb / 20);
                const double d = kit.dMin * std::exp(lnR * o[5] / 255.0);
                const double ph = o[6] / 256.0 * 2 * PI;
                if (f >= fs * 0.49) continue;
                // アタック区間は元の減衰率 r、その後は decay 倍に伸縮した r2。-100dB まで減衰したら打ち切り
                const double r = std::exp(-d / fs), r2 = std::exp(-d / (decay * fs));
                const double lenA = std::ceil(std::fmax(0.0, std::log(A / 1e-5)) / d * fs) + 1;
                const int len = lenA <= na ? (int)std::fmin((double)n, lenA)
                    : (int)std::fmin((double)n, na + std::ceil(std::fmax(0.0, std::log(A / 1e-5) - d * na / fs) / d * decay * fs) + 1);
                mLen[k] = len; mSw[k] = std::min(len, na); mR[k] = r; mR2[k] = r2;
                if (mid) energy[k] = A * A / (2 * d);
                if (glide) {
                    mW[k] = 2 * PI * f; mPH[k] = ph; mEnv[k] = A;
                } else {
                    // 2次共振器の漸化式: y[n] = 2r cos(w) y[n-1] - r^2 y[n-2]
                    const double w = 2 * PI * f / fs, cw = std::cos(w);
                    mCW[k] = cw; mC1[k] = 2 * r * cw; mC2[k] = -r * r;
                    mY1[k] = A / r * std::sin(ph - w); mY2[k] = A / (r * r) * std::sin(ph - 2 * w);
                }
                k++;
            }
            nm = k; nmKeep = k;
            if (mid && k > MID_MODES) keepStrongModes(k);
            if (mid && glide) {
                // 音程の動きが落ち着いたら（時定数の6倍、残りのずれは位相で約0.01rad）共振器に切り替える
                hand = std::max(2, (int)std::ceil(6 * tau * fs));
                for (auto* v : { &hY1, &hY2, &hC1, &hC2, &hCW }) v->assign(K, 0.0);
                hOn.assign(K, 0);
            }
        }

        // ---- Transient（残差。チューニング時は同じ比率で再サンプル）
        tOn = opts.transient;
        if (tOn) {
            tM = std::min(na, n);
            aT = hit.scale * std::pow(10.0, opts.attack / 20);
            tI.resize((size_t)naS);
            std::memcpy(tI.data(), body + hit.pcm, (size_t)naS * 2);   // リトルエンディアン前提
        }

        // ---- Noise: 包絡の行は必要になったときに計算し、出力はリングバッファに重ね合わせる
        nOn = opts.noise;
        nParts = 0;
        if (nOn) {
            B = inst.B; J = inst.J; nb = N / 2;
            q = body + hit.noise;
            tg.resize((size_t)J);
            for (int jj = 0; jj < J; jj++) tg[jj] = ta + ((double)n / fs - ta) * std::pow((double)jj / (J - 1), 2.5);
            centers.resize((size_t)B);
            for (int b = 0; b < B; b++) centers[b] = (inst.edges[b] + inst.edges[b + 1] - 1) / 2.0;
            noiseTop = hit.noiseTop; noiseStep = kit.noiseStep; gN = opts.noiseGain;
            for (auto& r : rows) r.assign((size_t)nb + 1, 0.0);
            rowJ[0] = rowJ[1] = -1;
            re.assign((size_t)N, 0.0); im.assign((size_t)N, 0.0);
            ring.assign(2 * (size_t)N, 0.0); mask = 2 * N - 1;
            prepareFft(N);
            startS = na - nf;
            nextM = (int)std::fmax(0.0, std::floor((double)(startS - N / 2) / hop));
            m1 = (int)std::ceil((double)(n + N / 2) / hop);
            j = 0;
            if (!inst.partialF.empty()) setupPartials(inst);
            if (mid) {
                amp.assign((size_t)nb, 0.0); ampQ.assign((size_t)nb, 0.0); ampJ = -1; ampClamped = true;
                zr.assign((size_t)nb, 0.0); zi.assign((size_t)nb, 0.0); xt.assign((size_t)N, 0.0);
            }
        }
        fo = std::min((int)std::lround(0.03 * fs), n >> 2);
    }

    int length() const { return n; }
    bool done() const { return pos >= n; }

    /** out[0 .. count) に書き込む（加算ではなく上書き）。書いたサンプル数を返す */
    int render(float* out, int count) {
        int written = 0;
        while (written < count && pos < n) {
            const int c = std::min({ BLOCK, count - written, n - pos });
            block(c);
            for (int i = 0; i < c; i++) out[written + i] = (float)tmp[i];
            written += c;
            pos += c;
        }
        return written;
    }

private:
    double tiltDb(double f) const {
        return tilt != 0 ? std::max(-30.0, std::min(30.0, tilt * std::log2(std::max(f, 20.0) / 1000))) : 0.0;
    }

    void prepareFft(int n_) {
        if (!fft || fft->size() != n_) {
            fft = std::make_unique<Ifft>(n_); fftHalf = std::make_unique<Ifft>(n_ / 2);
            win.resize((size_t)n_);
            for (int i = 0; i < n_; i++) win[i] = 0.5 * (1 - std::cos(2 * PI * i / n_));
            twC.resize((size_t)n_ / 2); twS.resize((size_t)n_ / 2);
            for (int k = 0; k < n_ / 2; k++) { twC[k] = std::cos(2 * PI * k / n_); twS[k] = std::sin(2 * PI * k / n_); }
        }
    }

    // 中間版: エネルギーの大きい MID_MODES 本だけを最後まで鳴らす。残りはトランジェントの区間だけ鳴らして
    // トランジェントと同じ所でフェードアウトする（トランジェントは「元音 − 全モード」なので、そうしないとアタックが崩れる）
    void keepStrongModes(int k) {
        order.resize((size_t)k);
        for (int i = 0; i < k; i++) order[i] = i;
        // エネルギーの降順（同じなら番号順。JS の安定ソートと同じ並び）
        std::sort(order.begin(), order.end(), [&](int a, int b) { return energy[a] > energy[b] || (energy[a] == energy[b] && a < b); });
        std::sort(order.begin(), order.begin() + MID_MODES);
        std::sort(order.begin() + MID_MODES, order.end());
        auto permute = [&](std::vector<double>& v) { perm.assign(v.begin(), v.begin() + k); for (int i = 0; i < k; i++) v[i] = perm[order[i]]; };
        auto permuteI = [&](std::vector<int>& v) { iperm.assign(v.begin(), v.begin() + k); for (int i = 0; i < k; i++) v[i] = iperm[order[i]]; };
        permuteI(mLen); permuteI(mSw);
        for (auto* v : { &mR, &mR2, &mC1, &mC2, &mY1, &mY2, &mCW, &mW, &mPH, &mEnv }) permute(*v);
        for (int i = MID_MODES; i < k; i++) mLen[i] = std::min(mLen[i], na);
        nmKeep = MID_MODES;
    }

    // 時間点 jj のビン別dB（帯域中心間を線形補間）。2行分だけ保持する。other は今必要なもう1行
    const double* row(int jj, int other) {
        for (int s = 0; s < 2; s++) if (rowJ[s] == jj) return rows[s].data();
        const int s = rowJ[0] == other ? 1 : 0;
        double* r = rows[s].data();
        int b = 0;
        for (int k = 1; k < nb; k++) {
            const double ks = k / ratio;    // チューニング時は周波数軸をずらして読む
            if (ks > nb - 1) { r[k] = -300; continue; }
            while (b < B - 1 && centers[b + 1] <= ks) b++;
            const int v0 = q[jj * B + b], v1 = q[jj * B + (b + 1 < B ? b + 1 : B - 1)];
            double fr = b < B - 1 ? (ks - centers[b]) / (centers[b + 1] - centers[b]) : 0;
            fr = fr < 0 ? 0 : fr > 1 ? 1 : fr;
            const double v = v0 + (v1 - v0) * fr;
            r[k] = (v0 == 0 && v1 == 0) ? -300 : noiseTop - (255 - v) * noiseStep + gN + tiltDb(ks * fs / N);
        }
        rowJ[s] = jj;
        return r;
    }

    // 倍音バンク: ノイズ包絡のうち「倍音の割合」の分を、楽器固有の倍音の周波数の正弦波として鳴らす。
    // 正弦波はノイズと同じ逆FFTのフレームに、フレーム間で位相がつながるように書き込む
    void setupPartials(const Instrument& inst) {
        bandOf.assign((size_t)nb + 1, -1); nsc.assign((size_t)nb + 1, 1.0);
        for (int k = 1; k < nb; k++) {
            const double ks = k / ratio;
            int b = 0;
            while (b < B - 1 && inst.edges[b + 1] <= ks) b++;
            if (ks >= inst.edges[0] && ks < inst.edges[B]) { bandOf[k] = b; nsc[k] = std::sqrt(1 - inst.partialAlpha[b]); }
        }
        pK.clear(); pInc.clear(); pPos.clear(); pShare.clear(); pBand.clear(); pCorr.clear();
        pSumR.assign((size_t)B, 0.0); pT.assign((size_t)B, 0.0); pAlpha = inst.partialAlpha.data();
        for (size_t i = 0; i < inst.partialF.size(); i++) {
            const double f = inst.partialF[i] * ratio;
            const int k = (int)std::lround(f / fs * N);
            if (k < 1 || k >= nb || bandOf[k] < 0) continue;
            const double r = std::pow(10.0, inst.partialR[i] / 10);
            // ビンの中心からのずれ δ による目減り（約 -5.7·δ² dB）を戻す
            const double dlt = f / fs * N - k;
            pCorr.push_back(std::pow(10.0, 5.7 * dlt * dlt / 10));
            pK.push_back(k); pInc.push_back(f / fs * hop * PH_N); pPos.push_back(rnd.next() * PH_N);
            pShare.push_back(r); pBand.push_back(bandOf[k]);
            pSumR[bandOf[k]] += r;
        }
        nParts = (int)pK.size();
        for (int i = 0; i < nParts; i++) pShare[i] = pShare[i] / pSumR[pBand[i]] * pCorr[i];
        // パワーの換算: ノイズのビン振幅 a の寄与は (2/N^2)·a^2·Σw^2、正弦波（振幅 A）のフレーム値は A·N/(2·Σw)
        pPw = (2.0 / ((double)N * N)) * (3.0 * N / (8.0 * hop));
        pCA = N / (2.0 * (N / (2.0 * hop)));
    }

    // フレームのスペクトル re/im（k=1..nb-1）に倍音を足す。full=true なら共役側（N-k）にも書く
    void addPartials(bool full) {
        const auto& C = phTable(false);
        const auto& S = phTable(true);
        for (int qq = 0; qq < nParts; qq++) {
            const int b = pBand[qq];
            const double A = std::sqrt(2 * pAlpha[b] * pT[b] * pPw * pShare[qq]);
            double ps = pPos[qq] + pInc[qq];
            ps -= std::floor(ps / PH_N) * PH_N;
            pPos[qq] = ps;
            const int ix = (int)ps, k = pK[qq];
            const double c = A * pCA, xr = c * C[ix], xi = c * S[ix];
            re[k] += xr; im[k] += xi;
            if (full) { re[N - k] += xr; im[N - k] -= xi; }
        }
    }

    // ノイズの1フレーム（ランダム位相の逆FFT × 窓）をリングバッファに重ねる
    void frame(int m) {
        const double K = std::log(10.0) / 20;
        const double tc = (double)m * hop / fs;
        while (j < J - 2 && tg[j + 1] < tc) j++;
        double fr = (tc - tg[j]) / (tg[j + 1] - tg[j]);
        fr = fr < 0 ? 0 : fr > 1 ? 1 : fr;
        const double* r0 = row(j, j + 1);
        const double* r1 = row(j + 1, j);
        re[0] = im[0] = re[nb] = im[nb] = 0;
        if (nParts) {
            // 倍音バンクのある楽器: ノイズは「倍音の割合」を除いた分だけ。帯域ごとの元のパワーを数えておく
            std::fill(pT.begin(), pT.end(), 0.0);
            for (int k = 1; k < nb; k++) {
                const double a0 = std::exp(K * (r0[k] + (r1[k] - r0[k]) * fr)) / 0.75;
                const int b = bandOf[k];
                if (b >= 0) pT[b] += a0 * a0;
                const double a = a0 * nsc[k], th = 2 * PI * rnd.next();
                const double xr = a * std::cos(th), xi = a * std::sin(th);
                re[k] = xr; im[k] = xi; re[N - k] = xr; im[N - k] = -xi;
            }
            addPartials(true);
        } else {
            for (int k = 1; k < nb; k++) {
                const double a = std::exp(K * (r0[k] + (r1[k] - r0[k]) * fr)) / 0.75;
                const double th = 2 * PI * rnd.next();
                const double xr = a * std::cos(th), xi = a * std::sin(th);
                re[k] = xr; im[k] = xi; re[N - k] = xr; im[N - k] = -xi;
            }
        }
        fft->run(re.data(), im.data());
        const int s0 = m * hop - N / 2;
        const int i0 = s0 < 0 ? -s0 : 0, i1 = N < n - s0 ? N : n - s0;
        for (int i = i0; i < i1; i++) ring[(s0 + i) & mask] += re[i] * win[i];
    }

    // 実数信号の逆FFT: エルミート対称なスペクトル X[0..N/2] から長さ N の実数列を、N/2 点の複素逆FFT 1回で作る
    void irfft() {
        const int h = N / 2;
        for (int k = 0; k < h; k++) {
            const double ar = re[k], ai = im[k], br = re[h - k], bi = -im[h - k];   // b = conj(X[N/2-k])
            const double er = (ar + br) / 2, ei = (ai + bi) / 2;                     // 偶数番目のスペクトル
            const double dr = (ar - br) / 2, di = (ai - bi) / 2;
            const double orr = dr * twC[k] - di * twS[k], oi = dr * twS[k] + di * twC[k];   // 奇数番目: d × e^{+2πik/N}
            zr[k] = er - oi; zi[k] = ei + orr;
        }
        fftHalf->run(zr.data(), zi.data());
        for (int m = 0; m < h; m++) { xt[2 * m] = zr[m]; xt[2 * m + 1] = zi[m]; }
    }

    // 中間版のノイズ1フレーム: 振幅は時間点の間で等比的に変わるので、区間が変わるまでは掛け算1回で更新する
    void frameMid(int m) {
        const double K = std::log(10.0) / 20;
        const double tc = (double)m * hop / fs;
        while (j < J - 2 && tg[j + 1] < tc) j++;
        const double span = tg[j + 1] - tg[j];
        const double frRaw = (tc - tg[j]) / span;
        const bool clamped = frRaw < 0 || frRaw > 1;
        if (j != ampJ || clamped || ampClamped) {
            const double fr = std::min(1.0, std::max(0.0, frRaw)), dfr = (double)hop / fs / span;
            const double* r0 = row(j, j + 1);
            const double* r1 = row(j + 1, j);
            const double gg = (hop == 2 * baseHop ? std::sqrt(2.0) : 1.0) / 0.75;   // フレーム間隔を倍にしたときは √2 倍
            for (int k = 1; k < nb; k++) {
                amp[k] = std::exp(K * (r0[k] + (r1[k] - r0[k]) * fr)) * gg;
                ampQ[k] = clamped ? 1 : std::exp(K * (r1[k] - r0[k]) * dfr);
            }
            ampJ = j; ampClamped = clamped;
        } else {
            for (int k = 1; k < nb; k++) amp[k] *= ampQ[k];
        }
        double pw = 0;
        for (int k = 1; k < nb; k++) pw += amp[k] * amp[k];
        if (2 * pw / ((double)N * N) < 1e-12) return;   // -120dB 未満のフレームは飛ばす（乱数も引かない）
        const auto& C = phTable(false);
        const auto& S = phTable(true);
        re[0] = im[0] = re[nb] = im[nb] = 0;
        if (nParts) {
            std::fill(pT.begin(), pT.end(), 0.0);
            for (int k = 1; k < nb; k++) {
                const double a0 = amp[k];
                const int b = bandOf[k];
                if (b >= 0) pT[b] += a0 * a0;
                const double a = a0 * nsc[k];
                const int ix = (int)(rnd.next() * PH_N);
                re[k] = a * C[ix]; im[k] = a * S[ix];
            }
            addPartials(false);
        } else {
            for (int k = 1; k < nb; k++) {
                const double a = amp[k];
                const int ix = (int)(rnd.next() * PH_N);
                re[k] = a * C[ix]; im[k] = a * S[ix];
            }
        }
        irfft();
        const int s0 = m * hop - N / 2;
        const int i0 = s0 < 0 ? -s0 : 0, i1 = N < n - s0 ? N : n - s0;
        for (int i = i0; i < i1; i++) ring[(s0 + i) & mask] += xt[i] * win[i];
    }

    // 中間版のグライド: hand サンプル目までは sin、以降は共振器（直前2サンプルの値を状態として引き継ぐ）
    void glideMid(int c, int p0) {
        const int iHand = std::min(c, std::max(0, hand - p0));
        for (int i = 0; i < iHand; i++) { const double t = (double)(p0 + i) / fs; u[i] = t + g * tau * (1 - std::exp(-t / tau)); }
        for (int k = 0; k < nm; k++) {
            const int end = std::min(c, mLen[k] - p0);
            if (end <= 0) continue;
            double* dst = k < nmKeep ? tmp : tmp2;   // 途中で消すモードは別に足してフェードを掛ける
            const double w = mW[k], ph = mPH[k], r = mR[k], r2 = mR2[k];
            const int sw = mSw[k];
            int i = 0;
            const int iH = std::min(end, iHand);
            if (iH > 0) {
                double env = mEnv[k];
                for (; i < iH; i++) {
                    const double y = env * std::sin(w * u[i] + ph);
                    dst[i] += y;
                    const int s = p0 + i;
                    if (s == hand - 2) hY2[k] = y; else if (s == hand - 1) hY1[k] = y;
                    env *= s < sw ? r : r2;
                }
                mEnv[k] = env;
            }
            if (i >= end) continue;
            if (!hOn[k]) {   // 切り替え時の周波数（グライドの残りを含む瞬時周波数）で共振器を作る
                const double wh = w * (1 + g * std::exp(-(double)hand / fs / tau)) / fs, rr = hand < sw ? r : r2, cw = std::cos(wh);
                hCW[k] = cw; hC1[k] = 2 * rr * cw; hC2[k] = -rr * rr; hOn[k] = 1;
            }
            double c1 = hC1[k], c2 = hC2[k], y1 = hY1[k], y2 = hY2[k];
            const int swl = sw - p0;
            for (; i < end; i++) {
                if (i == swl && sw >= hand) { y2 *= r / r2; c1 = 2 * r2 * hCW[k]; c2 = -r2 * r2; }
                const double y = c1 * y1 + c2 * y2; dst[i] += y; y2 = y1; y1 = y;
            }
            hC1[k] = c1; hC2[k] = c2; hY1[k] = y1; hY2[k] = y2;
        }
    }

    void block(int c) {
        const int p0 = pos;
        for (int i = 0; i < c; i++) tmp[i] = 0;
        const bool cutOn = nmKeep < nm && p0 < na;
        if (cutOn) for (int i = 0; i < c; i++) tmp2[i] = 0;
        // ---- Modes（ヒット全体を一度に合成した場合と、サンプルごとの加算順が同じになるようにしている）
        if (nm) {
            if (glide && mid) {
                glideMid(c, p0);
            } else if (glide) {
                for (int i = 0; i < c; i++) { const double t = (double)(p0 + i) / fs; u[i] = t + g * tau * (1 - std::exp(-t / tau)); }
                for (int k = 0; k < nm; k++) {
                    const int end = std::min(c, mLen[k] - p0);
                    if (end <= 0) continue;
                    const double w = mW[k], ph = mPH[k], r = mR[k], r2 = mR2[k];
                    double env = mEnv[k];
                    const int iSw = std::min(end, std::max(0, mSw[k] - p0));
                    for (int i = 0; i < iSw; i++) { tmp[i] += env * std::sin(w * u[i] + ph); env *= r; }
                    for (int i = iSw; i < end; i++) { tmp[i] += env * std::sin(w * u[i] + ph); env *= r2; }
                    mEnv[k] = env;
                }
            } else {
                for (int k = 0; k < nm; k++) {
                    const int end = std::min(c, mLen[k] - p0);
                    if (end <= 0) continue;
                    double c1 = mC1[k], c2 = mC2[k], y1 = mY1[k], y2 = mY2[k];
                    const int sw = mSw[k] - p0, iSw = std::min(end, std::max(0, sw));
                    double* dst = k < nmKeep ? tmp : tmp2;   // 中間版で途中で消すモードは別に足してフェードを掛ける
                    for (int i = 0; i < iSw; i++) { const double y = c1 * y1 + c2 * y2; dst[i] += y; y2 = y1; y1 = y; }
                    if (sw >= 0 && sw < end) {   // 減衰率を切り替えても包絡が連続になるよう状態を補正
                        const double r2 = mR2[k];
                        y2 *= mR[k] / r2; c1 = 2 * r2 * mCW[k]; c2 = -r2 * r2;
                    }
                    for (int i = iSw; i < end; i++) { const double y = c1 * y1 + c2 * y2; dst[i] += y; y2 = y1; y1 = y; }
                    mC1[k] = c1; mC2[k] = c2; mY1[k] = y1; mY2[k] = y2;
                }
            }
        }
        if (cutOn) {
            const int end = std::min(c, na - p0), iF = std::min(end, std::max(0, na - nf - p0));
            for (int i = 0; i < iF; i++) tmp[i] += tmp2[i];
            for (int i = iF; i < end; i++) tmp[i] += tmp2[i] * std::cos(0.5 * PI * (p0 + i - (na - nf)) / nf);
        }
        // ---- Transient
        if (tOn && p0 < tM) {
            const int end = std::min(c, tM - p0);
            for (int i = 0; i < end; i++) {
                const int s = p0 + i;
                double v;
                if (ratio == 1) v = tI[s];
                else {
                    const double ps = s * ratio;
                    const int i0 = (int)std::floor(ps);
                    const double fr = ps - i0;
                    v = i0 + 1 < naS ? tI[i0] * (1 - fr) + tI[i0 + 1] * fr : (i0 < naS ? tI[i0] * (1 - fr) : 0.0);
                }
                double gg = aT;
                if (s >= na - nf) gg *= std::cos(0.5 * PI * (s - (na - nf)) / nf);
                tmp[i] += v * gg;
            }
        }
        // ---- Noise
        if (nOn) {
            // ブロックの最後のサンプルまでに始まるフレームを先に作る（リングは 2N なので、先の分を書いても溢れない）
            const int last = p0 + c - 1;
            while (nextM <= m1 && nextM * hop - N / 2 <= last) { if (mid) frameMid(nextM++); else frame(nextM++); }
            const int iS = std::min(c, std::max(0, startS - p0)), iA = std::min(c, std::max(iS, na - p0));
            for (int i = 0; i < iS; i++) ring[(p0 + i) & mask] = 0;
            for (int i = iS; i < iA; i++) { const int s = p0 + i; tmp[i] += ring[s & mask] * std::sin(0.5 * PI * (s - startS) / nf); ring[s & mask] = 0; }
            for (int i = iA; i < c; i++) { const int s = p0 + i; tmp[i] += ring[s & mask]; ring[s & mask] = 0; }
        }
        // ---- 末尾フェード
        for (int i = std::max(0, n - fo - p0); i < c; i++) tmp[i] *= 0.5 * (1 - std::cos(PI * (n - 1 - (p0 + i)) / fo));
    }

    bool mid = false;
    int fs = 48000, N = 1024, hop = 256, baseHop = 256, nf = 0, naS = 0, na = 0, n = 0, pos = 0, fo = 0;
    double ratio = 1, tilt = 0;
    Rng rnd{ 1 };
    // Modes
    bool glide = false;
    double g = 0, tau = 0.05;
    int nm = 0, nmKeep = 0, hand = 0;
    std::vector<int> mLen, mSw, iperm, order;
    std::vector<double> mR, mR2, mC1, mC2, mY1, mY2, mCW, mW, mPH, mEnv, energy, perm;
    std::vector<double> hY1, hY2, hC1, hC2, hCW;
    std::vector<uint8_t> hOn;
    double tmp[BLOCK] = {}, tmp2[BLOCK] = {}, u[BLOCK] = {};
    // Transient
    bool tOn = false;
    int tM = 0;
    double aT = 0;
    std::vector<int16_t> tI;
    // Noise
    bool nOn = false;
    int B = 0, J = 0, nb = 0, mask = 0, startS = 0, nextM = 0, m1 = 0, j = 0;
    const uint8_t* q = nullptr;
    double noiseTop = 0, noiseStep = 0, gN = 0;
    std::vector<double> tg, centers, re, im, ring, win, twC, twS;
    std::vector<double> rows[2];
    int rowJ[2] = { -1, -1 };
    std::unique_ptr<Ifft> fft, fftHalf;
    // 中間版のノイズ
    std::vector<double> amp, ampQ, zr, zi, xt;
    int ampJ = -1;
    bool ampClamped = true;
    // 倍音バンク
    int nParts = 0;
    std::vector<int> bandOf, pK, pBand;
    std::vector<double> nsc, pInc, pPos, pShare, pSumR, pT, pCorr;
    const double* pAlpha = nullptr;
    double pPw = 0, pCA = 0;
};

/** 1ヒット全体を合成する（書き出しや検証用。発音には HitVoice を使う） */
inline std::vector<float> renderHit(const Kit& kit, const Instrument& inst, const Hit& hit, const RenderOptions& opts = {}) {
    HitVoice v;
    v.start(kit, inst, hit, opts);
    std::vector<float> out((size_t)v.length());
    v.render(out.data(), v.length());
    return out;
}

}   // namespace ctd
