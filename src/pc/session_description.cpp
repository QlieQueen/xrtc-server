#include "pc/session_description.h"

#include <charconv>
#include <memory>
#include <sstream>

#include <rtc_base/logging.h>


namespace xrtc {

const char k_media_protocol_dtls_savpf[] = "UDP/TLS/RTP/SAVPF";

// --- AudioContentDescription 构造函数 ---
AudioContentDescription::AudioContentDescription() {
    auto codec = std::make_shared<AudioCodecInfo>(111, "opus", 48000, 2);

    // add feedback param
    codec->feedback_param.push_back(FeedBackParam("transport-cc"));

    // add codec param
    codec->codec_param["minptime"] = "10";
    codec->codec_param["useinbandfec"] = "1";

    _codecs.push_back(codec);
}

// -- VideoContentDescription 构造函数 --
// 填入 H264(96) + rtx(97)
VideoContentDescription::VideoContentDescription() {
    // H264 - 96
    auto h264 = std::make_shared<VideoCodecInfo>(96, "H264", 90000);
    h264->feedback_param.push_back(FeedBackParam("goog-remb"));
    h264->feedback_param.push_back(FeedBackParam("transport-cc"));
    h264->feedback_param.push_back(FeedBackParam("ccm", "fir"));
    h264->feedback_param.push_back(FeedBackParam("nack"));
    h264->feedback_param.push_back(FeedBackParam("nack", "pli"));
    h264->codec_param["level-asymmetry-allowed"] = "1";
    h264->codec_param["packetization-mode"] = "1";
    h264->codec_param["profile-level-id"] = "42e01f";
    _codecs.push_back(h264);

    // rtx - 97 (重传使用)
    auto rtx = std::make_shared<VideoCodecInfo>(97, "rtx", 90000);
    rtx->codec_param["apt"] = std::to_string(96);
    _codecs.push_back(rtx);
}

// === ContentGroup ===
bool ContentGroup::has_content_name(const std::string& content_name) {
    for (auto& name : _content_names) {
        if (name == content_name) return true;
    }
    return false;
}

void ContentGroup::add_content_name(const std::string& content_name) {
    if (!has_content_name(content_name)) {
        _content_names.push_back(content_name);
    } 
}

// -- SessionDecription （构造函数/析构函数/基本操作） --- 
SessionDescription::SessionDescription(SdpType type) : _sdp_type(type) {}
SessionDescription::~SessionDescription() {}

std::shared_ptr<MediaContentDescription> SessionDescription::get_content(const std::string& mid) {
    for (auto& content : _contents) {
        if (mid == content->mid()) {
            return content;
        }
    }
    return nullptr;
}

void SessionDescription::add_content(std::shared_ptr<MediaContentDescription> content) {
    _contents.push_back(content);
}

void SessionDescription::add_group(const ContentGroup& group) {
    _content_groups.push_back(group);
}

std::vector<const ContentGroup*> SessionDescription::get_group_by_name(const std::string& name) const {
    std::vector<const ContentGroup*> result;
    for (const auto& group : _content_groups) {
        if (group.semantics() == name) {
            result.push_back(&group);
        }
    }
    return result;
}

// --- TransportDescription 操作 ---
static std::string connection_role_to_string(ConnectionRole role) {
    switch (role) {
        case ACTIVE:    return "active";
        case PASSIVE:   return "passive";
        case ACTPASS:   return "actpass";
        case HOLDCONN:  return "holdconn";
        default:        return "none";
    }
}

bool SessionDescription::add_transport_info(const std::string& mid,
    const IceParameters& ice_param,
    rtc::RTCCertificate* certificate)
{
    auto tdesc = std::make_shared<TransportDescription>();
    tdesc->mid = mid;
    tdesc->ice_ufrag = ice_param.ice_ufrag;
    tdesc->ice_pwd = ice_param.ice_pwd;
    if (certificate) {
        tdesc->identity_fingerprint = rtc::SSLFingerprint::CreateFromCertificate(*certificate);
        if (!tdesc->identity_fingerprint) {
            RTC_LOG(LS_WARNING) << "get fingerprint failed";
            return false;
        }
    }
    tdesc->connection_role = (_sdp_type == SdpType::k_offer) ? ACTPASS : ACTIVE;
    _transport_infos.push_back(tdesc);
    return true;
}

bool SessionDescription::add_transport_info(std::shared_ptr<TransportDescription> td) {
    _transport_infos.push_back(td);
    return true;
}

std::shared_ptr<TransportDescription> SessionDescription::get_transport_info(const std::string& mid) {
    for (auto& tdesc : _transport_infos) {
        if (tdesc->mid == mid) return tdesc;
    }
    return nullptr;
}

bool SessionDescription::is_bundle(const std::string& mid) {
    auto groups = get_group_by_name("BUNDLE");
    if (groups.empty()) {
        return false;
    }
    for (auto g : groups) {
        for (auto& name : g->content_names()) {
            if (name == mid) {
                return true;
            }
        }
    }
    return false;
}

std::string SessionDescription::get_first_bundle_mid() {
    auto groups = get_group_by_name("BUNDLE");
    if (groups.empty()) {
        return "";
    }
    return groups[0]->content_names()[0];
}

// === SDP 序列化(辅助函数) ===
// a=rtcp-fb:111 transport-cc
static void add_rtcp_fb_line(std::shared_ptr<CodecInfo> codec, std::stringstream& ss) {
    for (auto& param : codec->feedback_param) {
        ss << "a=rtcp-fb:" << codec->id << " " << param.id();
        if (!param.param().empty()) {
            ss << " " << param.param();
        }
        ss << "\r\n";
    }
}

// a=fmtp:111 minptime=10;useinbandfec=1
static void add_fmtp_line(std::shared_ptr<CodecInfo> codec,
        std::stringstream& ss)
{
    if (!codec->codec_param.empty()) {
        ss << "a=fmtp:" << codec->id << " ";
        std::string data;
        for (auto& param : codec->codec_param) {
            data += ";" + param.first + "=" + param.second;
        }
        ss << data.substr(1) << "\r\n"; // 去掉开头的 ";"
    }
}

static void build_rtp_map(std::shared_ptr<MediaContentDescription> content, std::stringstream& ss) {
    for (auto& codec : content->get_codecs()) {
        // audio -> a=rtpmap:111 opus/48000/2
        // video -> 
        ss << "a=rtpmap:" << codec->id << " " << codec->name << "/" << codec->clockrate;
        if (content->type() == MediaType::MEDIA_TYPE_AUDIO) {
            ss << "/" << codec->as_audio()->channels;
        }
        ss << "\r\n";
        //a=rtcp-fb:111 transport-cc
        add_rtcp_fb_line(codec, ss);
        //a=fmtp:111 minptime=10;useinbandfec=1
        add_fmtp_line(codec, ss);
    }
}

static void build_rtp_direction(std::shared_ptr<MediaContentDescription> content, std::stringstream& ss) {
    switch (content->direction()) {
        case RtpDirection::k_send_recv:
            ss << "a=sendrecv\r\n";
            break;
        case RtpDirection::k_recv_only:
            ss << "a=recvonly\r\n";
            break;
        case RtpDirection::k_send_only:
            ss << "a=sendonly\r\n";
            break;
        default:
            ss << "a=inactive\r\n";
            break;
    }
}

static void build_candidate(std::shared_ptr<MediaContentDescription> content,
        std::stringstream& ss)
{
    for (auto& c : content->candidates()) {
        ss << "a=candidate:" << c.foundation
           << " " << c.component_id
           << " " << c.protocol
           << " " << c.priority
           << " " << c.address
           << " " << c.port
           << " " << c.type
           << "\r\n";
    }    
}

static void add_ssrc_line(uint32_t ssrc, const std::string& attr,
        const std::string& value, std::stringstream& ss)
{
    ss << "a=ssrc:" << ssrc << " " << attr << ":" << value << "\r\n";
}


/*
a=ssrc-group:FID 1830137564 644714241
a=ssrc:1830137564 cname:62ijRRZbdrSrZLF0
a=ssrc:1830137564 msid:stream_id video_label
a=ssrc:1830137564 mslabel:stream_id
a=ssrc:1830137564 label:video_label
a=ssrc:644714241 cname:62ijRRZbdrSrZLF0
a=ssrc:644714241 msid:stream_id video_label
a=ssrc:644714241 mslabel:stream_id
a=ssrc:644714241 label:video_label
*/
static void build_ssrc(std::shared_ptr<MediaContentDescription> content,
        std::stringstream& ss)
{
    for (auto& track : content->streams()) {
        for (auto& ssrc_group : track.ssrc_groups) {
            if (ssrc_group.ssrcs.empty()) {
                continue;
            }

            ss << "a=ssrc_group:" << ssrc_group.semantics;
            for (auto ssrc : ssrc_group.ssrcs) {
                ss << " " << ssrc;
            }
            ss << "\r\n";
        }
        std::string msid = track.stream_id + " " + track.id;
        for (auto ssrc : track.ssrcs) {
            add_ssrc_line(ssrc, "cname", track.cname, ss);
            add_ssrc_line(ssrc, "msid", msid, ss);
            add_ssrc_line(ssrc, "mslabel", track.stream_id, ss);
            add_ssrc_line(ssrc, "label", track.id, ss);
        }
    }
}

// === to_string() -- 核心序列化方法 ===
std::string SessionDescription::to_string() {
    std::stringstream ss;

    /*
    v=0
    o=- 0 2 IN IP4 127.0.0.1
    s=-
    t=0 0
    */
    ss << "v=0\r\n";
    ss << "0=- 0 2 IN IP4 127.0.0.0\r\n";
    ss << "s=-\r\n";
    ss << "t=0 0\r\n";

    // BUNDLE 分组
    // a=group:BUNDLE audio video
    auto groups = get_group_by_name("BUNDLE");
    if (!groups.empty()) {
        ss << "a=group:BUNDLE";
        for (auto g : groups) {
            for (auto& name : g->content_names()) {
                ss << " " << name;
            }
        }
        ss << "\r\n";
    }

    ss << "a=msid-semantic: WMS\r\n";

    /*

    */
    for (auto& content : _contents) {
        // m= 行
        std::string fmt;
        for (auto& codec : content->get_codecs()) {
            fmt += " " + std::to_string(codec->id);
        }

        //m=audio 9 UDP/TLS/RTP/SAVPF 111
        //c=IN IP4 0.0.0.0
        //a=rtcp:9 IN IP4 0.0.0.0
        ss << "m=" << content->mid() << " 9 " << k_media_protocol_dtls_savpf << fmt << "\r\n";
        ss << "c=IN IP4 0.0.0.0\r\n";
        ss << "a=rtcp:9 IN IP4 0.0.0.0\r\n";

        //a=candidate:1975680953 1 udp 2113937151 120.76.197.143 10028 typ host
        build_candidate(content, ss);

        //a=ice-ufrag:bmOu
        //a=ice-pwd:gN7glSPNwmh1J0uo+Olrdgcd
        //a=fingerprint:sha-256 72:6D:FC:06:53:D0:64:AE:54:D2:3F:9F:6F:AC:C2:7E:7C:7C:CF:6F:CB:01:16:E2:9A:DC:0E:77:3C:0E:B6:AB
        //a=setup:actpass
        auto transport_info = get_transport_info(content->mid());
        if (transport_info) {
            ss << "a=ice-ufrag:" << transport_info->ice_ufrag << "\r\n";
            ss << "a=ice-pwd:" << transport_info->ice_pwd << "\r\n";
            if (transport_info->identity_fingerprint) {
                ss << "a=fingerprint:" << transport_info->identity_fingerprint->algorithm
                   << " " << transport_info->identity_fingerprint->GetRfc4572Fingerprint()
                   << "\r\n";
                ss << "a=setup:" << connection_role_to_string(transport_info->connection_role) << "\r\n";
            }
        }

        //a=mid:audio
        ss << "a=mid:" << content->mid() << "\r\n";
        //a=sendonly
        build_rtp_direction(content, ss);

        //a=rtcp-mux
        if (content->rtcp_mux()) {
            ss << "a=rtcp-mux\r\n";
        }

        //a=rtpmap:111 opus/48000/2
        //a=rtcp-fb:111 transport-cc
        //a=fmtp:111 minptime=10;useinbandfec=1
        build_rtp_map(content, ss);
        build_ssrc(content, ss);
    }

    return ss.str();
}

} // namespace xrtc
