#ifndef __PC_CODEC_INFO_H_
#define __PC_CODEC_INFO_H_

#include <map>
#include <vector>
#include <string>

namespace xrtc {

class AudioCodecInfo;
class VideoCodecInfo;

// rtcp 控制传输质量的参数类
class FeedBackParam {
public:
    FeedBackParam(const std::string& id, const std::string& param) :
        _id(id), _param(param) {}
    explicit FeedBackParam(const std::string& id) : _id(id), _param("") {}

    std::string id() const { return _id; }
    std::string param() const { return _param; }

private:
    std::string _id;
    std::string _param;
};

// 控制编解码器质量的参数类
typedef std::map<std::string, std::string> CodecParam;

class CodecInfo {
public:
    CodecInfo(int id, const std::string& name, int clockrate) :
        id(id), name(name), clockrate(clockrate) {}
    virtual ~CodecInfo() {}

    virtual AudioCodecInfo* as_audio() { return nullptr; }
    virtual VideoCodecInfo* as_video() { return nullptr; }

public:
    int id;
    std::string name;
    int clockrate;
    std::vector<FeedBackParam> feedback_param;
    CodecParam codec_param;
};

class AudioCodecInfo : public CodecInfo {
public:
    AudioCodecInfo(int id, const std::string& name, int clockrate, int channels) :
        CodecInfo(id, name, clockrate), channels(channels) {}
    AudioCodecInfo* as_audio() override { return this; }

public:
    int channels;
};

class VideoCodecInfo : public CodecInfo {
public:
    VideoCodecInfo(int id, const std::string& name, int clockrate) :
        CodecInfo(id, name, clockrate) {}
    VideoCodecInfo* as_video() override { return this; }
};

} // namespace xrtc

#endif // __BASE_CODEC_INFO_H_