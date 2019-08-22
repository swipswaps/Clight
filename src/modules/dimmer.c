#include <backlight.h>
#include "idler.h"

static int on_new_idle(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
static void dim_backlight(const double pct);
static void restore_backlight(const double pct);
static void upower_timeout_callback(void);
static void inhibit_callback(void);

static sd_bus_slot *slot;
static char client[PATH_MAX + 1];
static display_upd display_msg = { DISPLAY_UPD };

MODULE("DIMMER");

static void init(void) {
    int r = idle_init(client, &slot, conf.dimmer_timeout[state.ac_state], on_new_idle);
    if (r == 0) {
        M_SUB(UPOWER_UPD);
        M_SUB(INHIBIT_UPD);
        M_SUB(DIMMER_TO_REQ);
        
        /*
         * If dimmer is started and BACKLIGHT module is disabled, or automatic calibration is disabled,
         * we need to ensure to start from a well known backlight level.
         * Force 100% backlight level.
         */
        if (conf.no_backlight || conf.no_auto_calib) {
            set_backlight_level(1.0, 0, 0, 0);
        }
    } else {
        WARN("Failed to init.\n");
        m_poisonpill(self());
    }
}

static bool check(void) {
    return true;
}

static bool evaluate(void) {
    return conf.no_dimmer == 0 && state.ac_state != -1;
}

static void receive(const msg_t *const msg, const void* userdata) {
    if (msg->is_pubsub && msg->ps_msg->type == USER) {
        MSG_TYPE();
        switch (type) {
            case UPOWER_UPD:
                upower_timeout_callback();
                break;
            case DIMMER_TO_REQ: {
                timeout_upd *up = (timeout_upd *)msg->ps_msg->message;
                conf.dimmer_timeout[up->state] = up->new;
                if (up->state == state.ac_state) {
                    upower_timeout_callback();
                }
                }
                break;
            case INHIBIT_UPD:
                inhibit_callback();
                break;
            default:
                break;
        }
    }
}

static void destroy(void) {
    if (slot) {
        slot = sd_bus_slot_unref(slot);
    }
    idle_client_stop(client);
    idle_client_destroy(client);
}

static int on_new_idle(sd_bus_message *m, UNUSED void *userdata, UNUSED sd_bus_error *ret_error) {
    static double old_pct = -1.0;
    int dimmed;
    
    display_msg.old = state.display_state;
    
    sd_bus_message_read(m, "b", &dimmed);
    if (dimmed) {
        state.display_state |= DISPLAY_DIMMED;
        DEBUG("Entering dimmed state...\n");
        old_pct = state.current_bl_pct;
        dim_backlight(conf.dimmer_pct);
    } else if (old_pct >= 0.0) {
        state.display_state &= ~DISPLAY_DIMMED;
        DEBUG("Leaving dimmed state...\n");
        restore_backlight(old_pct);
    }
    
    display_msg.new = state.display_state;
    M_PUB(&display_msg);
    return 0;
}

static void dim_backlight(const double pct) {
    /* Don't touch backlight if a lower level is already set */
    if (pct >= state.current_bl_pct) {
        DEBUG("A lower than dimmer_pct backlight level is already set. Avoid changing it.\n");
    } else {
        set_backlight_level(pct, !conf.no_smooth_dimmer[ENTER], conf.dimmer_trans_step[ENTER], conf.dimmer_trans_timeout[ENTER]);
    }
}

/* restore previous backlight level */
static void restore_backlight(const double pct) {
    set_backlight_level(pct, !conf.no_smooth_dimmer[EXIT], conf.dimmer_trans_step[EXIT], conf.dimmer_trans_timeout[EXIT]);
}

/* Reset dimmer timeout */
static void upower_timeout_callback(void) {
    idle_set_timeout(client, conf.dimmer_timeout[state.ac_state]);
}

/*
 * If we're getting inhibited, stop idle client.
 * Else, restart it.
 */
static void inhibit_callback(void) {
    if (!state.pm_inhibited) {
        DEBUG("Being resumed.\n");
        idle_client_start(client);
    } else {
        DEBUG("Being paused.\n");
        idle_client_stop(client);
    }
}
