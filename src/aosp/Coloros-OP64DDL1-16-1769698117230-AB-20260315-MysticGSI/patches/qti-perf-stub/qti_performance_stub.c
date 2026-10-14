/* No-op com.qualcomm.qti.Performance natives for ColorOS on QEMU.
 *
 * ColorOS (built for Snapdragon) calls these natives from system_server and
 * apps (BoostFramework, app-launch feedback); without them the callers die
 * with UnsatisfiedLinkError. Qualcomm's own libqti_performance needs the perf
 * HAL and is only loaded on a real Snapdragon. Built as libcompiler_rt.so,
 * which zygote always preloads through the boot class loader (see build.sh),
 * so JNI_OnLoad registers the natives. Every call means "no boost available":
 * locks and hints are not granted (-1), feedback is 0, properties return the
 * caller's default.
 */
#include <jni.h>
#include <stddef.h>

#define UNUSED(x) (void)(x)

static jint lock_acq(JNIEnv *e, jobject o, jint h, jint d, jintArray a) { UNUSED(e); UNUSED(o); UNUSED(h); UNUSED(d); UNUSED(a); return -1; }
static jint lock_acq_rel(JNIEnv *e, jobject o, jint h, jint d, jint r, jint t, jintArray a) { UNUSED(e); UNUSED(o); UNUSED(h); UNUSED(d); UNUSED(r); UNUSED(t); UNUSED(a); return -1; }
static jint lock_rel(JNIEnv *e, jobject o, jint h) { UNUSED(e); UNUSED(o); UNUSED(h); return 0; }
static jint hint(JNIEnv *e, jobject o, jint h, jstring p, jint d, jint t) { UNUSED(e); UNUSED(o); UNUSED(h); UNUSED(p); UNUSED(d); UNUSED(t); return -1; }
static jint hint_acq_rel(JNIEnv *e, jobject o, jint pl, jint h, jstring p, jint d, jint t, jint n, jintArray a) { UNUSED(e); UNUSED(o); UNUSED(pl); UNUSED(h); UNUSED(p); UNUSED(d); UNUSED(t); UNUSED(n); UNUSED(a); return -1; }
static jint hint_rel(JNIEnv *e, jobject o, jint h) { UNUSED(e); UNUSED(o); UNUSED(h); return 0; }
static jint get_feedback(JNIEnv *e, jobject o, jint h, jstring p) { UNUSED(e); UNUSED(o); UNUSED(h); UNUSED(p); return 0; }
static jint get_feedback_extn(JNIEnv *e, jobject o, jint h, jstring p, jint n, jintArray a) { UNUSED(e); UNUSED(o); UNUSED(h); UNUSED(p); UNUSED(n); UNUSED(a); return 0; }
static void event(JNIEnv *e, jobject o, jint h, jstring p, jint n, jintArray a) { UNUSED(e); UNUSED(o); UNUSED(h); UNUSED(p); UNUSED(n); UNUSED(a); }
static jdouble get_hal_ver(JNIEnv *e, jobject o) { UNUSED(e); UNUSED(o); return 0.0; }
static jstring get_prop(JNIEnv *e, jobject o, jstring name, jstring def) { UNUSED(e); UNUSED(o); UNUSED(name); return def; }
static jint io_prefetch_start(JNIEnv *e, jobject o, jint pid, jstring p, jstring c) { UNUSED(e); UNUSED(o); UNUSED(pid); UNUSED(p); UNUSED(c); return -1; }
static jint io_prefetch_stop(JNIEnv *e, jobject o) { UNUSED(e); UNUSED(o); return -1; }
static jstring sync_request(JNIEnv *e, jobject o, jint c) { UNUSED(e); UNUSED(o); UNUSED(c); return NULL; }
static jint ux_events(JNIEnv *e, jobject o, jint op, jint pid, jstring p, jint l) { UNUSED(e); UNUSED(o); UNUSED(op); UNUSED(pid); UNUSED(p); UNUSED(l); return -1; }
static jstring ux_trigger(JNIEnv *e, jobject o, jint op) { UNUSED(e); UNUSED(o); UNUSED(op); return NULL; }


/* Also export the JNI names, in case RegisterNatives ran before the class
 * was loadable: ART then resolves the natives by symbol. */
__attribute__((visibility("default"), alias("lock_acq"))) void Java_com_qualcomm_qti_Performance_native_1perf_1lock_1acq(void);
__attribute__((visibility("default"), alias("lock_acq_rel"))) void Java_com_qualcomm_qti_Performance_native_1perf_1lock_1acq_1rel(void);
__attribute__((visibility("default"), alias("lock_rel"))) void Java_com_qualcomm_qti_Performance_native_1perf_1lock_1rel(void);
__attribute__((visibility("default"), alias("hint"))) void Java_com_qualcomm_qti_Performance_native_1perf_1hint(void);
__attribute__((visibility("default"), alias("hint_acq_rel"))) void Java_com_qualcomm_qti_Performance_native_1perf_1hint_1acq_1rel(void);
__attribute__((visibility("default"), alias("hint_acq_rel"))) void Java_com_qualcomm_qti_Performance_native_1perf_1hint_1renew(void);
__attribute__((visibility("default"), alias("hint_rel"))) void Java_com_qualcomm_qti_Performance_native_1perf_1hint_1rel(void);
__attribute__((visibility("default"), alias("get_feedback"))) void Java_com_qualcomm_qti_Performance_native_1perf_1get_1feedback(void);
__attribute__((visibility("default"), alias("get_feedback_extn"))) void Java_com_qualcomm_qti_Performance_native_1perf_1get_1feedback_1extn(void);
__attribute__((visibility("default"), alias("event"))) void Java_com_qualcomm_qti_Performance_native_1perf_1event(void);
__attribute__((visibility("default"), alias("get_hal_ver"))) void Java_com_qualcomm_qti_Performance_native_1perf_1get_1hal_1ver(void);
__attribute__((visibility("default"), alias("get_prop"))) void Java_com_qualcomm_qti_Performance_native_1perf_1get_1prop(void);
__attribute__((visibility("default"), alias("io_prefetch_start"))) void Java_com_qualcomm_qti_Performance_native_1perf_1io_1prefetch_1start(void);
__attribute__((visibility("default"), alias("io_prefetch_stop"))) void Java_com_qualcomm_qti_Performance_native_1perf_1io_1prefetch_1stop(void);
__attribute__((visibility("default"), alias("sync_request"))) void Java_com_qualcomm_qti_Performance_native_1perf_1sync_1request(void);
__attribute__((visibility("default"), alias("ux_events"))) void Java_com_qualcomm_qti_Performance_native_1perf_1uxEngine_1events(void);
__attribute__((visibility("default"), alias("ux_trigger"))) void Java_com_qualcomm_qti_Performance_native_1perf_1uxEngine_1trigger(void);

static const JNINativeMethod kMethods[] = {
    {"native_perf_lock_acq", "(II[I)I", (void *)lock_acq},
    {"native_perf_lock_acq_rel", "(IIII[I)I", (void *)lock_acq_rel},
    {"native_perf_lock_rel", "(I)I", (void *)lock_rel},
    {"native_perf_hint", "(ILjava/lang/String;II)I", (void *)hint},
    {"native_perf_hint_acq_rel", "(IILjava/lang/String;III[I)I", (void *)hint_acq_rel},
    {"native_perf_hint_renew", "(IILjava/lang/String;III[I)I", (void *)hint_acq_rel},
    {"native_perf_hint_rel", "(I)I", (void *)hint_rel},
    {"native_perf_get_feedback", "(ILjava/lang/String;)I", (void *)get_feedback},
    {"native_perf_get_feedback_extn", "(ILjava/lang/String;I[I)I", (void *)get_feedback_extn},
    {"native_perf_event", "(ILjava/lang/String;I[I)V", (void *)event},
    {"native_perf_get_hal_ver", "()D", (void *)get_hal_ver},
    {"native_perf_get_prop", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", (void *)get_prop},
    {"native_perf_io_prefetch_start", "(ILjava/lang/String;Ljava/lang/String;)I", (void *)io_prefetch_start},
    {"native_perf_io_prefetch_stop", "()I", (void *)io_prefetch_stop},
    {"native_perf_sync_request", "(I)Ljava/lang/String;", (void *)sync_request},
    {"native_perf_uxEngine_events", "(IILjava/lang/String;I)I", (void *)ux_events},
    {"native_perf_uxEngine_trigger", "(I)Ljava/lang/String;", (void *)ux_trigger},
};

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    JNIEnv *env;
    jclass cls;
    UNUSED(reserved);
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    /* Loaded by zygote before QPerformance.jar may be reachable: registering
     * is best effort, the class picks the natives up when it binds them. */
    cls = (*env)->FindClass(env, "com/qualcomm/qti/Performance");
    if (cls) {
        (*env)->RegisterNatives(env, cls, kMethods, sizeof(kMethods) / sizeof(kMethods[0]));
        (*env)->DeleteLocalRef(env, cls);
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    return JNI_VERSION_1_6;
}
