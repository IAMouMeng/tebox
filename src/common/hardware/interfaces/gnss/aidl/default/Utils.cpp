/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * AIDL-only slice of gnss/common/utils/default/Utils.cpp (HIDL 1.0/2.0/2.1
 * helpers dropped for the NDK port).
 */
#include <Constants.h>
#include <MockLocation.h>
#include <Utils.h>
#include <utils/SystemClock.h>

namespace android {
namespace hardware {
namespace gnss {
namespace common {

using aidl::android::hardware::gnss::ElapsedRealtime;
using aidl::android::hardware::gnss::GnssClock;
using aidl::android::hardware::gnss::GnssConstellationType;
using aidl::android::hardware::gnss::GnssData;
using aidl::android::hardware::gnss::GnssLocation;
using aidl::android::hardware::gnss::GnssMeasurement;
using aidl::android::hardware::gnss::SatellitePvt;
using GnssSvInfo = aidl::android::hardware::gnss::IGnssCallback::GnssSvInfo;
using GnssSvFlags = aidl::android::hardware::gnss::IGnssCallback::GnssSvFlags;
using GnssAgc = aidl::android::hardware::gnss::GnssData::GnssAgc;

namespace {
GnssMeasurement getMockGnssMeasurement(int svid, GnssConstellationType constellationType,
                                       float cN0DbHz, float basebandCN0DbHz,
                                       double carrierFrequencyHz, bool enableCorrVecOutputs) {
    aidl::android::hardware::gnss::GnssSignalType signalType = {
            .constellation = constellationType,
            .carrierFrequencyHz = carrierFrequencyHz,
            .codeType = aidl::android::hardware::gnss::GnssSignalType::CODE_TYPE_C,
    };
    GnssMeasurement measurement = {
            .flags = GnssMeasurement::HAS_AUTOMATIC_GAIN_CONTROL |
                     GnssMeasurement::HAS_CARRIER_FREQUENCY | GnssMeasurement::HAS_CARRIER_PHASE |
                     GnssMeasurement::HAS_CARRIER_PHASE_UNCERTAINTY |
                     GnssMeasurement::HAS_FULL_ISB | GnssMeasurement::HAS_FULL_ISB_UNCERTAINTY |
                     GnssMeasurement::HAS_SATELLITE_ISB |
                     GnssMeasurement::HAS_SATELLITE_ISB_UNCERTAINTY |
                     GnssMeasurement::HAS_SATELLITE_PVT,
            .svid = svid,
            .signalType = signalType,
            .state = GnssMeasurement::STATE_CODE_LOCK | GnssMeasurement::STATE_BIT_SYNC |
                     GnssMeasurement::STATE_SUBFRAME_SYNC | GnssMeasurement::STATE_TOW_DECODED |
                     GnssMeasurement::STATE_GLO_STRING_SYNC |
                     GnssMeasurement::STATE_GLO_TOD_DECODED,
            .receivedSvTimeInNs = 8195997131077,
            .receivedSvTimeUncertaintyInNs = 15,
            .antennaCN0DbHz = cN0DbHz,
            .basebandCN0DbHz = basebandCN0DbHz,
            .pseudorangeRateMps = -484.13739013671875,
            .pseudorangeRateUncertaintyMps = 0.1037999987602233,
            .accumulatedDeltaRangeState = GnssMeasurement::ADR_STATE_VALID,
            .accumulatedDeltaRangeM = 1.52,
            .accumulatedDeltaRangeUncertaintyM = 2.43,
            .multipathIndicator = aidl::android::hardware::gnss::GnssMultipathIndicator::UNKNOWN,
            .agcLevelDb = 2.3,
            .fullInterSignalBiasNs = 21.5,
            .fullInterSignalBiasUncertaintyNs = 792.0,
            .satelliteInterSignalBiasNs = 233.9,
            .satelliteInterSignalBiasUncertaintyNs = 921.2,
            .satellitePvt =
                    {
                            .flags = SatellitePvt::HAS_POSITION_VELOCITY_CLOCK_INFO |
                                     SatellitePvt::HAS_IONO | SatellitePvt::HAS_TROPO,
                            .satPosEcef = {.posXMeters = 10442993.1153328,
                                           .posYMeters = -19926932.8051666,
                                           .posZMeters = -12034295.0216203,
                                           .ureMeters = 1000.2345678},
                            .satVelEcef = {.velXMps = -478.667183715732,
                                           .velYMps = 1580.68371984114,
                                           .velZMps = -3030.52994449997,
                                           .ureRateMps = 10.2345678},
                            .satClockInfo = {.satHardwareCodeBiasMeters = 1.396983861923e-09,
                                             .satTimeCorrectionMeters = -7113.08964331,
                                             .satClkDriftMps = 0},
                            .ionoDelayMeters = 3.069949602639317e-08,
                            .tropoDelayMeters = 3.882265204404031,
                            .timeOfClockSeconds = 12345,
                            .issueOfDataClock = 143,
                            .timeOfEphemerisSeconds = 9876,
                            .issueOfDataEphemeris = 48,
                            .ephemerisSource =
                                    SatellitePvt::SatelliteEphemerisSource::SERVER_LONG_TERM,
                    },
            .correlationVectors = {}};

    if (enableCorrVecOutputs) {
        aidl::android::hardware::gnss::CorrelationVector correlationVector1 = {
                .frequencyOffsetMps = 10,
                .samplingWidthM = 30,
                .samplingStartM = 0,
                .magnitude = {0, 5000, 10000, 5000, 0, 0, 3000, 0}};
        aidl::android::hardware::gnss::CorrelationVector correlationVector2 = {
                .frequencyOffsetMps = 20,
                .samplingWidthM = 30,
                .samplingStartM = -10,
                .magnitude = {0, 3000, 5000, 3000, 0, 0, 1000, 0}};
        measurement.correlationVectors = {correlationVector1, correlationVector2};
        measurement.flags |= GnssMeasurement::HAS_CORRELATION_VECTOR;
    }
    return measurement;
}

GnssSvInfo getMockSvInfo(int svid, GnssConstellationType type, float cN0DbHz, float basebandCN0DbHz,
                         float elevationDegrees, float azimuthDegrees, long carrierFrequencyHz) {
    GnssSvInfo svInfo = {
            .svid = svid,
            .constellation = type,
            .cN0Dbhz = cN0DbHz,
            .basebandCN0DbHz = basebandCN0DbHz,
            .elevationDegrees = elevationDegrees,
            .azimuthDegrees = azimuthDegrees,
            .carrierFrequencyHz = carrierFrequencyHz,
            .svFlag = (int)GnssSvFlags::USED_IN_FIX | (int)GnssSvFlags::HAS_EPHEMERIS_DATA |
                      (int)GnssSvFlags::HAS_ALMANAC_DATA | (int)GnssSvFlags::HAS_CARRIER_FREQUENCY};
    return svInfo;
}
}  // namespace

GnssData Utils::getMockMeasurement(const bool enableCorrVecOutputs, const bool enableFullTracking) {
    std::vector<GnssMeasurement> measurements = {
            getMockGnssMeasurement(3, GnssConstellationType::GPS, 32.5, 27.5, kGpsL1FreqHz,
                                   enableCorrVecOutputs),
            getMockGnssMeasurement(5, GnssConstellationType::GPS, 27.0, 22.0, kGpsL1FreqHz,
                                   enableCorrVecOutputs),
            getMockGnssMeasurement(17, GnssConstellationType::GPS, 30.5, 25.5, kGpsL5FreqHz,
                                   enableCorrVecOutputs),
            getMockGnssMeasurement(26, GnssConstellationType::GPS, 24.1, 19.1, kGpsL5FreqHz,
                                   enableCorrVecOutputs),
            getMockGnssMeasurement(2, GnssConstellationType::GALILEO, 33.5, 27.5, kGalE1FreqHz,
                                   enableCorrVecOutputs),
            getMockGnssMeasurement(4, GnssConstellationType::GALILEO, 28.0, 22.0, kGalE1FreqHz,
                                   enableCorrVecOutputs),
            getMockGnssMeasurement(10, GnssConstellationType::GALILEO, 35.5, 25.5, kGalE1FreqHz,
                                   enableCorrVecOutputs),
            getMockGnssMeasurement(29, GnssConstellationType::GALILEO, 34.1, 19.1, kGalE1FreqHz,
                                   enableCorrVecOutputs),
            getMockGnssMeasurement(5, GnssConstellationType::GLONASS, 20.5, 15.5, kGloG1FreqHz,
                                   enableCorrVecOutputs),
            getMockGnssMeasurement(17, GnssConstellationType::GLONASS, 21.5, 16.5, kGloG1FreqHz,
                                   enableCorrVecOutputs),
            getMockGnssMeasurement(18, GnssConstellationType::GLONASS, 28.3, 25.3, kGloG1FreqHz,
                                   enableCorrVecOutputs),
            getMockGnssMeasurement(10, GnssConstellationType::GLONASS, 25.0, 20.0, kGloG1FreqHz,
                                   enableCorrVecOutputs),
            getMockGnssMeasurement(3, GnssConstellationType::IRNSS, 22.0, 19.7, kIrnssL5FreqHz,
                                   enableCorrVecOutputs),
    };

    GnssClock clock = {
            .gnssClockFlags = GnssClock::HAS_FULL_BIAS | GnssClock::HAS_BIAS |
                              GnssClock::HAS_BIAS_UNCERTAINTY | GnssClock::HAS_DRIFT |
                              GnssClock::HAS_DRIFT_UNCERTAINTY,
            .timeNs = 2713545000000,
            .fullBiasNs = -1226701900521857520,
            .biasNs = 0.59689998626708984,
            .biasUncertaintyNs = 47514.989972114563,
            .driftNsps = -51.757811607455452,
            .driftUncertaintyNsps = 310.64968328491528,
            .hwClockDiscontinuityCount = 1,
            .referenceSignalTypeForIsb = {
                    .constellation = GnssConstellationType::GLONASS,
                    .carrierFrequencyHz = 1.59975e+09,
                    .codeType = aidl::android::hardware::gnss::GnssSignalType::CODE_TYPE_C,
            }};

    ElapsedRealtime timestamp = {
            .flags = ElapsedRealtime::HAS_TIMESTAMP_NS | ElapsedRealtime::HAS_TIME_UNCERTAINTY_NS,
            .timestampNs = ::android::elapsedRealtimeNano(),
            .timeUncertaintyNs = 1020400};

    GnssAgc gnssAgc1 = {
            .agcLevelDb = 3.5,
            .constellation = GnssConstellationType::GLONASS,
            .carrierFrequencyHz = (int64_t)kGloG1FreqHz,
    };
    GnssAgc gnssAgc2 = {
            .agcLevelDb = -5.1,
            .constellation = GnssConstellationType::GPS,
            .carrierFrequencyHz = (int64_t)kGpsL1FreqHz,
    };

    (void)enableFullTracking;  // GnssData AIDL v2 has no isFullTracking (added later).
    GnssData gnssData = {.measurements = measurements,
                         .clock = clock,
                         .elapsedRealtime = timestamp,
                         .gnssAgcs = std::vector({gnssAgc1, gnssAgc2})};
    return gnssData;
}

GnssLocation Utils::getMockLocation() {
    ElapsedRealtime elapsedRealtime = {
            .flags = ElapsedRealtime::HAS_TIMESTAMP_NS | ElapsedRealtime::HAS_TIME_UNCERTAINTY_NS,
            .timestampNs = ::android::elapsedRealtimeNano(),
            .timeUncertaintyNs = 1020400};
    GnssLocation location = {.gnssLocationFlags = 0xFF,
                             .latitudeDegrees = gMockLatitudeDegrees,
                             .longitudeDegrees = gMockLongitudeDegrees,
                             .altitudeMeters = gMockAltitudeMeters,
                             .speedMetersPerSec = gMockSpeedMetersPerSec,
                             .bearingDegrees = gMockBearingDegrees,
                             .horizontalAccuracyMeters = kMockHorizontalAccuracyMeters,
                             .verticalAccuracyMeters = kMockVerticalAccuracyMeters,
                             .speedAccuracyMetersPerSecond = kMockSpeedAccuracyMetersPerSecond,
                             .bearingAccuracyDegrees = kMockBearingAccuracyDegrees,
                             .timestampMillis = static_cast<int64_t>(
                                     kMockTimestamp + ::android::elapsedRealtimeNano() / 1e6),
                             .elapsedRealtime = elapsedRealtime};
    return location;
}

std::vector<GnssSvInfo> Utils::getMockSvInfoList() {
    return {
            getMockSvInfo(3, GnssConstellationType::GPS, 32.5, 27.5, 59.1, 166.5, kGpsL1FreqHz),
            getMockSvInfo(5, GnssConstellationType::GPS, 27.0, 22.0, 29.0, 56.5, kGpsL1FreqHz),
            getMockSvInfo(17, GnssConstellationType::GPS, 30.5, 25.5, 71.0, 77.0, kGpsL5FreqHz),
            getMockSvInfo(26, GnssConstellationType::GPS, 24.1, 19.1, 28.0, 253.0, kGpsL5FreqHz),
            getMockSvInfo(2, GnssConstellationType::GALILEO, 33.5, 27.5, 59.1, 166.5, kGalE1FreqHz),
            getMockSvInfo(4, GnssConstellationType::GALILEO, 28.0, 22.0, 29.0, 56.5, kGalE1FreqHz),
            getMockSvInfo(10, GnssConstellationType::GALILEO, 35.5, 25.5, 71.0, 77.0, kGalE1FreqHz),
            getMockSvInfo(29, GnssConstellationType::GALILEO, 34.1, 19.1, 28.0, 253.0, kGalE1FreqHz),
            getMockSvInfo(5, GnssConstellationType::GLONASS, 20.5, 15.5, 11.5, 116.0, kGloG1FreqHz),
            getMockSvInfo(17, GnssConstellationType::GLONASS, 21.5, 16.5, 28.5, 186.0, kGloG1FreqHz),
            getMockSvInfo(18, GnssConstellationType::GLONASS, 28.3, 25.3, 38.8, 69.0, kGloG1FreqHz),
            getMockSvInfo(10, GnssConstellationType::GLONASS, 25.0, 20.0, 66.0, 247.0, kGloG1FreqHz),
            getMockSvInfo(3, GnssConstellationType::IRNSS, 22.0, 19.7, 35.0, 112.0, kIrnssL5FreqHz),
    };
}

}  // namespace common
}  // namespace gnss
}  // namespace hardware
}  // namespace android
