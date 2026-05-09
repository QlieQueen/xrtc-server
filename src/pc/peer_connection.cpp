#include "pc/peer_connection.h"

#include <vector>

#include <rtc_base/logging.h>
#include <rtc_base/string_encode.h>
#include <rtc_base/third_party/sigslot/sigslot.h>
#include <absl/algorithm/container.h>

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

// a=ice-ufrag:clientUfrag
static std::string get_attribute(const std::string& line) {
    std::vector<std::string> fields;
    size_t size = rtc::tokenize(line, ':', &fields);
    if (size != 2) {
        RTC_LOG(LS_WARNING) << "get attribute error: " << line;
        return "";
    }
    return fields[1];
}

static int parse_transport_info(TransportDescription* td,
        const std::string& line)
{
    if (line.find("a=ice-ufrag") != std::string::npos) {
        td->ice_ufrag = get_attribute(line);
        if (td->ice_ufrag.empty()) {
            return -1;
        }
    } else if (line.find("a=ice-pwd") != std::string::npos) {
        td->ice_pwd = get_attribute(line);
        if (td->ice_pwd.empty()) {
            return -1;
        }
    } else if (line.find("a=fingerprint") != std::string::npos) {
        // a=fingerprint:sha-256 4E:0A:88:FB:7F:D7:1D:13:49:8F:FF:27:EA:78:11:64:15:92:F7:B3:F3:F2:F5:89:3F:93:B3:04:14:84:0F:C2
        std::vector<std::string> items;
        rtc::tokenize(line, ' ', &items); // 分隔算法和指纹内容
        if (items.size() != 2) {
            RTC_LOG(LS_WARNING) << "parse a=fingerprint error: " << line;
            return -1;
        }

        // a=fingerprint: -> 14位
        std::string alg = items[0].substr(14); // sha-256
        absl::c_transform(alg, alg.begin(), ::tolower); // 转小写
        std::string content = items[1];

        td->identity_fingerprint = rtc::SSLFingerprint::CreateUniqueFromRfc4572(
            alg, content);
        if (!(td->identity_fingerprint.get())) {
            RTC_LOG(LS_WARNING) << "create fingerprint error: " << line;
            return -1;
        }
    }

    return 0;
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

    auto audio_td = std::make_shared<TransportDescription>();
    auto video_td = std::make_shared<TransportDescription>();

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
        
        if ("audio" == media_type) {
            if (parse_transport_info(audio_td.get(), field)) {
                return -1;
            }
        } else if ("video" == media_type) {
            if (parse_transport_info(video_td.get(), field)) {
                return -1;
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
