#include "ve/assets/AudioCook.hpp"

#include "ve/core/FileSystem.hpp"
#include "ve/core/Log.hpp"

#include <miniaudio.h>
#include <vorbis/vorbisenc.h>
#define OV_EXCLUDE_STATIC_CALLBACKS // (vorbisfile.h's static callback tables: unused here)
#include <vorbis/vorbisfile.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <vector>

namespace ve {

// --- Settings ------------------------------------------------------------------

std::filesystem::path AudioSettings::settingsFile(const std::filesystem::path& soundFile)
{
    std::filesystem::path p = soundFile;
    p += ".audio";
    return p;
}

AudioSettings AudioSettings::forFile(const std::filesystem::path& soundFile)
{
    AudioSettings s;
    const std::filesystem::path file = settingsFile(soundFile);
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) return s;
    std::ifstream in(file, std::ios::binary);
    const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (!j.is_object()) log::warn("Audio settings {}: not a JSON object (using defaults)", file.string());
    else s.applyJson(j);
    return s;
}

nlohmann::json AudioSettings::toJson() const
{
    return {{"compress", compress}, {"quality", quality}, {"mono", mono}, {"sampleRate", sampleRate}};
}

void AudioSettings::applyJson(const nlohmann::json& j)
{
    if (!j.is_object()) return;
    if (auto it = j.find("compress"); it != j.end() && it->is_boolean()) compress = it->get<bool>();
    if (auto it = j.find("quality"); it != j.end() && it->is_number()) quality = std::clamp(it->get<float>(), -0.1f, 1.0f);
    if (auto it = j.find("mono"); it != j.end() && it->is_boolean()) mono = it->get<bool>();
    if (auto it = j.find("sampleRate"); it != j.end() && it->is_number()) sampleRate = uint32_t(std::max(0, it->get<int>()));
}

uint64_t AudioSettings::cookHash() const
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (8 * i)) & 0xFF;
            h *= 1099511628211ull;
        }
    };
    mix(compress);
    mix(uint64_t(std::lround(quality * 1000.0f) + 1000));
    mix(mono);
    mix(sampleRate);
    return h;
}

bool isCookableAudio(const std::filesystem::path& file)
{
    std::string ext = file.extension().string();
    for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".wav" || ext == ".flac";
}

std::filesystem::path cookedAudioPath(const std::filesystem::path& cacheDir, const std::string& assetPath)
{
    return cacheDir / std::filesystem::path(assetPath + ".ogg");
}

// --- Cooking -------------------------------------------------------------------

AudioCookResult cookAudio(const std::filesystem::path& source, const AudioSettings& settings,
                          const std::filesystem::path& output, const std::atomic<bool>* cancelled)
{
    const auto t0 = std::chrono::steady_clock::now();
    AudioCookResult r;
    if (!isCookableAudio(source)) {
        r.error = "not a cookable sound";
        return r;
    }
    const nlohmann::json stamp = makeCookStamp(source, kAudioCookerVersion, settings.cookHash(), settings.toJson());

    // Decode (miniaudio: WAV / FLAC), downmixed / resampled as asked, as float.
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, settings.mono ? 1 : 0, settings.sampleRate);
    ma_decoder decoder;
    if (ma_decoder_init_file(source.string().c_str(), &config, &decoder) != MA_SUCCESS) {
        r.error = "can't decode";
        return r;
    }
    ma_format format;
    ma_uint32 channels = 0, rate = 0;
    ma_decoder_get_data_format(&decoder, &format, &channels, &rate, nullptr, 0);

    // Encode (libvorbis, VBR) into Ogg pages.
    vorbis_info vi;
    vorbis_info_init(&vi);
    if (vorbis_encode_init_vbr(&vi, long(channels), long(rate), settings.quality) != 0) {
        vorbis_info_clear(&vi);
        ma_decoder_uninit(&decoder);
        r.error = "the Vorbis encoder doesn't support " + std::to_string(channels) + " channels at " + std::to_string(rate) + " Hz";
        return r;
    }
    vorbis_comment vc;
    vorbis_comment_init(&vc);
    vorbis_comment_add_tag(&vc, "ENCODER", "vulkan-engine audio cooker");
    const std::string stampText = stamp.dump();
    vorbis_comment_add_tag(&vc, "VE_COOK", stampText.c_str());
    vorbis_dsp_state vd;
    vorbis_block vb;
    vorbis_analysis_init(&vd, &vi);
    vorbis_block_init(&vd, &vb);
    ogg_stream_state os;
    ogg_stream_init(&os, int(std::hash<std::string>{}(source.string()) & 0x7FFFFFFF)); // any serial number

    std::vector<unsigned char> file;
    auto writePage = [&](const ogg_page& page) {
        file.insert(file.end(), page.header, page.header + page.header_len);
        file.insert(file.end(), page.body, page.body + page.body_len);
    };
    ogg_packet header, headerComment, headerCode;
    vorbis_analysis_headerout(&vd, &vc, &header, &headerComment, &headerCode);
    ogg_stream_packetin(&os, &header);
    ogg_stream_packetin(&os, &headerComment);
    ogg_stream_packetin(&os, &headerCode);
    ogg_page page;
    while (ogg_stream_flush(&os, &page) != 0) writePage(page); // the audio starts on a fresh page

    constexpr ma_uint64 kChunk = 4096;
    std::vector<float> pcm(size_t(kChunk) * channels);
    uint64_t frames = 0;
    bool eos = false, aborted = false;
    while (!eos) {
        if (cancelled && cancelled->load(std::memory_order_relaxed)) {
            aborted = true;
            break;
        }
        ma_uint64 got = 0;
        ma_decoder_read_pcm_frames(&decoder, pcm.data(), kChunk, &got);
        if (got == 0) {
            vorbis_analysis_wrote(&vd, 0); // end of stream
        } else {
            float** buffer = vorbis_analysis_buffer(&vd, int(got));
            for (ma_uint64 i = 0; i < got; ++i)
                for (ma_uint32 c = 0; c < channels; ++c) buffer[c][i] = pcm[size_t(i) * channels + c];
            vorbis_analysis_wrote(&vd, int(got));
            frames += got;
        }
        ogg_packet packet;
        while (vorbis_analysis_blockout(&vd, &vb) == 1) {
            vorbis_analysis(&vb, nullptr);
            vorbis_bitrate_addblock(&vb);
            while (vorbis_bitrate_flushpacket(&vd, &packet)) {
                ogg_stream_packetin(&os, &packet);
                while (ogg_stream_pageout(&os, &page) != 0) {
                    writePage(page);
                    if (ogg_page_eos(&page)) eos = true;
                }
            }
        }
    }
    ogg_stream_clear(&os);
    vorbis_block_clear(&vb);
    vorbis_dsp_clear(&vd);
    vorbis_comment_clear(&vc);
    vorbis_info_clear(&vi);
    ma_decoder_uninit(&decoder);
    if (aborted) {
        r.error = "cancelled";
        return r;
    }
    if (!writeFileReplacing(output, file.data(), file.size(), &r.error)) return r;

    r.ok = true;
    r.channels = channels;
    r.sampleRate = rate;
    r.seconds = rate ? double(frames) / rate : 0.0;
    r.bytes = file.size();
    r.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

CookedState checkCookedAudio(const std::filesystem::path& cooked, const std::filesystem::path& source,
                             const AudioSettings* settings, AudioSettings* settingsOut)
{
    // The stamp is the VE_COOK tag of the Vorbis comment header (the start of
    // the file). Read through the virtual file system: also from a packaged
    // game's archive.
    std::unique_ptr<std::istream> in = vfs::open(cooked);
    if (!in) return CookedState::Unusable;
    ov_callbacks callbacks{};
    callbacks.read_func = [](void* ptr, size_t size, size_t count, void* source) -> size_t {
        auto* s = static_cast<std::istream*>(source);
        s->read(static_cast<char*>(ptr), std::streamsize(size * count));
        return size ? size_t(s->gcount()) / size : 0;
    };
    callbacks.seek_func = [](void* source, ogg_int64_t offset, int whence) -> int {
        auto* s = static_cast<std::istream*>(source);
        s->clear();
        s->seekg(std::streamoff(offset), whence == SEEK_SET ? std::ios::beg : whence == SEEK_CUR ? std::ios::cur : std::ios::end);
        return s->fail() ? -1 : 0;
    };
    callbacks.close_func = nullptr; // (the stream is ours)
    callbacks.tell_func = [](void* source) -> long { return long(static_cast<std::istream*>(source)->tellg()); };
    OggVorbis_File vf;
    if (ov_open_callbacks(in.get(), &vf, nullptr, 0, callbacks) != 0) return CookedState::Unusable;
    const char* text = vorbis_comment_query(ov_comment(&vf, -1), "VE_COOK", 0);
    const nlohmann::json stamp = text ? nlohmann::json::parse(text, nullptr, false) : nlohmann::json();
    ov_clear(&vf);
    if (!stamp.is_object()) return CookedState::Unusable;
    if (settingsOut) settingsOut->applyJson(stamp.value("settings", nlohmann::json::object()));
    const uint64_t hash = settings ? settings->cookHash() : 0;
    return checkCookStamp(stamp, source, kAudioCookerVersion, settings ? &hash : nullptr);
}

} // namespace ve
