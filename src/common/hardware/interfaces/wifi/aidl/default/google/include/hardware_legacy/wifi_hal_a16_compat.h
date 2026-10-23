#pragma once
/* A16 wifi HAL types missing from the AOSPA uvite wifi_hal.h snapshot.
 * RTT v3/v4 structs live at the end of rtt.h (they need wifi_rtt_config). */

#ifndef WIFI_FEATURE_SET_VOIP_MODE
#define WIFI_FEATURE_SET_VOIP_MODE        (uint64_t)0x800000000
#define WIFI_FEATURE_MLO_SAP              (uint64_t)0x1000000000
#define WIFI_FEATURE_MULTIPLE_MLD_ON_SAP  (uint64_t)0x2000000000
#define WIFI_FEATURE_ROAMING_MODE_CONTROL (uint64_t)0x4000000000
#define WIFI_FEATURE_CACHED_SCAN_RESULTS  (uint64_t)0x8000000000
#endif

#ifndef NAN_PAIRING_BOOTSTRAPPING_NONE_MASK
#define NAN_PAIRING_BOOTSTRAPPING_NONE_MASK 0x00
#endif

#ifndef RTT_MAX_COOKIE_LEN
#define RTT_MAX_COOKIE_LEN 255
#endif
#ifndef RTT_SECURITY_MAX_PASSPHRASE_LEN
#define RTT_SECURITY_MAX_PASSPHRASE_LEN 63
#endif
#ifndef PMKID_LEN
#define PMKID_LEN 16
#endif

#ifndef WPA_CIPHER_NONE
enum {
    WPA_CIPHER_NONE = 0,
    WPA_CIPHER_CCMP_128 = 1,
    WPA_CIPHER_CCMP_256 = 2,
    WPA_CIPHER_GCMP_128 = 3,
    WPA_CIPHER_GCMP_256 = 4,
};
#endif

#ifndef WPA_KEY_MGMT_NONE
enum {
    WPA_KEY_MGMT_NONE = 0,
    WPA_KEY_MGMT_PASN = 1,
    WPA_KEY_MGMT_SAE = 2,
    WPA_KEY_MGMT_EAP_FILS_SHA256 = 3,
    WPA_KEY_MGMT_EAP_FILS_SHA384 = 4,
    WPA_KEY_MGMT_EAP_FT_SHA256 = 5,
    WPA_KEY_MGMT_EAP_FT_SHA384 = 6,
    WPA_KEY_MGMT_FT_PSK_SHA256 = 7,
    WPA_KEY_MGMT_FT_PSK_SHA384 = 8,
};
#endif

typedef int wifi_rtt_akm;
typedef int wifi_rtt_cipher_suite;

typedef struct {
    int is_twt_requester_supported;
    int is_twt_responder_supported;
    int is_broadcast_twt_supported;
    int is_flexible_twt_supported;
    int min_wake_duration_micros;
    int max_wake_duration_micros;
    int min_wake_interval_micros;
    int max_wake_interval_micros;
} wifi_twt_capabilities;

typedef enum {
    WIFI_TWT_ERROR_NONE = 0,
    WIFI_TWT_ERROR_CODE_TIMEOUT = 1,
    WIFI_TWT_ERROR_CODE_PEER_REJECTED = 2,
    WIFI_TWT_ERROR_CODE_PEER_NOT_SUPPORTED = 3,
    WIFI_TWT_ERROR_CODE_NOT_SUPPORTED = 4,
    WIFI_TWT_ERROR_CODE_NOT_AVAILABLE = 5,
    WIFI_TWT_ERROR_CODE_MAX_SESSION_REACHED = 6,
    WIFI_TWT_ERROR_CODE_INVALID_PARAMS = 7,
    WIFI_TWT_ERROR_CODE_ALREADY_SUSPENDED = 8,
    WIFI_TWT_ERROR_CODE_ALREADY_RESUMED = 9,
} wifi_twt_error_code;

typedef struct {
    int mlo_link_id;
    int min_wake_duration_micros;
    int max_wake_duration_micros;
    int min_wake_interval_micros;
    int max_wake_interval_micros;
} wifi_twt_request;

typedef enum {
    WIFI_TWT_NEGO_TYPE_INDIVIDUAL = 0,
    WIFI_TWT_NEGO_TYPE_BROADCAST = 1,
} wifi_twt_negotiation_type;

typedef struct {
    int session_id;
    int mlo_link_id;
    int wake_duration_micros;
    int wake_interval_micros;
    wifi_twt_negotiation_type negotiation_type;
    int is_trigger_enabled;
    int is_announced;
    int is_implicit;
    int is_protected;
    int is_updatable;
    int is_suspendable;
    int is_responder_pm_mode_enabled;
} wifi_twt_session;

typedef struct {
    int avg_pkt_num_tx;
    int avg_pkt_num_rx;
    int avg_tx_pkt_size;
    int avg_rx_pkt_size;
    int avg_eosp_dur_us;
    int eosp_count;
} wifi_twt_session_stats;

typedef enum {
    WIFI_TWT_TEARDOWN_REASON_CODE_UNKNOWN = 0,
    WIFI_TWT_TEARDOWN_REASON_CODE_LOCALLY_REQUESTED = 1,
    WIFI_TWT_TEARDOWN_REASON_CODE_INTERNALLY_INITIATED = 2,
    WIFI_TWT_TEARDOWN_REASON_CODE_PEER_INITIATED = 3,
} wifi_twt_teardown_reason_code;

typedef struct {
    void (*on_twt_failure)(wifi_request_id id, wifi_twt_error_code error_code);
    void (*on_twt_session_create)(wifi_request_id id, wifi_twt_session twt_session);
    void (*on_twt_session_update)(wifi_request_id id, wifi_twt_session twt_session);
    void (*on_twt_session_teardown)(wifi_request_id id, int twt_session_id,
                                    wifi_twt_teardown_reason_code reason_code);
    void (*on_twt_session_stats)(wifi_request_id id, int twt_session_id,
                                 wifi_twt_session_stats stats);
    void (*on_twt_session_suspend)(wifi_request_id id, int twt_session_id);
    void (*on_twt_session_resume)(wifi_request_id id, int twt_session_id);
} wifi_twt_events;

struct wifi_rtt_result_v3;
struct wifi_rtt_result_v4;
typedef struct {
    void (*on_rtt_results_v3)(wifi_request_id id, unsigned num_results,
                              struct wifi_rtt_result_v3* results[]);
} wifi_rtt_event_handler_v3;
typedef struct {
    void (*on_rtt_results_v4)(wifi_request_id id, unsigned num_results,
                              struct wifi_rtt_result_v4* results[]);
} wifi_rtt_event_handler_v4;
