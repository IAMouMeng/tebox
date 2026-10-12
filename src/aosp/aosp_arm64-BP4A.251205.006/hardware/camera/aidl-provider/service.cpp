#include <aidl/android/hardware/camera/provider/BnCameraProvider.h>
#include <aidl/android/hardware/camera/device/BnCameraDevice.h>
#include <aidl/android/hardware/camera/device/BnCameraDeviceSession.h>
#include <aidl/android/hardware/camera/device/HalStream.h>
#include <aidl/android/hardware/camera/device/CaptureResult.h>
#include <aidl/android/hardware/camera/device/BufferStatus.h>
#include <aidl/android/hardware/camera/device/NotifyMsg.h>
#include <aidl/android/hardware/camera/device/ShutterMsg.h>
#include <aidl/android/hardware/camera/device/StreamType.h>
#include <aidl/android/hardware/graphics/common/PixelFormat.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <android/log.h>
#include <fmq/AidlMessageQueue.h>
#include <system/camera_metadata.h>
#include <cstring>
#include <map>
#include <mutex>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

using aidl::android::hardware::camera::provider::BnCameraProvider;
using aidl::android::hardware::camera::provider::CameraIdAndStreamCombination;
using aidl::android::hardware::camera::provider::ConcurrentCameraIdCombination;
using aidl::android::hardware::camera::provider::ICameraProviderCallback;
using aidl::android::hardware::camera::common::VendorTagSection;
using aidl::android::hardware::camera::device::ICameraDevice;
using aidl::android::hardware::camera::device::CameraMetadata;
using aidl::android::hardware::camera::device::RequestTemplate;
using aidl::android::hardware::camera::device::StreamConfiguration;
using aidl::android::hardware::camera::common::CameraResourceCost;
using aidl::android::hardware::camera::device::ICameraDeviceCallback;
using aidl::android::hardware::camera::device::ICameraDeviceSession;
using aidl::android::hardware::camera::device::BnCameraDeviceSession;
using aidl::android::hardware::camera::device::HalStream;
using aidl::android::hardware::common::fmq::MQDescriptor;
using aidl::android::hardware::common::fmq::SynchronizedReadWrite;
using aidl::android::hardware::graphics::common::PixelFormat;
using aidl::android::hardware::camera::device::CaptureResult;
using aidl::android::hardware::camera::device::BufferStatus;
using aidl::android::hardware::camera::device::NotifyMsg;
using aidl::android::hardware::camera::device::ShutterMsg;
using aidl::android::hardware::common::NativeHandle;
using CameraQueue = android::AidlMessageQueue<int8_t, SynchronizedReadWrite>;

static aidl::android::hardware::camera::device::CameraMetadata makeCharacteristics() {
    aidl::android::hardware::camera::device::CameraMetadata result;
    camera_metadata_t* m = allocate_camera_metadata(64, 1024);
    if (!m) return result;
    auto add = [&](uint32_t tag, const void* data, size_t count) {
        if (add_camera_metadata_entry(m, tag, data, count) != 0)
            __android_log_print(ANDROID_LOG_ERROR, "QemuCamera", "metadata tag %u failed", tag);
    };
    const uint8_t facing = ANDROID_LENS_FACING_BACK;
    const int32_t active[] = {0, 0, 640, 480};
    const int32_t orientation = 90;
    const uint8_t capabilities[] = {ANDROID_REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE};
    const int32_t formats[] = {34, 35, 33}; // IMPLEMENTATION_DEFINED, YUV_420_888, BLOB/JPEG
    const int32_t streamConfig[] = {
        34, 640, 480, ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT,
        34, 480, 480, ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT,
        35, 640, 480, ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT,
        35, 480, 480, ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT,
        33, 640, 480, ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT,
        33, 480, 480, ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT};
    const int32_t recommendedConfigs[] = {
        640, 480, 34, ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT,
        (1 << ANDROID_SCALER_AVAILABLE_RECOMMENDED_STREAM_CONFIGURATIONS_PREVIEW) |
        (1 << ANDROID_SCALER_AVAILABLE_RECOMMENDED_STREAM_CONFIGURATIONS_RECORD)};
    const int64_t durations[] = {
        34, 640, 480, 16666666,
        34, 480, 480, 16666666,
        35, 640, 480, 16666666,
        35, 480, 480, 16666666,
        33, 640, 480, 16666666,
        33, 480, 480, 16666666};
    const int64_t stalls[] = {
        34, 640, 480, 0,
        34, 480, 480, 0,
        35, 640, 480, 0,
        35, 480, 480, 0,
        33, 640, 480, 0,
        33, 480, 480, 0};
    const int32_t processedSizes[] = {640, 480, 480, 480};
    const int32_t jpegSizes[] = {640, 480, 480, 480};
    const int64_t processedDurations[] = {16666666, 16666666};
    const int64_t jpegDurations[] = {16666666, 16666666};
    const int64_t exposure[] = {100000, 1000000000};
    const int64_t maxFrameDuration = 16666666;
    const int32_t sensitivity[] = {100, 800};
    const uint8_t ae[] = {ANDROID_CONTROL_AE_MODE_ON};
    const uint8_t lockAvailable[] = {ANDROID_CONTROL_AE_LOCK_AVAILABLE_FALSE};
    const int32_t aeFpsRanges[] = {15, 30};
    const uint8_t af[] = {ANDROID_CONTROL_AF_MODE_OFF};
    const uint8_t awb[] = {ANDROID_CONTROL_AWB_MODE_AUTO};
    const uint8_t awbLockAvailable[] = {ANDROID_CONTROL_AWB_LOCK_AVAILABLE_FALSE};
    const uint8_t controlModes[] = {ANDROID_CONTROL_MODE_AUTO};
    const int32_t hwLevel = ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_LEGACY;
    const int32_t maxOutputs[] = {1, 0, 0};
    const int32_t partialCount = 1;
    const int32_t pipelineDepth = 4;
    const int32_t syncLatency = ANDROID_SYNC_MAX_LATENCY_PER_FRAME_CONTROL;
    const float maxZoom = 1.0f;
    const int32_t aeCompRange[] = {0, 0};
    const camera_metadata_rational_t aeCompStep = {0, 1};
    const int32_t maxRegions[] = {0, 0, 0};
    const uint8_t distortionModes[] = {ANDROID_DISTORTION_CORRECTION_MODE_OFF};
    const uint8_t flash = 0;
    const float focus = 0.0f;
    const float focal = 3.5f;
    const int32_t thumb[] = {0, 0, 160, 120};
    const float physicalSize[] = {3.2f, 2.4f};
    const int32_t keys[] = {
        ANDROID_CONTROL_AE_MODE, ANDROID_CONTROL_AF_MODE, ANDROID_CONTROL_AWB_MODE,
        ANDROID_JPEG_ORIENTATION, ANDROID_SCALER_CROP_REGION,
    };
    add(ANDROID_LENS_FACING, &facing, 1);
    add(ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE, active, 4);
    add(ANDROID_SENSOR_INFO_PIXEL_ARRAY_SIZE, active + 2, 2);
    add(ANDROID_SENSOR_INFO_PHYSICAL_SIZE, physicalSize, 2);
    add(ANDROID_SENSOR_ORIENTATION, &orientation, 1);
    add(ANDROID_REQUEST_AVAILABLE_CAPABILITIES, capabilities, 1);
    add(ANDROID_SCALER_AVAILABLE_FORMATS, formats, 3);
    add(ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, streamConfig, 24);
    add(ANDROID_SCALER_AVAILABLE_RECOMMENDED_STREAM_CONFIGURATIONS, recommendedConfigs, 5);
    add(ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS, durations, 24);
    add(ANDROID_SCALER_AVAILABLE_STALL_DURATIONS, stalls, 24);
    add(ANDROID_SCALER_AVAILABLE_PROCESSED_SIZES, processedSizes, 4);
    add(ANDROID_SCALER_AVAILABLE_JPEG_SIZES, jpegSizes, 4);
    add(ANDROID_SCALER_AVAILABLE_PROCESSED_MIN_DURATIONS, processedDurations, 2);
    add(ANDROID_SCALER_AVAILABLE_JPEG_MIN_DURATIONS, jpegDurations, 2);
    const uint8_t timestampSource = ANDROID_SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME;
    add(ANDROID_SENSOR_INFO_TIMESTAMP_SOURCE, &timestampSource, 1);
    add(ANDROID_SENSOR_INFO_EXPOSURE_TIME_RANGE, exposure, 2);
    add(ANDROID_SENSOR_INFO_MAX_FRAME_DURATION, &maxFrameDuration, 1);
    add(ANDROID_SENSOR_INFO_SENSITIVITY_RANGE, sensitivity, 2);
    add(ANDROID_CONTROL_AE_AVAILABLE_MODES, ae, 1);
    add(ANDROID_CONTROL_AE_LOCK_AVAILABLE, lockAvailable, 1);
    add(ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, aeFpsRanges, 2);
    add(ANDROID_CONTROL_AE_COMPENSATION_RANGE, aeCompRange, 2);
    add(ANDROID_CONTROL_AE_COMPENSATION_STEP, &aeCompStep, 1);
    add(ANDROID_CONTROL_MAX_REGIONS, maxRegions, 3);
    add(ANDROID_CONTROL_AF_AVAILABLE_MODES, af, 1);
    add(ANDROID_CONTROL_AWB_AVAILABLE_MODES, awb, 1);
    add(ANDROID_CONTROL_AWB_LOCK_AVAILABLE, awbLockAvailable, 1);
    add(ANDROID_CONTROL_AVAILABLE_MODES, controlModes, 1);
    add(ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL, &hwLevel, 1);
    add(ANDROID_REQUEST_MAX_NUM_OUTPUT_STREAMS, maxOutputs, 3);
    add(ANDROID_REQUEST_PARTIAL_RESULT_COUNT, &partialCount, 1);
    add(ANDROID_REQUEST_PIPELINE_MAX_DEPTH, &pipelineDepth, 1);
    add(ANDROID_DISTORTION_CORRECTION_AVAILABLE_MODES, distortionModes, 1);
    add(ANDROID_SYNC_MAX_LATENCY, &syncLatency, 1);
    add(ANDROID_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM, &maxZoom, 1);
    add(ANDROID_FLASH_INFO_AVAILABLE, &flash, 1);
    add(ANDROID_LENS_INFO_MINIMUM_FOCUS_DISTANCE, &focus, 1);
    add(ANDROID_LENS_INFO_AVAILABLE_FOCAL_LENGTHS, &focal, 1);
    add(ANDROID_JPEG_AVAILABLE_THUMBNAIL_SIZES, thumb, 4);
    add(ANDROID_REQUEST_AVAILABLE_REQUEST_KEYS, keys, sizeof(keys) / sizeof(keys[0]));
    add(ANDROID_REQUEST_AVAILABLE_RESULT_KEYS, keys, sizeof(keys) / sizeof(keys[0]));
    sort_camera_metadata(m);
    result.metadata.resize(get_camera_metadata_size(m));
    __android_log_print(ANDROID_LOG_INFO, "QemuCamera", "default metadata size=%zu entries=%zu", result.metadata.size(), get_camera_metadata_entry_count(m));
    std::memcpy(result.metadata.data(), m, result.metadata.size());
    free_camera_metadata(m);
    return result;
}

static CameraMetadata makeDefaultRequestSettings(int64_t sensorTimestamp = 0) {
    CameraMetadata result;
    camera_metadata_t* m = allocate_camera_metadata(16, 256);
    if (!m) return result;
    auto add = [&](uint32_t tag, const void* data, size_t count) {
        add_camera_metadata_entry(m, tag, data, count);
    };
    const uint8_t ae = ANDROID_CONTROL_AE_MODE_ON;
    const uint8_t af = ANDROID_CONTROL_AF_MODE_OFF;
    const uint8_t awb = ANDROID_CONTROL_AWB_MODE_AUTO;
    const uint8_t control = ANDROID_CONTROL_MODE_AUTO;
    const uint8_t intent = ANDROID_CONTROL_CAPTURE_INTENT_PREVIEW;
    const int32_t orientation = 90;
    const int32_t crop[] = {0, 0, 640, 480};
    const int64_t exposure = 10000000;
    const int32_t sensitivity = 100;
    const int64_t frameDuration = 16666666;
    add(ANDROID_CONTROL_MODE, &control, 1);
    add(ANDROID_CONTROL_CAPTURE_INTENT, &intent, 1);
    add(ANDROID_CONTROL_AE_MODE, &ae, 1);
    add(ANDROID_CONTROL_AF_MODE, &af, 1);
    add(ANDROID_CONTROL_AWB_MODE, &awb, 1);
    add(ANDROID_JPEG_ORIENTATION, &orientation, 1);
    add(ANDROID_SCALER_CROP_REGION, crop, 4);
    add(ANDROID_SENSOR_EXPOSURE_TIME, &exposure, 1);
    add(ANDROID_SENSOR_SENSITIVITY, &sensitivity, 1);
    add(ANDROID_SENSOR_FRAME_DURATION, &frameDuration, 1);
    if (sensorTimestamp > 0)
        add(ANDROID_SENSOR_TIMESTAMP, &sensorTimestamp, 1);
    sort_camera_metadata(m);
    result.metadata.resize(get_camera_metadata_size(m));
    std::memcpy(result.metadata.data(), m, result.metadata.size());
    free_camera_metadata(m);
    return result;
}

class Session final : public BnCameraDeviceSession {
public:
    explicit Session(const std::shared_ptr<ICameraDeviceCallback>& cb)
        : callback(cb), requestQueue(1024), resultQueue(1024) {
        __android_log_print(ANDROID_LOG_INFO, "QemuCamera", "session created (FMQ request=%s result=%s)",
                            requestQueue.isValid() ? "valid" : "invalid",
                            resultQueue.isValid() ? "valid" : "invalid");
    }
    ndk::ScopedAStatus close() override { callback.reset(); return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus configureStreams(const StreamConfiguration& config, std::vector<HalStream>* out) override {
        __android_log_print(ANDROID_LOG_INFO, "QemuCamera", "configure streams %zu", config.streams.size());
        out->clear();
        for (const auto& stream : config.streams) {
            HalStream h;
            h.id = stream.id;
            h.overrideFormat = static_cast<PixelFormat>(stream.format);
            h.maxBuffers = 4;
            h.overrideDataSpace = stream.dataSpace;
            out->push_back(h);
        }
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus constructDefaultRequestSettings(RequestTemplate, CameraMetadata* out) override { __android_log_print(ANDROID_LOG_INFO, "QemuCamera", "constructDefaultRequestSettings"); *out = makeDefaultRequestSettings(); return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus flush() override { return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus getCaptureRequestMetadataQueue(MQDescriptor<int8_t, SynchronizedReadWrite>* out) override { __android_log_print(ANDROID_LOG_INFO, "QemuCamera", "request queue"); *out = requestQueue.dupeDesc(); return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus getCaptureResultMetadataQueue(MQDescriptor<int8_t, SynchronizedReadWrite>* out) override { __android_log_print(ANDROID_LOG_INFO, "QemuCamera", "result queue"); *out = resultQueue.dupeDesc(); return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus isReconfigurationRequired(const CameraMetadata&, const CameraMetadata&, bool* out) override { *out = false; return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus processCaptureRequest(const std::vector<aidl::android::hardware::camera::device::CaptureRequest>& requests, const std::vector<aidl::android::hardware::camera::device::BufferCache>& cachesToRemove, int32_t* out) override {
        __android_log_print(ANDROID_LOG_INFO, "QemuCamera", "process capture %zu", requests.size());
        *out = static_cast<int32_t>(requests.size());
        if (!callback) return ndk::ScopedAStatus::ok();
        {
            std::lock_guard<std::mutex> lock(bufferMutex);
            for (const auto& cache : cachesToRemove)
                buffers.erase({cache.streamId, cache.bufferId});
        }
        for (const auto& request : requests) {
            timespec now{};
            clock_gettime(CLOCK_MONOTONIC, &now);
            const int64_t timestamp = static_cast<int64_t>(now.tv_sec) * 1000000000LL + now.tv_nsec;
            CaptureResult result;
            result.frameNumber = request.frameNumber;
            result.result = makeDefaultRequestSettings(timestamp);
            result.inputBuffer.streamId = -1;
            result.inputBuffer.status = BufferStatus::ERROR;
            result.outputBuffers.clear();
            for (const auto& input : request.outputBuffers) {
                __android_log_print(ANDROID_LOG_INFO, "QemuCamera",
                                    "frame=%d stream=%d buffer=%lld fds=%zu ints=%zu",
                                    request.frameNumber, input.streamId,
                                    static_cast<long long>(input.bufferId),
                                    input.buffer.fds.size(), input.buffer.ints.size());
                NativeHandle handle;
                bool haveHandle = false;
                if (!input.buffer.fds.empty()) {
                    handle.ints = input.buffer.ints;
                    for (const auto& fd : input.buffer.fds)
                        handle.fds.emplace_back(ndk::ScopedFileDescriptor(dup(fd.get())));
                    std::lock_guard<std::mutex> lock(bufferMutex);
                    buffers[{input.streamId, input.bufferId}] = std::move(handle);
                    haveHandle = true;
                } else {
                    std::lock_guard<std::mutex> lock(bufferMutex);
                    auto it = buffers.find({input.streamId, input.bufferId});
                    if (it != buffers.end()) {
                        handle.ints = it->second.ints;
                        for (const auto& fd : it->second.fds)
                            handle.fds.emplace_back(ndk::ScopedFileDescriptor(dup(fd.get())));
                        haveHandle = true;
                    }
                }
                aidl::android::hardware::camera::device::StreamBuffer buffer;
                buffer.streamId = input.streamId;
                buffer.bufferId = input.bufferId;
                buffer.status = BufferStatus::OK;
                // The returned CaptureResult must not carry the graphics handle;
                // framework identifies it by (frameNumber, streamId, bufferId).
                const int format = handle.ints.size() > 3 ? handle.ints[3] : 0;
                if (haveHandle && handle.ints.size() > 2 && !handle.fds.empty()) {
                    const size_t height = static_cast<size_t>(handle.ints[1]);
                    const size_t stride = static_cast<size_t>(handle.ints[2]);
                    // Camera2 preview normally negotiates IMPLEMENTATION_DEFINED
                    // (34), which resolves to a 4-byte RGBA/RGBX gralloc buffer.
                    // The old provider only filled YUV (35), leaving preview
                    // buffers untouched and therefore black.
                    const size_t bytes = format == 35
                        ? stride * height + (stride * height) / 2
                        : stride * height * ((format == 4) ? 2 : 4);
                    void* pixels = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED,
                                         handle.fds[0].get(), 0);
                    if (pixels != MAP_FAILED) {
                        if (format == 35) {
                            const size_t yBytes = stride * height;
                            std::memset(pixels, 80, yBytes);
                            std::memset(static_cast<uint8_t*>(pixels) + yBytes, 128,
                                        bytes - yBytes);
                        } else {
                            // A visible neutral blue-gray test frame confirms
                            // the complete buffer path without a real sensor.
                            uint8_t* p = static_cast<uint8_t*>(pixels);
                            const size_t bpp = (format == 4) ? 2 : 4;
                            for (size_t y = 0; y < height; ++y) {
                                for (size_t x = 0; x < stride; ++x) {
                                    const uint8_t shade = static_cast<uint8_t>(48 + ((x + y) & 0x3f));
                                    if (bpp == 4) {
                                        size_t i = (y * stride + x) * 4;
                                        p[i + 0] = shade;
                                        p[i + 1] = 96;
                                        p[i + 2] = 160;
                                        p[i + 3] = 255;
                                    } else {
                                        size_t i = (y * stride + x) * 2;
                                        p[i] = shade;
                                        p[i + 1] = 128;
                                    }
                                }
                            }
                        }
                        munmap(pixels, bytes);
                    }
                }
                result.outputBuffers.push_back(std::move(buffer));
            }
            result.partialResult = 1;
            NotifyMsg notify;
            notify.set<NotifyMsg::shutter>(ShutterMsg{request.frameNumber, timestamp, 0});
            callback->notify({notify});
            std::vector<CaptureResult> results;
            results.push_back(std::move(result));
            callback->processCaptureResult(std::move(results));
        }
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus signalStreamFlush(const std::vector<int32_t>&, int32_t) override { return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus switchToOffline(const std::vector<int32_t>&, aidl::android::hardware::camera::device::CameraOfflineSessionInfo*, std::shared_ptr<aidl::android::hardware::camera::device::ICameraOfflineSession>* out) override { out->reset(); return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION); }
    ndk::ScopedAStatus repeatingRequestEnd(int32_t, const std::vector<int32_t>&) override { return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus configureStreamsV2(const StreamConfiguration& config, aidl::android::hardware::camera::device::ConfigureStreamsRet* out) override { out->halStreams.clear(); return configureStreams(config, &out->halStreams); }
private:
    std::shared_ptr<ICameraDeviceCallback> callback;
    CameraQueue requestQueue;
    CameraQueue resultQueue;
    std::mutex bufferMutex;
    std::map<std::pair<int32_t, int64_t>, NativeHandle> buffers;
};

class Device final : public aidl::android::hardware::camera::device::BnCameraDevice {
public:
    ndk::ScopedAStatus getCameraCharacteristics(CameraMetadata* out) override { *out = makeCharacteristics(); __android_log_print(ANDROID_LOG_INFO, "QemuCamera", "getCameraCharacteristics"); return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus getPhysicalCameraCharacteristics(const std::string&, CameraMetadata* out) override { out->metadata.clear(); return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus getResourceCost(CameraResourceCost* out) override { out->resourceCost = 0; out->resourceCost = 0; return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus isStreamCombinationSupported(const StreamConfiguration&, bool* out) override { *out = true; return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus open(const std::shared_ptr<ICameraDeviceCallback>& cb, std::shared_ptr<ICameraDeviceSession>* out) override { __android_log_print(ANDROID_LOG_INFO, "QemuCamera", "open"); *out = ndk::SharedRefBase::make<Session>(cb); return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus openInjectionSession(const std::shared_ptr<ICameraDeviceCallback>&, std::shared_ptr<aidl::android::hardware::camera::device::ICameraInjectionSession>* out) override { out->reset(); return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION); }
    ndk::ScopedAStatus setTorchMode(bool) override { return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION); }
    ndk::ScopedAStatus turnOnTorchWithStrengthLevel(int32_t) override { return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION); }
    ndk::ScopedAStatus getTorchStrengthLevel(int32_t* out) override { *out = 0; return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus constructDefaultRequestSettings(RequestTemplate, CameraMetadata* out) override { __android_log_print(ANDROID_LOG_INFO, "QemuCamera", "device default request"); *out = makeDefaultRequestSettings(); return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus isStreamCombinationWithSettingsSupported(const StreamConfiguration&, bool* out) override { *out = false; return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus getSessionCharacteristics(const StreamConfiguration&, CameraMetadata* out) override { out->metadata.clear(); return ndk::ScopedAStatus::ok(); }
};

class Provider final : public BnCameraProvider {
public:
    ndk::ScopedAStatus setCallback(const std::shared_ptr<ICameraProviderCallback>& cb) override {
        callback = cb;
        if (callback) callback->cameraDeviceStatusChange("device@1.0/internal/0", aidl::android::hardware::camera::common::CameraDeviceStatus::PRESENT);
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus getVendorTags(std::vector<VendorTagSection>* out) override { out->clear(); return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus getCameraIdList(std::vector<std::string>* out) override { *out = {"device@1.0/internal/0"}; return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus getCameraDeviceInterface(const std::string&, std::shared_ptr<ICameraDevice>* out) override {
        __android_log_print(ANDROID_LOG_INFO, "QemuCamera", "getCameraDeviceInterface");
        *out = ndk::SharedRefBase::make<Device>();
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus notifyDeviceStateChange(int64_t) override { return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus getConcurrentCameraIds(std::vector<ConcurrentCameraIdCombination>* out) override { out->clear(); return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus isConcurrentStreamCombinationSupported(const std::vector<CameraIdAndStreamCombination>&, bool* out) override { *out = false; return ndk::ScopedAStatus::ok(); }
private:
    std::shared_ptr<ICameraProviderCallback> callback;
};

int main() {
    __android_log_print(ANDROID_LOG_INFO, "QemuCamera", "starting provider");
    ABinderProcess_setThreadPoolMaxThreadCount(4);
    auto provider = ndk::SharedRefBase::make<Provider>();
    const char* name = "android.hardware.camera.provider.ICameraProvider/internal/0";
    if (AServiceManager_addService(provider->asBinder().get(), name) != EX_NONE) return 1;
    ABinderProcess_joinThreadPool();
    return 0;
}
