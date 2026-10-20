/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "sdp_compat.h"

#include <string_view>
#include <utility>

namespace mosaico_camera_sdp {
namespace {

struct MediaSection {
    std::string type;
    std::string mid;
    std::vector<std::string> lines;
};

struct SessionDescription {
    std::vector<std::string> header;
    std::vector<MediaSection> media;
};

bool StartsWith(std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

void AppendLine(std::string &sdp, std::string_view line)
{
    sdp.append(line.data(), line.size());
    sdp.append("\r\n");
}

bool ParseSdp(const std::string &sdp, SessionDescription &parsed)
{
    parsed = {};
    for (size_t begin = 0; begin < sdp.size();) {
        size_t end = sdp.find('\n', begin);
        if (end == std::string::npos) end = sdp.size();
        size_t text_end = end;
        if (text_end > begin && sdp[text_end - 1] == '\r') --text_end;
        std::string line(sdp, begin, text_end - begin);
        if (line.find('\0') != std::string::npos) return false;
        if (StartsWith(line, "m=")) {
            size_t space = line.find(' ', 2);
            if (space == std::string::npos || space == 2) return false;
            parsed.media.push_back({line.substr(2, space - 2), {}, {}});
        }
        if (!line.empty()) {
            if (parsed.media.empty()) {
                parsed.header.push_back(std::move(line));
            } else {
                if (StartsWith(line, "a=mid:")) {
                    if (!parsed.media.back().mid.empty()) return false;
                    parsed.media.back().mid = line.substr(6);
                }
                parsed.media.back().lines.push_back(std::move(line));
            }
        }
        if (end == sdp.size()) break;
        begin = end + 1;
    }
    return !parsed.media.empty();
}

bool RewriteGroup(const std::string &line, const std::vector<OfferedMedia> &media,
                  std::string &rewritten)
{
    size_t first_space = line.find(' ');
    if (first_space == std::string::npos) return false;
    rewritten = line.substr(0, first_space);
    for (size_t begin = first_space + 1; begin < line.size();) {
        size_t end = line.find(' ', begin);
        if (end == std::string::npos) end = line.size();
        if (end > begin) {
            std::string_view mid(line.data() + begin, end - begin);
            bool found = false;
            for (size_t i = 0; i < media.size(); ++i) {
                if (mid == media[i].mid) {
                    rewritten.append(" ").append(std::to_string(i));
                    found = true;
                    break;
                }
            }
            if (!found) return false;
        }
        begin = end + 1;
    }
    return true;
}

bool RejectedMLine(const std::string &offered, std::string &rejected)
{
    size_t first_space = offered.find(' ');
    if (first_space == std::string::npos) return false;
    size_t second_space = offered.find(' ', first_space + 1);
    if (second_space == std::string::npos) return false;
    rejected = offered.substr(0, first_space + 1);
    rejected.append("0").append(offered, second_space, std::string::npos);
    return true;
}

bool IsSendMedia(const MediaSection &media)
{
    const std::string &mline = media.lines.front();
    size_t port = mline.find(' ');
    if (port == std::string::npos || mline.compare(port, 3, " 0 ") == 0) return false;
    bool has_codec = false, has_sendonly = false, has_ssrc = false;
    bool has_ice_ufrag = false, has_ice_pwd = false, has_fingerprint = false;
    for (const auto &line : media.lines) {
        if (StartsWith(line, "a=rtpmap:")) {
            has_codec |= media.type == "video" ? line.find(" H264/") != std::string::npos :
                (line.find(" opus/48000/") != std::string::npos || line.find(" OPUS/48000/") != std::string::npos);
        }
        has_sendonly |= line == "a=sendonly";
        has_ssrc |= StartsWith(line, "a=ssrc:");
        has_ice_ufrag |= StartsWith(line, "a=ice-ufrag:");
        has_ice_pwd |= StartsWith(line, "a=ice-pwd:");
        has_fingerprint |= StartsWith(line, "a=fingerprint:");
    }
    return has_codec && has_sendonly && (media.type != "video" || has_ssrc) &&
        has_ice_ufrag && has_ice_pwd && has_fingerprint;
}

} // namespace

bool NormalizeOffer(const std::string &offer, std::string &normalized,
                    std::vector<OfferedMedia> &media)
{
    SessionDescription parsed;
    if (!ParseSdp(offer, parsed) || parsed.media.size() > 3) return false;
    std::vector<OfferedMedia> layout;
    size_t video_count = 0, audio_count = 0, data_count = 0;
    for (const auto &section : parsed.media) {
        if (section.mid.empty() || section.lines.empty()) return false;
        if (section.type == "video") ++video_count;
        else if (section.type == "audio") ++audio_count;
        else if (section.type == "application") ++data_count;
        else return false;
        for (const auto &previous : layout) {
            if (previous.mid == section.mid) return false;
        }
        layout.push_back({section.type, section.mid, section.lines.front()});
    }
    if (video_count > 1 || audio_count > 1 || data_count > 1 || video_count + audio_count == 0) return false;

    std::string result;
    for (const auto &line : parsed.header) {
        if (StartsWith(line, "a=group:")) {
            std::string rewritten;
            if (!RewriteGroup(line, layout, rewritten)) return false;
            AppendLine(result, rewritten);
        } else {
            AppendLine(result, line);
        }
    }
    for (size_t i = 0; i < parsed.media.size(); ++i) {
        for (const auto &line : parsed.media[i].lines) {
            if (StartsWith(line, "a=mid:")) {
                AppendLine(result, "a=mid:" + std::to_string(i));
            } else {
                AppendLine(result, line);
            }
        }
    }
    media = std::move(layout);
    normalized = std::move(result);
    return true;
}

bool BuildAnswer(const std::string &peer_answer,
                 const std::vector<OfferedMedia> &offered_media,
                 bool send_video, bool send_audio, std::string &answer)
{
    SessionDescription parsed;
    if ((!send_video && !send_audio) || !ParseSdp(peer_answer, parsed) ||
        parsed.media.size() != offered_media.size()) return false;
    bool found_video = false, found_audio = false;
    std::vector<bool> active(offered_media.size(), false);
    std::string bundle = "a=group:BUNDLE";
    for (size_t i = 0; i < offered_media.size(); ++i) {
        if (parsed.media[i].type != offered_media[i].type ||
            parsed.media[i].mid != std::to_string(i)) return false;
        if (offered_media[i].type == "video" && send_video) {
            if (found_video) return false;
            found_video = active[i] = true;
        } else if (offered_media[i].type == "audio" && send_audio) {
            if (found_audio) return false;
            found_audio = active[i] = true;
        }
        if (active[i]) {
            if (!IsSendMedia(parsed.media[i])) return false;
            bundle.append(" ").append(offered_media[i].mid);
        }
    }
    if (found_video != send_video || found_audio != send_audio) return false;

    std::string result;
    for (const auto &line : parsed.header) {
        if (!StartsWith(line, "a=group:")) AppendLine(result, line);
    }
    AppendLine(result, bundle);
    for (size_t i = 0; i < offered_media.size(); ++i) {
        if (active[i]) {
            for (const auto &line : parsed.media[i].lines) {
                if (StartsWith(line, "a=mid:")) AppendLine(result, "a=mid:" + offered_media[i].mid);
                else if (StartsWith(line, "a=rtcp-fb:")) continue;
                else AppendLine(result, line);
            }
        } else {
            std::string rejected;
            if (!RejectedMLine(offered_media[i].mline, rejected)) return false;
            AppendLine(result, rejected);
            AppendLine(result, "c=IN IP4 0.0.0.0");
            AppendLine(result, "a=mid:" + offered_media[i].mid);
            if (offered_media[i].type == "audio") AppendLine(result, "a=inactive");
        }
    }
    answer = std::move(result);
    return true;
}

} // namespace mosaico_camera_sdp
