// QEMU bring-up Audio HAL. PCM output is paced and discarded; input is silence.
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <android/log.h>
#include <aidl/android/hardware/audio/core/BnConfig.h>
#include <aidl/android/hardware/audio/effect/BnFactory.h>
#include <aidl/android/media/audio/common/AudioOutputFlags.h>
#include "audio_defaults.h"
#include "fmq.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace core = aidl::android::hardware::audio::core;
namespace effect = aidl::android::hardware::audio::effect;
namespace metadata = aidl::android::hardware::audio::common;
namespace media = aidl::android::media::audio::common;
using namespace core;
using namespace media;
using ndk::ScopedAStatus;
using ndk::SharedRefBase;
using Clock = std::chrono::steady_clock;
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "qemu-audio", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "qemu-audio", __VA_ARGS__)

static auto ok() { return ScopedAStatus::ok(); }
static auto bad() { return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT); }
static auto unsupported() { return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION); }
static auto illegalState() { return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE); }
static int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}
static AudioFormatDescription pcm16() {
    AudioFormatDescription f;
    f.type = AudioFormatType::PCM;
    f.pcm = PcmType::INT_16_BIT;
    return f;
}
static AudioChannelLayout channels(bool input) {
    return AudioChannelLayout::make<AudioChannelLayout::layoutMask>(
        input ? int(AudioChannelLayout::LAYOUT_MONO) : int(AudioChannelLayout::LAYOUT_STEREO));
}

class StreamContext {
    Fmq<StreamDescriptor::Command> command_;
    Fmq<StreamDescriptor::Reply> reply_;
    Fmq<int8_t> audio_;
    const bool input_;
    const int rate_, frameSize_, bufferFrames_;
    std::atomic<bool> stop_{false};
    std::thread worker_;
    std::mutex closeMutex_;
    void run() {
        using State = StreamDescriptor::State;
        using Tag = StreamDescriptor::Command::Tag;
        State state = State::STANDBY;
        int64_t frames = 0;
        auto deadline = Clock::now();
        std::vector<int8_t> data(size_t(bufferFrames_) * frameSize_, 0);
        while (!stop_) {
            StreamDescriptor::Command cmd;
            if (!command_.read(&cmd)) { command_.waitReadable(); continue; }
            if (stop_) break;
            StreamDescriptor::Reply result;
            result.status = 0;
            result.fmqByteCount = 0;
            result.latencyMs = (bufferFrames_ * 1000 + rate_ - 1) / rate_;
            result.xrunFrames = 0;
            switch (cmd.getTag()) {
                case Tag::getStatus: break;
                case Tag::start:
                    if (state == State::STANDBY) state = State::IDLE;
                    else if (state == State::PAUSED) state = State::ACTIVE;
                    else result.status = -ENOSYS;
                    deadline = Clock::now();
                    break;
                case Tag::burst: {
                    int bytes = cmd.get<Tag::burst>();
                    if (bytes < 0 || size_t(bytes) > data.size() || bytes % frameSize_) {
                        result.status = -EINVAL; break;
                    }
                    if (state != State::IDLE && state != State::ACTIVE) {
                        result.status = -ENOSYS; break;
                    }
                    // Synchronous PCM only: consumption and the reply complete together.
                    if (!input_ && !audio_.read(data.data(), bytes)) { result.status = -EIO; break; }
                    int count = bytes / frameSize_;
                    deadline = std::max(deadline, Clock::now()) +
                        std::chrono::nanoseconds(int64_t(count) * 1000000000 / rate_);
                    std::this_thread::sleep_until(deadline);
                    if (input_ && !audio_.write(data.data(), bytes)) { result.status = -EIO; break; }
                    frames += count;
                    result.fmqByteCount = bytes;
                    state = State::ACTIVE;
                    break;
                }
                case Tag::pause:
                    if (state == State::ACTIVE) state = State::PAUSED;
                    else result.status = -ENOSYS;
                    break;
                case Tag::flush:
                    if (state != State::PAUSED && state != State::IDLE) { result.status = -ENOSYS; break; }
                    state = State::IDLE;
                    break;
                case Tag::drain:
                    if (state == State::ACTIVE || state == State::IDLE) state = State::IDLE;
                    else result.status = -ENOSYS;
                    break;
                case Tag::standby:
                    if (state == State::IDLE) state = State::STANDBY;
                    else result.status = -ENOSYS;
                    break;
                default: result.status = -EINVAL; break;
            }
            result.state = state;
            result.observable.frames = frames;
            result.observable.timeNs = nowNs();
            result.hardware = result.observable;
            if (!reply_.write(&result)) {
                LOGE("reply queue full; stopping invalid stream");
                break;
            }
        }
        stop_ = true;
    }
  public:
    StreamContext(bool input, int rate, int frames)
        : input_(input), rate_(rate), frameSize_(input ? 2 : 4), bufferFrames_(frames) {}
    ~StreamContext() { close(); }
    bool initialize(StreamDescriptor* desc) {
        if (!command_.initialize(1) || !reply_.initialize(1) ||
            !audio_.initialize(size_t(bufferFrames_) * frameSize_)) return false;
        Fmq<int8_t>::Descriptor audioDesc;
        if (!command_.descriptor(&desc->command) || !reply_.descriptor(&desc->reply) ||
            !audio_.descriptor(&audioDesc)) return false;
        desc->frameSizeBytes = frameSize_;
        desc->bufferSizeFrames = bufferFrames_;
        desc->audio.set<StreamDescriptor::AudioBuffer::fmq>(std::move(audioDesc));
        worker_ = std::thread([this] { run(); });
        return true;
    }
    bool closed() const { return stop_; }
    void close() {
        std::lock_guard lock(closeMutex_);
        stop_ = true;
        command_.wake(2);
        if (worker_.joinable()) worker_.join();
    }
};

class StreamCommon final : public UnsupportedStreamCommon {
    std::shared_ptr<StreamContext> context_;
  public:
    explicit StreamCommon(std::shared_ptr<StreamContext> c) : context_(std::move(c)) {}
    ScopedAStatus close() override { context_->close(); return ok(); }
    ScopedAStatus prepareToClose() override { return ok(); }
    ScopedAStatus updateHwAvSyncId(int32_t) override { return unsupported(); }
    ScopedAStatus getVendorParameters(const std::vector<std::string>& ids,
                                      std::vector<VendorParameter>* out) override {
        out->clear(); return ids.empty() ? ok() : unsupported();
    }
    ScopedAStatus setVendorParameters(const std::vector<VendorParameter>& parameters, bool) override {
        return parameters.empty() ? ok() : unsupported();
    }
};

class StreamOut final : public UnsupportedStreamOut {
    std::shared_ptr<IStreamCommon> common_;
    std::mutex mutex_;
    std::vector<float> volumes_{1, 1};
  public:
    explicit StreamOut(const std::shared_ptr<StreamContext>& c)
        : common_(SharedRefBase::make<StreamCommon>(c)) {}
    ScopedAStatus getStreamCommon(std::shared_ptr<IStreamCommon>* out) override { *out = common_; return ok(); }
    ScopedAStatus updateMetadata(const metadata::SourceMetadata&) override { return ok(); }
    ScopedAStatus getHwVolume(std::vector<float>* out) override { std::lock_guard lock(mutex_); *out = volumes_; return ok(); }
    ScopedAStatus setHwVolume(const std::vector<float>& values) override {
        if (values.size() != 2 || std::any_of(values.begin(), values.end(),
            [](float v) { return !std::isfinite(v) || v < 0 || v > 1; })) return bad();
        std::lock_guard lock(mutex_); volumes_ = values; return ok();
    }
    ScopedAStatus getRecommendedLatencyModes(std::vector<AudioLatencyMode>* out) override {
        *out = {AudioLatencyMode::FREE}; return ok();
    }
    ScopedAStatus setLatencyMode(AudioLatencyMode mode) override {
        return mode == AudioLatencyMode::FREE ? ok() : unsupported();
    }
};

class StreamIn final : public UnsupportedStreamIn {
    std::shared_ptr<IStreamCommon> common_;
  public:
    explicit StreamIn(const std::shared_ptr<StreamContext>& c)
        : common_(SharedRefBase::make<StreamCommon>(c)) {}
    ScopedAStatus getStreamCommon(std::shared_ptr<IStreamCommon>* out) override { *out = common_; return ok(); }
    ScopedAStatus updateMetadata(const metadata::SinkMetadata&) override { return ok(); }
    ScopedAStatus getActiveMicrophones(std::vector<MicrophoneDynamicInfo>* out) override { out->clear(); return ok(); }
    ScopedAStatus getHwGain(std::vector<float>* out) override { *out = {1}; return ok(); }
};

class Module final : public UnsupportedModule {
    std::mutex mutex_;
    std::vector<AudioPort> ports_;
    std::vector<AudioRoute> routes_;
    std::map<int, AudioPortConfig> configs_;
    std::map<int, AudioPatch> patches_;
    std::map<int, std::weak_ptr<StreamContext>> streams_;
    int nextConfig_ = 100, nextPatch_ = 1;
    bool masterMute_ = false, micMute_ = false;
    float volume_ = 1;
    const AudioPort* port(int id) const {
        auto it = std::find_if(ports_.begin(), ports_.end(), [id](const auto& p) { return p.id == id; });
        return it == ports_.end() ? nullptr : &*it;
    }
    bool streamOpen(int id) {
        auto it = streams_.find(id);
        if (it == streams_.end()) return false;
        auto stream = it->second.lock();
        return stream && !stream->closed();
    }
    static AudioPort makePort(int id, const char* name, bool input, bool device) {
        AudioPort p;
        p.id = id; p.name = name;
        AudioProfile profile;
        profile.format = pcm16(); profile.channelMasks = {channels(input)};
        profile.sampleRates = {48000};
        p.profiles = {profile};
        if (input) p.flags.set<AudioIoFlags::input>(0);
        else p.flags.set<AudioIoFlags::output>(device ? 0 : (1 << int(AudioOutputFlags::PRIMARY)));
        if (device) {
            AudioPortDeviceExt ext;
            ext.device.type.type = input ? AudioDeviceType::IN_MICROPHONE : AudioDeviceType::OUT_SPEAKER;
            ext.flags = 1 << AudioPortDeviceExt::FLAG_INDEX_DEFAULT_DEVICE;
            p.ext.set<AudioPortExt::device>(ext);
        } else {
            AudioPortMixExt ext;
            ext.maxOpenStreamCount = 1; ext.maxActiveStreamCount = 1;
            p.ext.set<AudioPortExt::mix>(ext);
        }
        return p;
    }
    template<class Args, class Return, class Stream>
    ScopedAStatus open(const Args& args, Return* out, bool input) {
        std::lock_guard lock(mutex_);
        auto it = configs_.find(args.portConfigId);
        if (it == configs_.end() || it->second.portId != (input ? 4 : 3) ||
            args.bufferSizeFrames < 0 || args.bufferSizeFrames > 16384) return bad();
        if (streamOpen(args.portConfigId)) return illegalState();
        int frames = std::max<int64_t>(960, args.bufferSizeFrames);
        auto context = std::make_shared<StreamContext>(input, 48000, frames);
        if (!context->initialize(&out->desc)) {
            LOGE("unable to create stream FMQ: %s", strerror(errno));
            return illegalState();
        }
        out->stream = SharedRefBase::make<Stream>(context);
        streams_[args.portConfigId] = context;
        LOGI("opened %s PCM16 48000 Hz, %d frames", input ? "input" : "output", frames);
        return ok();
    }
  public:
    Module() {
        ports_ = {makePort(1, "Speaker", false, true), makePort(2, "Built-In Mic", true, true),
                  makePort(3, "primary output", false, false), makePort(4, "primary input", true, false)};
        AudioRoute output, input;
        output.sourcePortIds = {3}; output.sinkPortId = 1; output.isExclusive = false;
        input.sourcePortIds = {2}; input.sinkPortId = 4; input.isExclusive = false;
        routes_ = {output, input};
    }
    ScopedAStatus setModuleDebug(const ModuleDebug&) override { return ok(); }
    ScopedAStatus getTelephony(std::shared_ptr<ITelephony>* out) override { out->reset(); return ok(); }
    ScopedAStatus getBluetooth(std::shared_ptr<IBluetooth>* out) override { out->reset(); return ok(); }
    ScopedAStatus getBluetoothA2dp(std::shared_ptr<IBluetoothA2dp>* out) override { out->reset(); return ok(); }
    ScopedAStatus getBluetoothLe(std::shared_ptr<IBluetoothLe>* out) override { out->reset(); return ok(); }
    ScopedAStatus getAudioPorts(std::vector<AudioPort>* out) override { *out = ports_; return ok(); }
    ScopedAStatus getAudioPort(int32_t id, AudioPort* out) override {
        auto p = port(id); if (!p) return bad(); *out = *p; return ok();
    }
    ScopedAStatus getAudioRoutes(std::vector<AudioRoute>* out) override { *out = routes_; return ok(); }
    ScopedAStatus getAudioRoutesForAudioPort(int32_t id, std::vector<AudioRoute>* out) override {
        if (!port(id)) return bad(); out->clear();
        for (const auto& r : routes_) if (r.sinkPortId == id ||
            std::find(r.sourcePortIds.begin(), r.sourcePortIds.end(), id) != r.sourcePortIds.end()) out->push_back(r);
        return ok();
    }
    ScopedAStatus getAudioPortConfigs(std::vector<AudioPortConfig>* out) override {
        std::lock_guard lock(mutex_); out->clear(); for (auto& [id, c] : configs_) out->push_back(c); return ok();
    }
    ScopedAStatus setAudioPortConfig(const AudioPortConfig& requested, AudioPortConfig* suggested,
                                     bool* applied) override {
        std::lock_guard lock(mutex_);
        auto p = port(requested.portId);
        if (!p || (requested.id && (!configs_.count(requested.id) ||
            configs_[requested.id].portId != requested.portId))) return bad();
        if (streamOpen(requested.id)) return illegalState();
        *suggested = requested;
        bool input = p->flags.getTag() == AudioIoFlags::input;
        Int rate; rate.value = 48000;
        suggested->sampleRate = rate;
        suggested->channelMask = channels(input);
        suggested->format = pcm16();
        suggested->flags = p->flags;
        if (requested.ext.getTag() != p->ext.getTag()) suggested->ext = p->ext;
        *applied = requested.sampleRate == suggested->sampleRate &&
                   requested.channelMask == suggested->channelMask && requested.format == suggested->format;
        if (*applied) {
            if (!suggested->id) suggested->id = nextConfig_++;
            configs_[suggested->id] = *suggested;
            LOGI("configured port %d as %d", p->id, suggested->id);
        }
        return ok();
    }
    ScopedAStatus resetAudioPortConfig(int32_t id) override {
        std::lock_guard lock(mutex_);
        if (!configs_.count(id)) return bad();
        if (streamOpen(id)) return illegalState();
        for (auto& [key, p] : patches_) {
            if (std::find(p.sourcePortConfigIds.begin(), p.sourcePortConfigIds.end(), id) != p.sourcePortConfigIds.end() ||
                std::find(p.sinkPortConfigIds.begin(), p.sinkPortConfigIds.end(), id) != p.sinkPortConfigIds.end()) return illegalState();
        }
        configs_.erase(id); streams_.erase(id); return ok();
    }
    ScopedAStatus getAudioPatches(std::vector<AudioPatch>* out) override {
        std::lock_guard lock(mutex_); out->clear(); for (auto& [id, p] : patches_) out->push_back(p); return ok();
    }
    ScopedAStatus setAudioPatch(const AudioPatch& request, AudioPatch* out) override {
        std::lock_guard lock(mutex_);
        if (request.sourcePortConfigIds.size() != 1 || request.sinkPortConfigIds.size() != 1 ||
            (request.id && !patches_.count(request.id))) return bad();
        auto src = configs_.find(request.sourcePortConfigIds[0]);
        auto dst = configs_.find(request.sinkPortConfigIds[0]);
        if (src == configs_.end() || dst == configs_.end()) return bad();
        if (!((src->second.portId == 3 && dst->second.portId == 1) ||
              (src->second.portId == 2 && dst->second.portId == 4))) return bad();
        *out = request;
        if (!out->id) out->id = nextPatch_++;
        out->minimumStreamBufferSizeFrames = 960;
        out->latenciesMs = {20};
        patches_[out->id] = *out;
        return ok();
    }
    ScopedAStatus resetAudioPatch(int32_t id) override {
        std::lock_guard lock(mutex_); return patches_.erase(id) ? ok() : bad();
    }
    ScopedAStatus openOutputStream(const OpenOutputStreamArguments& args, OpenOutputStreamReturn* out) override {
        if (args.offloadInfo || args.callback) return unsupported();
        return open<OpenOutputStreamArguments, OpenOutputStreamReturn, StreamOut>(args, out, false);
    }
    ScopedAStatus openInputStream(const OpenInputStreamArguments& args, OpenInputStreamReturn* out) override {
        return open<OpenInputStreamArguments, OpenInputStreamReturn, StreamIn>(args, out, true);
    }
    ScopedAStatus getMasterMute(bool* out) override { std::lock_guard lock(mutex_); *out = masterMute_; return ok(); }
    ScopedAStatus setMasterMute(bool value) override { std::lock_guard lock(mutex_); masterMute_ = value; return ok(); }
    ScopedAStatus getMicMute(bool* out) override { std::lock_guard lock(mutex_); *out = micMute_; return ok(); }
    ScopedAStatus setMicMute(bool value) override { std::lock_guard lock(mutex_); micMute_ = value; return ok(); }
    ScopedAStatus getMasterVolume(float* out) override { std::lock_guard lock(mutex_); *out = volume_; return ok(); }
    ScopedAStatus setMasterVolume(float value) override {
        if (!std::isfinite(value) || value < 0 || value > 1) return bad();
        std::lock_guard lock(mutex_); volume_ = value; return ok();
    }
    ScopedAStatus getMicrophones(std::vector<MicrophoneInfo>* out) override { out->clear(); return ok(); }
    ScopedAStatus updateAudioMode(AudioMode) override { return ok(); }
    ScopedAStatus updateScreenRotation(ScreenRotation) override { return ok(); }
    ScopedAStatus updateScreenState(bool) override { return ok(); }
    ScopedAStatus getSoundDose(std::shared_ptr<core::sounddose::ISoundDose>* out) override { out->reset(); return ok(); }
    ScopedAStatus getVendorParameters(const std::vector<std::string>& ids, std::vector<VendorParameter>* out) override {
        out->clear(); return ids.empty() ? ok() : unsupported();
    }
    ScopedAStatus setVendorParameters(const std::vector<VendorParameter>& parameters, bool) override {
        return parameters.empty() ? ok() : unsupported();
    }
    ScopedAStatus getMmapPolicyInfos(AudioMMapPolicyType, std::vector<AudioMMapPolicyInfo>* out) override {
        out->clear(); return ok();
    }
    ScopedAStatus supportsVariableLatency(bool* out) override { *out = false; return ok(); }
    ScopedAStatus getAAudioMixerBurstCount(int32_t* out) override { *out = 2; return ok(); }
    ScopedAStatus getAAudioHardwareBurstMinUsec(int32_t* out) override { *out = 20000; return ok(); }
};

class Config final : public BnConfig {
  public:
    ScopedAStatus getSurroundSoundConfig(SurroundSoundConfig* out) override { *out = {}; return ok(); }
    ScopedAStatus getEngineConfig(AudioHalEngineConfig* out) override {
        // An empty engine config selects the framework's default product strategies.
        *out = {}; return ok();
    }
};
class Factory final : public effect::BnFactory {
  public:
    ScopedAStatus queryEffects(const std::optional<AudioUuid>&, const std::optional<AudioUuid>&,
                              const std::optional<AudioUuid>&, std::vector<effect::Descriptor>* out) override {
        out->clear(); return ok();
    }
    ScopedAStatus queryProcessing(const std::optional<effect::Processing::Type>&,
                                 std::vector<effect::Processing>* out) override { out->clear(); return ok(); }
    ScopedAStatus createEffect(const AudioUuid&, std::shared_ptr<effect::IEffect>* out) override {
        out->reset(); return bad();
    }
    ScopedAStatus destroyEffect(const std::shared_ptr<effect::IEffect>&) override { return bad(); }
};

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(4);
    auto module = SharedRefBase::make<Module>();
    auto config = SharedRefBase::make<Config>();
    auto factory = SharedRefBase::make<Factory>();
    for (auto& [name, binder] : std::vector<std::pair<const char*, ndk::SpAIBinder>>{
        {"android.hardware.audio.core.IModule/default", module->asBinder()},
        {"android.hardware.audio.core.IConfig/default", config->asBinder()},
        {"android.hardware.audio.effect.IFactory/default", factory->asBinder()}}) {
        auto result = AServiceManager_addService(binder.get(), name);
        if (result != STATUS_OK) { LOGE("cannot register %s: %d", name, result); return 1; }
        LOGI("registered %s (silent PCM backend)", name);
    }
    ABinderProcess_joinThreadPool();
    return 0;
}
