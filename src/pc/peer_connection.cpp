#include "pc/peer_connection.h"

#include <vector>

#include <rtc_base/logging.h>
#include <rtc_base/string_encode.h>
#include <rtc_base/third_party/sigslot/sigslot.h>

#include "pc/peer_connection_def.h"

namespace xrtc {

// "aceive"/"passive"/"actpass" -> ConnectionRole 枚举
static ConnectionRole string_to_connection_role(const std::string& role) {
    if (role == "active") {
        return ACTIVE;
    } else if (role == "passive") {
        return PASSIVE;
    } else if (role == "actpass") {
        return ACTIVE;
    } else if (role == "holdconn") {
        return HOLDCONN;
    } else {
        return NONE;
    }
}

PeerConnection::PeerConnection(EventLoop* el, PortAllocator* allocator) : 
    _el(el),
    _transport_controller(new TransportController(el, allocator))
{
    _transport_controller->signal_candidate_allocate_done.connect(this,
        &PeerConnection::_on_candidate_allocate_done);
}

PeerConnection::~PeerConnection() {
}

int PeerConnection::init(rtc::RTCCertificate* certificate) {
    _certificate = certificate;
    _transport_controller->set_local_certificate(certificate);
    return 0;
}

// create_offer() - 自身构造SDP -> 调 TransportController -> 返回 SDP
std::string PeerConnection::create_offer(const RTCOfferAnswerOptions& options) {
    if (options.dtls_on && !_certificate) {
        RTC_LOG(LS_WARNING) << "certificate is null";
        return "";
    }

    _local_desc = std::make_unique<SessionDescription>(SdpType::k_offer);

    IceParameters ice_params = IceCredentials::create_random_ice_credentials();

    if (options.recv_audio) {
        auto audio = std::make_shared<AudioContentDescription>();
        audio->set_direction(RtpDirection::k_recv_only);
        audio->set_rtcp_mux(options.use_rtp_mux);
        _local_desc->add_content(audio);
        _local_desc->add_transport_info(audio->mid(), ice_params, _certificate);
    } 

    if (options.recv_video) {
        auto video = std::make_shared<VideoContentDescription>();
        video->set_direction(RtpDirection::k_recv_only);
        video->set_rtcp_mux(options.use_rtp_mux);
        _local_desc->add_content(video);
        _local_desc->add_transport_info(video->mid(), ice_params, _certificate);
    } 

    if (options.use_rtp_mux) {
        ContentGroup bundle_group("BUNDLE");
        for (auto& content : _local_desc->contents()) {
            bundle_group.add_content_name(content->mid());
        }

        if (!bundle_group.content_names().empty()) {
            _local_desc->add_group(bundle_group);
        }
    }

    _transport_controller->set_local_description(_local_desc.get());

    return _local_desc->to_string();
}

int PeerConnection::set_remote_sdp(const std::string& sdp) {
    // 1. 按\n分割
    std::vector<std::string> fields;
    size_t size = rtc::tokenize(sdp, '\n', &fields);
    if (size <= 0) {
        RTC_LOG(LS_WARNING) << "Invalid remote sdp";
        return -1;
    }

    // 2.检测\r\n，有则strip 行尾的 \r
    bool is_rn = false;
    if (sdp.find("\r\n") != std::string::npos) {
        is_rn = true;
    }

    // 3. 创建 _remote_desc
    _remote_desc = std::make_unique<SessionDescription>(SdpType::k_answer);
    std::string media_type;

    // 4. 预创建 content
    auto audio_content = std::make_shared<AudioContentDescription>();
    auto video_content = std::make_shared<VideoContentDescription>();

    // 5. 逐行处理
    for (auto field : fields) {
        if (is_rn) {
            field = field.substr(0, field.length() - 1); // strip \r
        }

        // a=group:BUNDLE audio video
        if (field.find("a=group:BUNDLE") != std::string::npos) {
            std::vector<std::string> items;
            rtc::split(field, ' ', &items);
            if (items.size() > 1) {
                ContentGroup answer_bundle("BUNDLE");
                // 从 i = 1开始，i = 0 存储的是a=group::BUNDLE
                for (size_t i = 1; i < items.size(); ++i) {
                    answer_bundle.add_content_name(items[i]);
                }
                _remote_desc->add_group(answer_bundle);
            }
        } else if (field.find("m=") != std::string::npos) {
            // m=audio 9 UDP/TLS/RTP/SAVPF 111
            std::vector<std::string> items;
            rtc::split(field, ' ', &items);
            if (items.size() <= 2) {
                return -1;
            }

            media_type = items[0].substr(2); // 从 m=audio 中取出 audio
            if ("audio" == media_type) {
                _remote_desc->add_content(audio_content);
            } else if ("video" == media_type) {
                _remote_desc->add_content(video_content);
            } else {
                RTC_LOG(LS_WARNING) << "Invalid remote sdp, has invalid media type: " << media_type;    
            }
        }

    }


    return 0;
}

// 信号回调 -- 把 candidate填入到 _local-desc
void PeerConnection::_on_candidate_allocate_done(TransportController*,
        const std::string& transport_name,
        IceCandidateComponent component,
        const std::vector<Candidate>& candidates)
{
    if (!_local_desc) {
        return;
    }

    auto content = _local_desc->get_content(transport_name);
    if (content) {
        content->add_candidates(candidates);
    }
}

} // namespace xrtc
