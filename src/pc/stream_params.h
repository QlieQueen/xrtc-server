#ifndef __PC_STREAM_PARAMS_H_
#define __PC_STREAM_PARAMS_H_

#include <vector>
#include <string>
#include <stdint.h>

namespace xrtc {

struct SsrcGroup {
    SsrcGroup(const std::string& semantics, const std::vector<uint32_t>& ssrcs);

    std::string semantics;
    std::vector<uint32_t> ssrcs;
};

/*
StreamParams           ← 一个 track（例如一个音频流或一个视频流）
  ├── ssrcs[]          ← 该 track 的所有 SSRC 值（主 SSRC）  
  ├── ssrc_groups[]    ← SSRC 分组关系（如 FID = 主流 + 重传流）
  │     └── semantics + ssrcs[]   
  ├── cname            ← CNAME 标识(Canonical Name)
  ├── id               ← track id
  └── stream_id        ← 媒体流 id (msid) 

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

struct StreamParams {
    bool has_ssrc(uint32_t ssrc) const;

    std::string id;
    std::vector<uint32_t> ssrcs;
    std::vector<SsrcGroup> ssrc_groups;
    std::string cname;
    std::string stream_id;
};

} // namespace xrtc


#endif // __PC_STREAM_PARAMS_H_