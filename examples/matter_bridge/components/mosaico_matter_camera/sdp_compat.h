/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <string>
#include <vector>

namespace mosaico_camera_sdp {

struct OfferedMedia {
    std::string type;
    std::string mid;
    std::string mline;
};

// esp_peer 1.5.x expects numeric MIDs. Keep the original layout for the
// Answer sent back to a controller that offered named MIDs.
bool NormalizeOffer(const std::string &offer, std::string &normalized,
                    std::vector<OfferedMedia> &media);

// Return an Answer for the selected media in the original Offer's order.
// Unselected sections remain present with port zero.
bool BuildAnswer(const std::string &peer_answer,
                 const std::vector<OfferedMedia> &offered_media,
                 bool send_video, bool send_audio, std::string &answer);

} // namespace mosaico_camera_sdp
