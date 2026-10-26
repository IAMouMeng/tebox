#include "ComponentStore.h"

#include "ComponentInterface.h"
#include "Configurable.h"
#include "MesaAvcComponent.h"
#include "HostDecComponent.h"
#include "TeboxAencComponent.h"
#include "mesa_pipe_video.h"

#include <aidl/android/hardware/media/c2/Status.h>
#include <aidl/android/hardware/media/c2/BnInputSurface.h>
#include <aidl/android/view/Surface.h>

#include <android/log.h>

#include <chrono>
#include <thread>

#define LOG_TAG "mesa-c2-store"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

using aidl::android::hardware::media::c2::IComponent;
using aidl::android::hardware::media::c2::IComponentInterface;
using aidl::android::hardware::media::c2::IComponentListener;
using aidl::android::hardware::media::c2::IComponentStore;
using aidl::android::hardware::media::c2::IConfigurable;
using aidl::android::hardware::media::c2::IInputSurface;
using aidl::android::hardware::media::c2::FieldDescriptor;
using aidl::android::hardware::media::c2::FieldId;
using aidl::android::hardware::media::c2::StructDescriptor;
using aidl::android::hardware::media::c2::Status;
using aidl::android::hardware::media::bufferpool2::IClientManager;
using ndk::ScopedAStatus;
using ndk::SharedRefBase;

namespace gki {

namespace {

class MesaInputSurface final : public aidl::android::hardware::media::c2::BnInputSurface {
public:
    ndk::ScopedAStatus getConfigurable(
            std::shared_ptr<aidl::android::hardware::media::c2::IConfigurable>* out) override {
        *out = nullptr;
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus getSurface(aidl::android::view::Surface* out) override {
        // The current component consumes linear AIDL blocks. Keep the input
        // surface object valid so MediaCodec can negotiate the encoder; a
        // non-zero native Surface is supplied only by a full BufferQueue path.
        out->nativeHandle = 0;
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus connect(
            const std::shared_ptr<aidl::android::hardware::media::c2::IInputSink>&,
            std::shared_ptr<aidl::android::hardware::media::c2::IInputSurfaceConnection>*)
            override {
        return ndk::ScopedAStatus::fromServiceSpecificError(Status::OMITTED);
    }
};

constexpr const char* kDecName = "c2.tebox.avc.decoder";
constexpr const char* kMime = "video/avc";

// Host hardware decoders (gfxstream): name, media type, V4L2 fourcc.
struct HostDecoder {
    const char* name;
    const char* mime;
    uint32_t codec;
};
constexpr HostDecoder kHostDecoders[] = {
    {"c2.tebox.hevc.decoder", "video/hevc", 0x43564548u /* 'HEVC' */},
    {"c2.tebox.avc.decoder", "video/avc", 0x34363248u /* 'H264' */},
};

// Host V4L2 encoders over tebox.venc (CAST_CODEC_*).
struct HostEncoder {
    const char* name;
    const char* mime;
    uint32_t castCodec;
};
constexpr HostEncoder kHostEncoders[] = {
    {"c2.tebox.avc.encoder", "video/avc", 0},
    {"c2.tebox.hevc.encoder", "video/hevc", 1},
    {"c2.tebox.vp8.encoder", "video/x-vnd.on2.vp8", 2},
    {"c2.tebox.vp9.encoder", "video/x-vnd.on2.vp9", 3},
};

// Host FFmpeg audio over tebox.aenc (CAST_AENC_*).
struct HostAudioEncoder {
    const char* name;
    const char* mime;
    uint32_t aencCodec;
};
constexpr HostAudioEncoder kHostAudioEncoders[] = {
    {"c2.tebox.aac.encoder", "audio/mp4a-latm", 0},
    {"c2.tebox.opus.encoder", "audio/opus", 1},
    {"c2.tebox.flac.encoder", "audio/flac", 2},
};

IComponentStore::ComponentTraits makeTraits(const char* name, IComponentStore::ComponentTraits::Kind kind,
                                            int rank, const char* mime = kMime) {
    IComponentStore::ComponentTraits t;
    t.name = name;
    t.domain = IComponentStore::ComponentTraits::Domain::VIDEO;
    t.kind = kind;
    t.rank = rank;
    t.mediaType = mime;
    t.aliases = {};
    return t;
}

}  // namespace

ComponentStore::ComponentStore() {
    // init starts this HAL alongside the graphics stack. Give renderD128 and
    // the guest VirGL capset a short window to appear before cataloguing.
    const mesa_video::Caps* capsPtr = nullptr;
    for (int attempt = 0; attempt < 10; ++attempt) {
        capsPtr = &mesa_video::queryCaps();
        if (capsPtr->avc_decode || capsPtr->avc_encode) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    const auto& caps = *capsPtr;
    ALOGI("VirGL caps: avc_decode=%d avc_encode=%d max=%ux%u",
          caps.avc_decode ? 1 : 0, caps.avc_encode ? 1 : 0,
          caps.max_width, caps.max_height);
    // A capset is only an advertisement. Publish a component after the
    // Gallium pipe-video bridge reports that it can create a real codec.
    if (caps.avc_decode && mesa_video::backendAvailable(mesa_video::Entrypoint::kDecode) &&
        !mCatalog.count(kDecName)) {
        ComponentDesc d;
        d.traits = makeTraits(kDecName, IComponentStore::ComponentTraits::Kind::DECODER, 16);
        d.kind = MesaCodecKind::kDecoder;
        mCatalog[kDecName] = d;
    }
    // Probe the real Gallium / host encoder; backendAvailable is authoritative.
    if (mesa_video::backendAvailable(mesa_video::Entrypoint::kEncode)) {
        for (const HostEncoder& enc : kHostEncoders) {
            ComponentDesc d;
            d.traits = makeTraits(enc.name, IComponentStore::ComponentTraits::Kind::ENCODER, 16,
                                  enc.mime);
            d.kind = MesaCodecKind::kEncoder;
            d.castCodec = enc.castCodec;
            mCatalog[enc.name] = d;
        }
        mHostEncoders = true;
    }
    addHostDecoders();
    addHostEncoders();
    ALOGI("catalog size=%zu (caps dec=%d enc=%d)", mCatalog.size(),
          caps.avc_decode ? 1 : 0, caps.avc_encode ? 1 : 0);
}

// gfxstream: the host V4L2 decoder decodes into the guest's buffers
// (tebox.vdec). Ranked ahead of the software decoders. QEMU names the
// virtio-serial ports after the guest driver is up, so the port may appear
// after this HAL starts: callers retry until it does.
void ComponentStore::addHostDecoders() {
    std::lock_guard<std::mutex> lock(mLock);
    if (mHostDecoders || !HostDecComponent::available()) return;
    for (const HostDecoder& dec : kHostDecoders) {
        ComponentDesc d;
        d.traits = makeTraits(dec.name, IComponentStore::ComponentTraits::Kind::DECODER, 16,
                              dec.mime);
        d.kind = MesaCodecKind::kDecoder;
        d.hostCodec = dec.codec;
        mCatalog[dec.name] = d;
    }
    mHostDecoders = true;
    ALOGI("host decoders available (tebox.vdec)");
}

void ComponentStore::addHostEncoders() {
    std::lock_guard<std::mutex> lock(mLock);
    if (!mHostEncoders && mesa_video::backendAvailable(mesa_video::Entrypoint::kEncode)) {
        for (const HostEncoder& enc : kHostEncoders) {
            if (mCatalog.count(enc.name)) continue;
            ComponentDesc d;
            d.traits = makeTraits(enc.name, IComponentStore::ComponentTraits::Kind::ENCODER, 16,
                                  enc.mime);
            d.kind = MesaCodecKind::kEncoder;
            d.castCodec = enc.castCodec;
            mCatalog[enc.name] = d;
        }
        mHostEncoders = true;
        ALOGI("host video encoders available (tebox.venc)");
    }
    if (!TeboxAencComponent::available()) {
        return;
    }
    for (const HostAudioEncoder& enc : kHostAudioEncoders) {
        if (mCatalog.count(enc.name)) continue;
        ComponentDesc d;
        d.traits.name = enc.name;
        d.traits.domain = IComponentStore::ComponentTraits::Domain::AUDIO;
        d.traits.kind = IComponentStore::ComponentTraits::Kind::ENCODER;
        d.traits.rank = 16;
        d.traits.mediaType = enc.mime;
        d.traits.aliases = {};
        d.kind = MesaCodecKind::kEncoder;
        d.audio = true;
        d.aencCodec = enc.aencCodec;
        mCatalog[enc.name] = d;
    }
    ALOGI("host audio encoders available (tebox.aenc)");
}

std::shared_ptr<Configurable> ComponentStore::storeConfigurable() {
    std::lock_guard<std::mutex> lock(mLock);
    if (!mStoreCfg) {
        mStoreCfg = SharedRefBase::make<Configurable>(0, "default");
    }
    return mStoreCfg;
}

bool ComponentStore::hasComponent(const std::string& name) {
    std::lock_guard<std::mutex> lock(mLock);
    return mCatalog.find(name) != mCatalog.end();
}

std::shared_ptr<Configurable> ComponentStore::interfaceFor(const std::string& name) {
    std::lock_guard<std::mutex> lock(mLock);
    auto it = mInterfaces.find(name);
    if (it != mInterfaces.end()) return it->second;
    int32_t id = mNextId++;
    auto desc = mCatalog.find(name);
    const bool decoder = desc != mCatalog.end() &&
                         desc->second.traits.kind ==
                                 IComponentStore::ComponentTraits::Kind::DECODER;
    const std::string mime =
            desc != mCatalog.end() ? desc->second.traits.mediaType : std::string(kMime);
    const bool audio = desc != mCatalog.end() && desc->second.audio;
    auto cfg = SharedRefBase::make<Configurable>(id, name, decoder, mime, audio);
    mInterfaces[name] = cfg;
    return cfg;
}

ScopedAStatus ComponentStore::listComponents(
        std::vector<IComponentStore::ComponentTraits>* out) {
    addHostDecoders();
    addHostEncoders();
    out->clear();
    std::lock_guard<std::mutex> lock(mLock);
    for (const auto& [name, desc] : mCatalog) {
        out->push_back(desc.traits);
        ALOGI("listComponents: %s (%s)", name.c_str(), desc.traits.mediaType.c_str());
    }
    return ScopedAStatus::ok();
}

ScopedAStatus ComponentStore::createInterface(const std::string& name,
                                            std::shared_ptr<IComponentInterface>* out) {
    addHostDecoders();
    addHostEncoders();
    if (!hasComponent(name)) {
        return ScopedAStatus::fromServiceSpecificError(Status::NOT_FOUND);
    }
    auto cfg = interfaceFor(name);
    *out = SharedRefBase::make<ComponentInterface>(cfg->id(), name, cfg);
    return ScopedAStatus::ok();
}

ScopedAStatus ComponentStore::createComponent(
        const std::string& name, const std::shared_ptr<IComponentListener>& listener,
        const std::shared_ptr<IClientManager>& /*pool*/, std::shared_ptr<IComponent>* out) {
    addHostDecoders();
    addHostEncoders();
    ComponentDesc desc;
    {
        std::lock_guard<std::mutex> lock(mLock);
        auto it = mCatalog.find(name);
        if (it == mCatalog.end()) {
            return ScopedAStatus::fromServiceSpecificError(Status::NOT_FOUND);
        }
        desc = it->second;
    }
    auto cfg = interfaceFor(name);
    auto intf = SharedRefBase::make<ComponentInterface>(cfg->id(), name, cfg);
    if (desc.audio) {
        *out = SharedRefBase::make<TeboxAencComponent>(name, desc.aencCodec, intf, cfg, listener);
        return ScopedAStatus::ok();
    }
    if (desc.hostCodec) {
        *out = SharedRefBase::make<HostDecComponent>(name, desc.hostCodec, intf, cfg, listener);
        return ScopedAStatus::ok();
    }
    auto comp = SharedRefBase::make<MesaAvcComponent>(desc.kind, name, intf, cfg, listener,
                                                      desc.castCodec);
    *out = comp;
    return ScopedAStatus::ok();
}

ScopedAStatus ComponentStore::getConfigurable(std::shared_ptr<IConfigurable>* out) {
    *out = storeConfigurable();
    return ScopedAStatus::ok();
}

ScopedAStatus ComponentStore::getPoolClientManager(std::shared_ptr<IClientManager>* out) {
    *out = nullptr;
    return ScopedAStatus::ok();
}

ScopedAStatus ComponentStore::copyBuffer(
        const aidl::android::hardware::media::c2::Buffer& /*src*/,
        const aidl::android::hardware::media::c2::Buffer& /*dst*/) {
    return ScopedAStatus::fromServiceSpecificError(Status::CANNOT_DO);
}

ScopedAStatus ComponentStore::getStructDescriptors(
        const std::vector<int32_t>& indices,
        std::vector<aidl::android::hardware::media::c2::StructDescriptor>* out) {
    out->clear();
    // CCodecConfig needs the standard raw.size layout to translate the
    // MediaFormat width/height into C2StreamPictureSizeInfo.  The rest of the
    // component uses fixed, framework-defined layouts and can be reflected by
    // the platform.
    auto requested = [&indices](uint32_t core) {
        if (indices.empty()) return true;
        for (int32_t index : indices) {
            if ((static_cast<uint32_t>(index) & 0x1ffffu) == core) return true;
        }
        return false;
    };
    // component.kind / component.domain: C2SimpleValueStruct<uint32 enum>;
    // raw.pixel-format (decoders): C2Uint32Value. Every advertised param
    // needs one: the client dereferences the first descriptor it gets back.
    for (uint32_t core : {0x804u, 0x805u, 0x1802u}) {
        if (!requested(core)) continue;
        StructDescriptor d;
        d.type = static_cast<int32_t>(core);
        FieldDescriptor value;
        value.fieldId = FieldId{0, 4};
        value.type = FieldDescriptor::Type::UINT32;
        value.extent = 1;
        value.name = "value";
        d.fields = {std::move(value)};
        out->push_back(std::move(d));
    }
    // {input,output}.media-type: flexible C2StringValue.
    if (requested(0x1080Cu)) {
        StructDescriptor d;
        d.type = static_cast<int32_t>(0x1080Cu);
        FieldDescriptor value;
        value.fieldId = FieldId{0, 1};
        value.type = FieldDescriptor::Type::STRING;
        value.extent = 0;
        value.name = "value";
        d.fields = {std::move(value)};
        out->push_back(std::move(d));
    }
    if (requested(0x1800u)) {
        StructDescriptor d;
        d.type = 0x1800;
        FieldDescriptor width;
        // StructDescriptor field offsets are relative to the payload struct;
        // the framework adds sizeof(C2Param) when applying them to a param.
        width.fieldId = FieldId{0, 4};
        width.type = FieldDescriptor::Type::UINT32;
        width.extent = 1;
        width.name = "width";
        FieldDescriptor height;
        height.fieldId = FieldId{4, 4};
        height.type = FieldDescriptor::Type::UINT32;
        height.extent = 1;
        height.name = "height";
        d.fields = {std::move(width), std::move(height)};
        out->push_back(std::move(d));
    }
    return ScopedAStatus::ok();
}

ScopedAStatus ComponentStore::createInputSurface(std::shared_ptr<IInputSurface>* out) {
    *out = nullptr;
    return ScopedAStatus::fromServiceSpecificError(Status::OMITTED);
}

}  // namespace gki
