#ifndef _WIFI_NANO_H
#define _WIFI_NANO_H

#if __cplusplus
extern "C" {
#endif

enum driver_status {
     DRV_UNLOADED = 0, /* nano driver modules not loaded */
     DRV_SLEEPING,     /* driver in shutdown to save power */
     DRV_WIFI_ON,      /* driver operational, WiFi client mode */
     DRV_SOFT_AP,      /* driver operational, Soft AP mode */
     DRV_ERROR,        /* error occured */
     DRV_UNKNOWN       /* status query failed (must be the last enum value) */
};

/* Driver status text representation
 * These strings must be used by the scripts startNano.sh, stopNano.sh,
 * wakeNano.sh, sleepNano.sh start_softap.sh and stop_softap.sh
 * to update the driver status textfile. */
#define DRV_STATUS_STRINGS { \
    "unloaded", \
    "sleeping", \
    "WiFi on",  \
    "Soft AP",  \
    "error",    \
    "unknown"   }

enum driver_status get_driver_status(void);
const char* driver_status_to_str(enum driver_status drv_status);
enum driver_status wait_on_driver_status(enum driver_status final_status, int timeout);

/* Driver status query is periodic.
 * This defines the period in microseconds. */
#define TIMEOUT_STEP          100000

/* Timeouts related to the WiFi client mode (in microseconds) */
#define TIMEOUT_DRV_LOAD     4000000 /* Driver modules loading (service nanowifi_start) */
#define TIMEOUT_DRV_WAKEUP   1000000 /* Shutdown state exit (service nanowifi_wake) */
#define TIMEOUT_DRV_SLEEP    1000000 /* Shutdown state entry (service nanowifi_sleep) */
#define TIMEOUT_DRV_UNLOAD   1000000 /* Driver modules removal (service nanowifi_stop) */
#define TIMEOUT_SUPPLICANT   1500000 /* Driver - WPA supplicant interface creation */

/* Timeouts related to the Soft AP  mode (in microseconds) */
#define TIMEOUT_SOFTAP_START 5000000 /* Soft AP mode entry (service nano_start_sap) */
#define TIMEOUT_SOFTAP_STOP  1500000 /* Soft AP mode exit (service nano_stop_sap) */

#if __cplusplus
}
#endif

#endif _WIFI_NANO_H

