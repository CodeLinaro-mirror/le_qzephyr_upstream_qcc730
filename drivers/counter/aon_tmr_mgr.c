/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "aon_tmr_mgr.h"
#include <zephyr/irq.h>


/*============================================================================
 * Module-Private Global Variables
 *==========================================================================*/

/** 
 * @brief Array of timer control blocks.
 * @warning This array MUST be placed in Retention RAM to survive deep sleep cycles.
 *          Example GCC attribute: __attribute__((section(".retention_data")))
 */
static aon_timer_t aon_clients[AON_CLIENT_MAX];

/** Global function pointer for the system's microsecond time source. Must be initialized. */
static get_abs_us_fn get_current_us;


/*============================================================================
 * Public API Functions
 *==========================================================================*/

/**
 * @brief Initializes the AON timer manager. Must be called once on system startup.
 * @param us_provider A function pointer that returns the current absolute system time in microseconds.
 */
void aon_manager_init(get_abs_us_fn us_provider) {
    get_current_us = us_provider;
    for (int i = 0; i < AON_CLIENT_MAX; i++) {
        aon_clients[i].enabled = false;
        aon_clients[i].callback = NULL;
        aon_clients[i].user_data = NULL;
        aon_clients[i].expiry_abs_us = 0;
    }
}

/**
 * @brief Registers a callback function for a timer client.
 * @param id The client identifier.
 * @param cb The function to call when the timer expires.
 * @param data A pointer to context data to be passed to the callback.
 */
void aon_timer_register(aon_client_id_t id, aon_expiry_fn cb, void *data) {
    if (id >= AON_CLIENT_MAX) return;
    unsigned int key = irq_lock();
    aon_clients[id].callback = cb;
    aon_clients[id].user_data = data;
    irq_unlock(key);
}

/**
 * @brief Deregisters (clears) the callback function for a timer client.
 * @param id The client identifier.
 */
void aon_timer_deregister(aon_client_id_t id) {
    if (id >= AON_CLIENT_MAX) return;
    unsigned int key = irq_lock();
    aon_clients[id].callback = NULL;
    aon_clients[id].user_data = NULL;
    irq_unlock(key);
}

/**
 * @brief Sets a timer to fire after a relative duration from "now".
 *
 * @param id The client identifier.
 * @param duration_us The relative duration in microseconds from the moment this function is called.
 */
void aon_timer_set(aon_client_id_t id, uint64_t duration_us) {
    if (id >= AON_CLIENT_MAX || get_current_us == NULL) return;
    
    unsigned int key = irq_lock();
    
    uint64_t now = get_current_us();
    /*Calculate and store the absolute expiry time.*/
    aon_clients[id].expiry_abs_us = now + duration_us;
    aon_clients[id].enabled = true;
    /* early_printk("now=%u us, duration_us %u \r\n", (uint32_t)now,(uint32_t)duration_us);*/
    irq_unlock(key);
}

/**
 * @brief Manually disables a timer, preventing it from being considered for sleep
 *        or from firing its callback.
 * @param id The client identifier.
 */
void aon_timer_disable(aon_client_id_t id) {
    if (id >= AON_CLIENT_MAX) return;
    unsigned int key = irq_lock();
    aon_clients[id].enabled = false;
    irq_unlock(key);
}

/**
 * @brief Disables all active AON timers.
 * 
 * @details This function iterates through all timer clients and disables them,
 *          effectively canceling all pending timers. This can be useful during
 *          system shutdown, power mode transitions, or when needing to reset
 *          the timer subsystem.
 */
void aon_disable_all_timers(void) {
    unsigned int key = irq_lock();
    
    for (int i = 0; i < AON_CLIENT_MAX; i++) {
        aon_clients[i].enabled = false;
    }
    
    irq_unlock(key);
}

/**
 * @brief Gets the minimum expiry time and the corresponding client ID.
 * @details This function calculates the time to the next event and identifies which client
 *          has the earliest expiry time. It corrects for any processing delay between
 *          `aon_timer_set` and the actual call to this function.
 * @return A structure containing both the sleep duration in microseconds and the client ID.
 */
void aon_get_min_expiry(aon_sleep_info_t* info) {

    if (get_current_us == NULL || info == NULL) return;
    info->client_id = AON_CLIENT_NONE;
    info->sleep_us = 0;

    unsigned int key = irq_lock();

    uint64_t min_expiry = UINT64_MAX;
    bool any_active = false;

    /* 1. Find the earliest absolute expiry time among all active timers. */
    for (int i = 0; i < AON_CLIENT_MAX; i++) {
        if (aon_clients[i].enabled) {
            any_active = true;
            if (aon_clients[i].expiry_abs_us < min_expiry) {
                min_expiry = aon_clients[i].expiry_abs_us;
                info->client_id = (aon_client_id_t)i;
            }
        }
    }

    if (!any_active) {
        irq_unlock(key);
        return ; /*No active timers, return defaults.*/
    }
    
    /* 2. Get the current time again, just before calculating the final delta.*/
    uint64_t now = get_current_us();

  
    /*3. Calculate the delta between "now" and the earliest expiry time.*/ 
    int64_t delta_us = min_expiry - now;
    
    /*early_printk("now=%u us, min_expiry %u %d delta %u\r\n", (uint32_t)now,(uint32_t)min_expiry,info->client_id, (uint32_t)delta_us);*/ 
    irq_unlock(key);

    /* 4. Handle boundary conditions.*/
    if (delta_us <= 0) {
        /*The timer has already expired or is about to. Return a minimal value.*/ 
        info->sleep_us = 0;
        return ;
    }

    /* Ensure the value fits within the 32-bit hardware timer register.
    TODO: delta_us could > UINT32_MAX */
    if (delta_us > UINT32_MAX) {
        info->sleep_us = UINT32_MAX;
    } else {
        info->sleep_us = (uint32_t)delta_us;
    }
    
    return;
}



/**
 * @brief Checks for and dispatches callbacks for any expired timers.
 * @details This function should be called upon waking from sleep. It compares the
 *          current time against the stored expiry times for all active timers.
 */
void aon_update_and_dispatch(void) {
    if (get_current_us == NULL) return;

    unsigned int key = irq_lock();
    
    uint64_t now = get_current_us();

    /* Iterate to find and dispatch expired timers. */
    for (int i = 0; i < AON_CLIENT_MAX; i++) {
        if (aon_clients[i].enabled && aon_clients[i].expiry_abs_us <= now) {
            
            /* Timer has expired, execute callback if it exists.*/
            if (aon_clients[i].callback != NULL) {
                /* IMPORTANT: The callback runs in the current context (e.g., ISR).
                 It must be designed to be fast and non-blocking. */
                aon_clients[i].callback(aon_clients[i].user_data);
            }

            /* Once the timer's task is done, disable it until it is explicitly set again. */
            aon_clients[i].enabled = false;
        }
    }

    irq_unlock(key);
}


/**
 * @brief Processes only the specific client that was expected to wake up the system.
 * 
 * @details This function is an optimization for the wakeup path. When the system wakes up,
 *          instead of checking all timers, it only checks and processes the one that was
 *          identified as the earliest by the previous call to aon_get_min_expiry().
 *          It verifies that the timer has actually expired before executing its callback.
 * 
 * @param client_id The ID of the client to check and potentially process.
 * @return true if the client's callback was executed, false otherwise.
 */
bool aon_process_specific_client(aon_client_id_t client_id, uint64_t* slp_back) {
    if (get_current_us == NULL || client_id >= AON_CLIENT_MAX || client_id == AON_CLIENT_NONE) {
        return false;
    }

    unsigned int key = irq_lock();
    *slp_back = 0;
    bool callback_executed = false;
    uint64_t now = get_current_us();
    
    /* Check if this specific client's timer has expired*/
    if (aon_clients[client_id].enabled && aon_clients[client_id].expiry_abs_us <= now) {
        /* Timer has expired, execute callback if it exists */
        if (aon_clients[client_id].callback != NULL) {
            /* Execute the callback in the current context */
            *slp_back = aon_clients[client_id].callback(aon_clients[client_id].user_data);
            callback_executed = true;
        }

        /* Mark the timer as processed */
        aon_clients[client_id].enabled = false;
    }
    
    irq_unlock(key);

    if(*slp_back !=0 ) {
        aon_timer_set(client_id,(uint64_t)*slp_back);
    }
    
    return callback_executed;
}
