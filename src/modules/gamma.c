#include "bus.h"

#define GAMMA_LONG_TRANS_TIMEOUT 10         // 10s between each step with slow transitioning

static void receive_waiting_daytime(const msg_t *const msg, UNUSED const void* userdata);
static int parse_bus_reply(sd_bus_message *reply, const char *member, void *userdata);
static void set_temp(int temp, const time_t *now, int smooth, int step, int timeout);
static void ambient_callback(void);
static void on_next_dayevt(evt_upd *up);
static void on_daytime_req(temp_upd *up);
static void interface_callback(temp_upd *req);

static bool long_transitioning;
static const self_t *daytime_ref;

DECLARE_MSG(temp_msg, TEMP_UPD);

MODULE("GAMMA");

static void init(void) {
    m_ref("DAYTIME", &daytime_ref);
    M_SUB(BL_UPD);
    M_SUB(TEMP_REQ);
    M_SUB(DAYTIME_UPD);
    M_SUB(NEXT_DAYEVT_UPD);
    m_become(waiting_daytime);
}

static bool check(void) {
    /* Only on X */
    return state.display && state.xauthority;
}

static bool evaluate(void) {
    return !conf.gamma_conf.disabled;
}

static void destroy(void) {

}

static void receive_waiting_daytime(const msg_t *const msg, UNUSED const void* userdata) {
    switch (MSG_TYPE()) {
    case DAYTIME_UPD: {
        if (module_is(daytime_ref, STOPPED)) {
            /*
             * We have been notified by LOCATION that neither
             * Geoclue (not installed) nor location cache file
             * could give us any location.
             */
            WARN("Killing GAMMA as no location provider is available.\n");
            m_poisonpill(self());
        } else {
            m_unbecome();
        }
        break;
    }
    default:
        break;
    }
}

static void receive(const msg_t *const msg, UNUSED const void* userdata) {
    switch (MSG_TYPE()) {
    case BL_UPD:
        ambient_callback();
        break;
    case TEMP_REQ: {
        temp_upd *up = (temp_upd *)MSG_DATA();
        if (VALIDATE_REQ(up)) {
            if (msg->ps_msg->sender == daytime_ref) {
                on_daytime_req(up);
            } else {
                interface_callback(up);
            }
        }
        break;
    }
    case NEXT_DAYEVT_UPD: {
        evt_upd *up = (evt_upd *)MSG_DATA();
        on_next_dayevt(up);
        break;
    }
    default:
        break;
    }
}

static int parse_bus_reply(sd_bus_message *reply, const char *member, void *userdata) {
    return sd_bus_message_read(reply, "b", userdata);
}

static void set_temp(int temp, const time_t *now, int smooth, int step, int timeout) {
    int ok;
    SYSBUS_ARG_REPLY(args, parse_bus_reply, &ok, CLIGHTD_SERVICE, "/org/clightd/clightd/Gamma", "org.clightd.clightd.Gamma", "Set");
    
    /* Compute long transition steps and timeouts (if outside of event, fallback to normal transition) */
    if (conf.gamma_conf.long_transition && now && state.in_event) {
        smooth = 1;
        if (state.event_time_range == 0) {
            /* Remaining time in first half + second half of transition */
            timeout = (state.day_events[state.next_event] - *now) + conf.day_conf.event_duration;
            temp = conf.gamma_conf.temp[!state.day_time]; // use correct temp, ie the one for next event
        } else {
            /* Remaining time in second half of transition */
            timeout = conf.day_conf.event_duration - (*now - state.day_events[state.next_event]);
        }
        /* Temperature difference */
        step = abs(conf.gamma_conf.temp[DAY] - conf.gamma_conf.temp[NIGHT]);
        /* Compute each step size with a gamma_trans_timeout of 10s */
        step /= (((double)timeout) / GAMMA_LONG_TRANS_TIMEOUT);
        /* force gamma_trans_timeout to 10s (in ms) */
        timeout = GAMMA_LONG_TRANS_TIMEOUT * 1000;
        
        long_transitioning = true;
    } else {
        long_transitioning = false;
    }
        
    int r = call(&args, "ssi(buu)", state.display, state.xauthority, temp, smooth, step, timeout);    
    if (!r && ok) {
        temp_msg.temp.old = state.current_temp;
        state.current_temp = temp;
        temp_msg.temp.new = state.current_temp;
        temp_msg.temp.smooth = smooth;
        temp_msg.temp.step = step;
        temp_msg.temp.timeout = timeout;
        temp_msg.temp.daytime = state.day_time;
        M_PUB(&temp_msg);
        if (!long_transitioning && conf.gamma_conf.no_smooth) {
            INFO("%d gamma temp set.\n", temp);
        } else {
            INFO("%s transition to %d gamma temp started.\n", long_transitioning ? "Long" : "Normal", temp);
        }
    }
}

static void ambient_callback(void) {
    if (conf.gamma_conf.ambient_gamma) {
        /* 
         * Note that conf.temp is not constant (it can be changed through bus api),
         * thus we have to always compute these ones.
         */
        const int diff = abs(conf.gamma_conf.temp[DAY] - conf.gamma_conf.temp[NIGHT]);
        const int min_temp = conf.gamma_conf.temp[NIGHT] < conf.gamma_conf.temp[DAY] ? 
                            conf.gamma_conf.temp[NIGHT] : conf.gamma_conf.temp[DAY]; 
        
        const int ambient_temp = (diff * state.current_bl_pct) + min_temp;
        set_temp(ambient_temp, NULL, !conf.gamma_conf.no_smooth, 
                 conf.gamma_conf.trans_step, conf.gamma_conf.trans_timeout); // force refresh (passing NULL time_t*)
    }
}

static void on_next_dayevt(evt_upd *up) {
    static time_t last_t;                          // last time_t check_gamma() was called
    
    const time_t t = time(NULL);
    struct tm tm_now, tm_old;
    localtime_r(&t, &tm_now);
    localtime_r(&last_t, &tm_old);
    
    /* 
     * Properly reset long_transitioning when we change target event or we change current day.
     * This is needed when:
     * 1) we change target event (ie: when event_time_range == -conf.event_duration)
     * 2) we are suspended and:
     *      A) resumed with a different next_event
     *      B) resumed with same next_event but in a different day/year (well this is kinda overkill)
     */
    if (long_transitioning &&
        (tm_now.tm_yday != tm_old.tm_yday || 
        tm_now.tm_year != tm_old.tm_year)) {
        
        INFO("Long transition ended.\n");
        long_transitioning = false;
    }
        
    last_t = t;
}

static void on_daytime_req(temp_upd *up) {
    if (!long_transitioning && !conf.gamma_conf.ambient_gamma) {
        const time_t t = time(NULL);        
        set_temp(conf.gamma_conf.temp[state.day_time], &t, !conf.gamma_conf.no_smooth, 
                 conf.gamma_conf.trans_step, conf.gamma_conf.trans_timeout);
    }
}

static void interface_callback(temp_upd *req) {
    if (req->new != conf.gamma_conf.temp[req->daytime]) {
        conf.gamma_conf.temp[req->daytime] = req->new;
        if (!conf.gamma_conf.ambient_gamma && req->daytime == state.day_time) {
            set_temp(req->new, NULL, req->smooth, req->step, req->timeout); // force refresh (passing NULL time_t*)
        }
    }
}
