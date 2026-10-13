#include "Configurable.h"

#include "C2Params.h"

#include <aidl/android/hardware/media/c2/Status.h>
#include <algorithm>
#include <cstring>

using aidl::android::hardware::media::c2::FieldSupportedValuesQuery;
using aidl::android::hardware::media::c2::FieldSupportedValuesQueryResult;
using aidl::android::hardware::media::c2::IConfigurable;
using aidl::android::hardware::media::c2::ParamDescriptor;
using aidl::android::hardware::media::c2::Params;
using aidl::android::hardware::media::c2::SettingResult;
using aidl::android::hardware::media::c2::Status;
using ndk::ScopedAStatus;

namespace gki {

Configurable::Configurable(int32_t id, std::string name)
    : mId(id), mName(std::move(name)) {}

uint32_t Configurable::width() const {
    std::lock_guard<std::mutex> lock(mLock);
    return mWidth;
}
uint32_t Configurable::height() const {
    std::lock_guard<std::mutex> lock(mLock);
    return mHeight;
}
void Configurable::setSize(uint32_t w, uint32_t h) {
    std::lock_guard<std::mutex> lock(mLock);
    mWidth = w;
    mHeight = h;
}

ScopedAStatus Configurable::getId(int32_t* out) {
    *out = mId;
    return ScopedAStatus::ok();
}

ScopedAStatus Configurable::getName(std::string* out) {
    *out = mName;
    return ScopedAStatus::ok();
}

ScopedAStatus Configurable::config(const Params& inParams, bool /*mayBlock*/,
                                   IConfigurable::ConfigResult* result) {
    c2param::PictureSize pic;
    const bool parsed = c2param::parseParams(inParams.params, &pic);
    if (parsed) {
        setSize(pic.width, pic.height);
    }
    result->params = inParams;  // echo
    // Codec2 configures C2StoreDmaBufUsageInfo on the store and reads the heap
    // name back; fill it in place (the client sized the flexible name).
    auto& blob = result->params.params;
    for (size_t at = 0; at + 8 <= blob.size();) {
        uint32_t size = 0, index = 0;
        memcpy(&size, blob.data() + at, 4);
        memcpy(&index, blob.data() + at + 4, 4);
        if (size < 8 || at + size > blob.size()) break;
        if (c2param::coreIndex(index) == c2param::kParamIndexStoreDmaBufUsage && size > 24) {
            static const char kHeap[] = "system";
            const size_t room = size - 24;
            memset(blob.data() + at + 24, 0, room);
            memcpy(blob.data() + at + 24, kHeap, std::min(room - 1, sizeof(kHeap) - 1));
        }
        at += (size + 7u) & ~7u;
    }
    result->failures.clear();
    result->status.status = Status::OK;
    return ScopedAStatus::ok();
}

ScopedAStatus Configurable::query(const std::vector<int32_t>& indices, bool /*mayBlock*/,
                                  IConfigurable::QueryResult* result) {
    result->params.params.clear();
    uint32_t w = width(), h = height();
    bool returnedPicture = false;
    for (int32_t idx : indices) {
        const uint32_t uidx = static_cast<uint32_t>(idx);
        const uint32_t core = c2param::coreIndex(uidx);
        std::vector<uint8_t> blob;
        if (core == c2param::kParamIndexPictureSize) {
            blob = c2param::makePictureSize(uidx, w, h);
            returnedPicture = true;
        } else if (core == c2param::kParamIndexBufferType) {
            // Graphic input is required by the AIDL GraphicBufferSource path.
            // The output remains linear (encoded Annex-B bytes).
            const uint32_t graphic = 3;  // C2BufferData::GRAPHIC
            const uint32_t linear = 1;   // C2BufferData::LINEAR
            blob = c2param::makeU32(uidx, (uidx & 0x10000000u) ? linear : graphic);
        } else if (core == c2param::kParamIndexKind) {
            blob = c2param::makeU32(uidx, 2);  // C2Component::KIND_ENCODER
        } else if (core == c2param::kParamIndexDomain) {
            blob = c2param::makeU32(uidx, 1);  // C2Component::DOMAIN_VIDEO
        } else if (core == c2param::kParamIndexStoreDmaBufUsage) {
            blob = c2param::makeDmaBufUsage(uidx, "system");
        } else if (core == c2param::kParamIndexMediaType) {
            // Codec2's encoder input is raw video and its output is AVC.
            blob = c2param::makeString(uidx,
                    (uidx & 0x10000000u) ? "video/avc" : "video/raw");
        }
        if (!blob.empty()) {
            result->params.params.insert(result->params.params.end(), blob.begin(), blob.end());
            // Params.aidl requires each C2Param start offset to be 8-byte
            // aligned; the size field itself remains the unpadded object size.
            while ((result->params.params.size() & 7u) != 0) {
                result->params.params.push_back(0);
            }
        }
    }
    // The minimal interface does not expose the full descriptor table used by
    // CCodecConfig.  Always include the input picture-size info so surface
    // setup still receives the configured dimensions.
    if (!returnedPicture && w != 0 && h != 0) {
        auto blob = c2param::makePictureSize(0xc2001800u, w, h);
        result->params.params.insert(result->params.params.end(), blob.begin(), blob.end());
    }
    result->status.status = Status::OK;
    return ScopedAStatus::ok();
}

ScopedAStatus Configurable::querySupportedParams(int32_t /*start*/, int32_t /*count*/,
                                                 std::vector<ParamDescriptor>* out) {
    out->clear();
    ParamDescriptor picture;
    picture.index = static_cast<int32_t>(0xc2001800u);
    picture.attrib = ParamDescriptor::ATTRIBUTE_READ_ONLY;
    picture.name = "raw.size";
    out->push_back(std::move(picture));
    if (mId == 0) return ScopedAStatus::ok();  // store-level configurable
    // CCodecConfig only maps advertised params into MediaFormat keys; without
    // output.media-type the output format has no "mime" and MediaMuxer
    // rejects the track.
    static const struct { uint32_t index; const char* name; } kComponentParams[] = {
        {0xA0000804u, "component.kind"},     // C2ComponentKindSetting
        {0xA0000805u, "component.domain"},   // C2ComponentDomainSetting
        {0x8001080Cu, "input.media-type"},   // C2PortMediaTypeSetting::input
        {0x9001080Cu, "output.media-type"},  // C2PortMediaTypeSetting::output
    };
    for (const auto& param : kComponentParams) {
        ParamDescriptor d;
        d.index = static_cast<int32_t>(param.index);
        d.attrib = ParamDescriptor::ATTRIBUTE_READ_ONLY;
        d.name = param.name;
        out->push_back(std::move(d));
    }
    return ScopedAStatus::ok();
}

ScopedAStatus Configurable::querySupportedValues(
        const std::vector<FieldSupportedValuesQuery>& inFields, bool /*mayBlock*/,
        IConfigurable::QuerySupportedValuesResult* result) {
    result->values.clear();
    result->values.resize(inFields.size());
    for (size_t i = 0; i < inFields.size(); ++i) {
        result->values[i].status.status = Status::OK;
    }
    result->status.status = Status::OK;
    return ScopedAStatus::ok();
}

}  // namespace gki
