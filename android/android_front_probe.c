/* VI bridge for an activity-owned ANativeWindow. Inert window tokens are
 * restricted to the explicit headless diagnostic mode. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <android/native_window.h>
#include <pthread.h>
#include <time.h>

extern _Noreturn void runtime_fail(const char *message);
extern void hle_bind_system_event(void *event, int autoclear);
extern void hle_signal_system_event(void *event);
static unsigned display_token, layer_token, window_token;
static pthread_mutex_t window_lock=PTHREAD_MUTEX_INITIALIZER;
static ANativeWindow *activity_window;
static void *vsync_events[16];
static unsigned vsync_count;
static int activity_focused=1;

extern void runtime_oe_push_message(uint32_t message);
void android_front_set_focus(int focused) {
    pthread_mutex_lock(&window_lock);activity_focused=focused;pthread_mutex_unlock(&window_lock);
    runtime_oe_push_message(15); /* FocusStateChanged, as the macOS frontend sends */
}

int runtime_front_focus(void) {
    pthread_mutex_lock(&window_lock);int focused=activity_focused;pthread_mutex_unlock(&window_lock);
    return focused;
}

static uint64_t last_tick_ns, tick_period_ns;

/* Refresh rate measured from Choreographer callbacks (0 until known). */
int android_front_display_hz(void) {
    pthread_mutex_lock(&window_lock);uint64_t period=tick_period_ns;pthread_mutex_unlock(&window_lock);
    return period?(int)((1000000000ull+period/2)/period):0;
}

int android_front_has_window(void) {
    pthread_mutex_lock(&window_lock);int has=activity_window!=NULL;pthread_mutex_unlock(&window_lock);
    return has;
}

void android_front_frame_tick(void) {
    struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
    uint64_t now=(uint64_t)ts.tv_sec*1000000000ull+(uint64_t)ts.tv_nsec;
    pthread_mutex_lock(&window_lock);
    if(last_tick_ns&&now-last_tick_ns<100000000ull) {
        uint64_t d=now-last_tick_ns; /* smoothed; a skipped callback is ignored */
        if(!tick_period_ns)tick_period_ns=d;
        else if(d<tick_period_ns*3/2)tick_period_ns=(tick_period_ns*7+d)/8;
    }
    last_tick_ns=now;
    if (activity_window) for (unsigned i=0;i<vsync_count;++i)
        hle_signal_system_event(vsync_events[i]);
    pthread_mutex_unlock(&window_lock);
}

void android_front_set_window(ANativeWindow *window) {
    pthread_mutex_lock(&window_lock);
    if (window) ANativeWindow_acquire(window);
    ANativeWindow *old=activity_window;
    activity_window=window;
    pthread_mutex_unlock(&window_lock);
    if (old) ANativeWindow_release(old);
}

static ANativeWindow *current_window(void) {
    pthread_mutex_lock(&window_lock);
    ANativeWindow *window=activity_window;
    pthread_mutex_unlock(&window_lock);
    return window;
}

ANativeWindow *android_front_acquire_window(void) {
    pthread_mutex_lock(&window_lock);
    ANativeWindow *window=activity_window;
    if(window)ANativeWindow_acquire(window);
    pthread_mutex_unlock(&window_lock);
    return window;
}

static void vi_initialize(void) {
    if (current_window()) {
        fprintf(stderr, "[android-front] VI initialized with Android native window\n");
    } else {
        if (!getenv("TOTK_PROBE_HEADLESS_VI")) runtime_fail("Android VI requires an activity window");
        fprintf(stderr, "[android-front] diagnostic VI initialized without a display\n");
    }
}
static uint32_t vi_open(void **out) {
    if (!out) return 0xf601;
    *out=&display_token;
    return 0;
}
static uint32_t vi_layer(void **out, void *display) {
    if (!out || display!=&display_token) return 0xf601;
    *out=&layer_token;
    return 0;
}
static uint32_t vi_window(void **out, void *layer) {
    if (!out || layer!=&layer_token) return 0xf601;
    ANativeWindow *window=current_window();
    if (!window && !getenv("TOTK_PROBE_HEADLESS_VI")) return 0xf601;
    *out=window ? (void *)window : (void *)&window_token;
    fprintf(stderr, "[android-front] %s native window returned\n",window?"Android":"diagnostic");
    return 0;
}
static uint32_t vi_vsync(void *event, void *display) {
    if (!event || display!=&display_token) return 0xf601;
    hle_bind_system_event(event, 1);
    pthread_mutex_lock(&window_lock);
    unsigned i=0;while(i<vsync_count&&vsync_events[i]!=event)++i;
    if(i==vsync_count) {
        if(vsync_count==16) runtime_fail("Android VI vsync event limit");
        vsync_events[vsync_count++]=event;
    }
    int real_window=activity_window!=NULL;
    pthread_mutex_unlock(&window_lock);
    fprintf(stderr, "[android-front] VI vsync event bound (%s)\n",
        real_window?"Android Choreographer":"headless diagnostic, no frame timer");
    return 0;
}

void *runtime_front_override(const char *name) {
    if (!current_window() && !getenv("TOTK_PROBE_HEADLESS_VI")) return NULL;
    if (!strcmp(name,"_ZN2nn2vi10InitializeEv")) return vi_initialize;
    if (!strcmp(name,"_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE")) return vi_open;
    if (!strcmp(name,"_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE")) return vi_layer;
    if (!strcmp(name,"_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE")) return vi_window;
    if (!strcmp(name,"_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE")) return vi_vsync;
    return NULL;
}
