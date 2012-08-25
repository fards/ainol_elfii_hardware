/*
 * Copyright 2008, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

#include "hardware_legacy/wifi.h"
#include "hardware_legacy/wifi_nano.h"
#include "libwpa_client/wpa_ctrl.h"

#define LOG_TAG "WiFi-WifiHW"
#include "cutils/log.h"
#include "cutils/memory.h"
#include "cutils/misc.h"
#include "cutils/properties.h"
#include "private/android_filesystem_config.h"
#ifdef HAVE_LIBC_SYSTEM_PROPERTIES
#define _REALLY_INCLUDE_SYS__SYSTEM_PROPERTIES_H_
#include <sys/_system_properties.h>
#endif

static struct wpa_ctrl *ctrl_conn;
static struct wpa_ctrl *monitor_conn;

extern int do_dhcp();
extern int ifc_init();
extern void ifc_close();
extern char *dhcp_lasterror();
extern void get_dhcp_info();

static char iface[PROPERTY_VALUE_MAX];

#ifndef WIFI_DRIVER_SUPP_CONFIG_TEMPLATE
#define WIFI_DRIVER_SUPP_CONFIG_TEMPLATE "/system/etc/wifi/wpa_supplicant.conf"
#endif
#ifndef WIFI_DRIVER_SUPP_CONFIG_FILE
#define WIFI_DRIVER_SUPP_CONFIG_FILE "/data/misc/wifi/wpa_supplicant.conf"
#endif
#ifndef WIFI_DRIVER_SUPP_IFACE_DIR
#define WIFI_DRIVER_SUPP_IFACE_DIR "/data/misc/wifi/wpa_supplicant"
#endif
#ifndef WIFI_DRIVER_IFACE
#define WIFI_DRIVER_IFACE "wlan0"
#endif

#define WIFI_TEST_INTERFACE "sta"

#ifndef WIFI_DRIVER_FW_PATH_STA
#define WIFI_DRIVER_FW_PATH_STA		NULL
#endif
#ifndef WIFI_DRIVER_FW_PATH_AP
#define WIFI_DRIVER_FW_PATH_AP		NULL
#endif
#ifndef WIFI_DRIVER_FW_PATH_P2P
#define WIFI_DRIVER_FW_PATH_P2P		NULL
#endif

#ifndef WIFI_DRIVER_FW_PATH_PARAM
#define WIFI_DRIVER_FW_PATH_PARAM	"/sys/module/wlan/parameters/fwpath"
#endif

static const char SUPP_ENTROPY_FILE[]   = WIFI_ENTROPY_FILE;
static unsigned char dummy_key[21] = { 0x02, 0x11, 0xbe, 0x33, 0x43, 0x35,
                                       0x68, 0x47, 0x84, 0x99, 0xa9, 0x2b,
                                       0x1c, 0xd3, 0xee, 0xff, 0xf1, 0xe2,
                                       0xf3, 0xf4, 0xf5 };

/* This is the path of the directory which contains the communication interface
 * (socket)for the wpa_supplicant and wpa_cli. This path must be equal to the
 * value of ctrl_interface in wpa_supplicant.conf
 */
static const char IFACE_DIR[]           = WIFI_DRIVER_SUPP_IFACE_DIR;

static const char DRIVER_PROP_NAME[]    = "wlan.driver.status";
static const char SUPPLICANT_NAME[]     = "wpa_supplicant";
static const char SUPP_PROP_NAME[]      = "init.svc.wpa_supplicant";
static const char SUPP_IFACE_PROP_NAME[]= "wpa_supplicant.interface";
static const char SUPP_CONFIG_TEMPLATE[]= WIFI_DRIVER_SUPP_CONFIG_TEMPLATE;
static const char SUPP_CONFIG_FILE[]    = WIFI_DRIVER_SUPP_CONFIG_FILE;
static const char MODULE_FILE[]         = "/proc/modules";

int check_and_set_property(const char *prop_name, char *prop_val)
{
    char prop_status[PROPERTY_VALUE_MAX];
    int count;

    for(count=8;( count != 0 );count--) {
        property_set(prop_name, prop_val);
        if( property_get(prop_name, prop_status, NULL) &&
            (strcmp(prop_status, prop_val) == 0) )
        break;
    }
    if( count ) {
        LOGD("Set property %s = %s - Ok\n", prop_name, prop_val);
    }
    else {
        LOGD("Set property %s = %s - Fail\n", prop_name, prop_val);
    }
    return( count );
}

int do_dhcp_request(int *ipaddr, int *gateway, int *mask,
                    int *dns1, int *dns2, int *server, int *lease) {
    LOGE("[WLAN DEBUG] do_dhcp_request : iface[%s]", iface);

    /* For test driver, always report success */
    if (strcmp(iface, WIFI_TEST_INTERFACE) == 0)
        return 0;

    if (ifc_init() < 0)
        return -1;

    if (do_dhcp(iface) < 0) {
        ifc_close();
        return -1;
    }
    ifc_close();
    get_dhcp_info(ipaddr, gateway, mask, dns1, dns2, server, lease);
    return 0;
}

const char *get_dhcp_error_string() {
    return dhcp_lasterror();
}

const char *driver_status_str[DRV_UNKNOWN + 1] = DRV_STATUS_STRINGS;
static char errmsg_buf[128];

enum driver_status get_driver_status(void)
{
    static char fname[128], driver_status_buf[64];
    FILE *f_driver_status;
    char* pchar;
    size_t driver_status_len;
    enum driver_status drv_status = DRV_UNKNOWN;

    snprintf(fname, sizeof(fname), "%s", WIFI_DRIVER_STATUS_PATH);
    f_driver_status = fopen(fname, "r");
    if (!f_driver_status) {
        LOGE("Failed to open file %s", fname);
        return drv_status;
    }

    pchar = fgets(driver_status_buf, sizeof(driver_status_buf), f_driver_status);

    driver_status_len = strlen(driver_status_buf);
    if (pchar != NULL && driver_status_len > 1) {

        /* Remove newline */
        if (driver_status_buf[driver_status_len-1] == '\n')
            driver_status_buf[driver_status_len-1] ='\0';

        for (drv_status = DRV_UNLOADED; drv_status < DRV_UNKNOWN; drv_status++)
            if (strcmp(driver_status_buf, driver_status_str[drv_status]) == 0)
                break;
    }

    /* If an error occured, the second line may contain a detailed message. */
    if (drv_status == DRV_ERROR) {
        errmsg_buf[0] = '\0';
        fgets(errmsg_buf, sizeof(errmsg_buf), f_driver_status);
    }

    fclose(f_driver_status);
    return drv_status;
}

const char* driver_status_to_str(enum driver_status drv_status)
{
    if (drv_status > DRV_UNKNOWN)
        drv_status = DRV_UNKNOWN;
    return driver_status_str[drv_status];
}

enum driver_status wait_on_driver_status(enum driver_status final_status, int timeout)
{
    enum driver_status drv_status;

    LOGD("Waiting until driver status = %s", driver_status_to_str(final_status));
    drv_status = get_driver_status();
    while (drv_status != final_status && drv_status != DRV_ERROR && timeout > 0) {
        timeout -= TIMEOUT_STEP;
        usleep(TIMEOUT_STEP);
        drv_status = get_driver_status();
    }

    if (drv_status == DRV_ERROR) 
        LOGE("Error: %s", errmsg_buf);
    else if (drv_status != final_status)
        LOGE("Timeout on driver status = %s", driver_status_to_str(drv_status));
    return drv_status;
}

int is_wifi_driver_loaded() {
	static char strbuf[128];
	FILE *profs_entry;
    enum driver_status drv_status;
	
    drv_status = get_driver_status();	
    if (drv_status == DRV_WIFI_ON) {	//fix : wifi switch is on before shutting down
	    sprintf(strbuf, "/proc/driver/%s/status", WIFI_DRIVER_IFACE);
	    profs_entry = fopen(strbuf, "r");
	    if (profs_entry) {
	        fclose(profs_entry);
			return 0;
	    }
		else {
			drv_status = DRV_UNKNOWN;
		}
    }

	if (drv_status == DRV_WIFI_ON)
		return 1;
	else return 0;	
}

int wifi_load_driver()
{
    static char strbuf[128];
    FILE *profs_entry;
    enum driver_status drv_status;
    int ret, timeout;

    drv_status = get_driver_status();
    LOGD("wifi_load_driver, driver_status = %s", driver_status_to_str(drv_status));
    memcpy(iface, WIFI_DRIVER_IFACE, sizeof(WIFI_DRIVER_IFACE));
    
    if (drv_status == DRV_WIFI_ON) {	//fix : wifi switch is on before shutting down
	    sprintf(strbuf, "/proc/driver/%s/status", WIFI_DRIVER_IFACE);
	    profs_entry = fopen(strbuf, "r");
	    if (profs_entry) {
	        fclose(profs_entry);
			return 0;
	    }
		else {
			drv_status = DRV_UNKNOWN;
		}
    }

    //if (drv_status == DRV_WIFI_ON)
    //    return 0;

    // load the driver for the first time
    if (drv_status == DRV_UNLOADED || drv_status == DRV_UNKNOWN) {

        LOGD("wifi_load_driver: Loading nanoradio driver");
        property_set("ctl.start", "nanowifi_start");
        wait_on_driver_status(DRV_WIFI_ON, TIMEOUT_DRV_LOAD + TIMEOUT_DRV_WAKEUP);
    }

    sprintf(strbuf, "/proc/driver/%s/status", WIFI_DRIVER_IFACE);
    profs_entry = fopen(strbuf, "r");
    if (profs_entry) {
        fclose(profs_entry);
        check_and_set_property(DRIVER_PROP_NAME, "ok");
        return 0;
    }
    else {
        LOGE("wifi_load_driver failed to start the driver!");
        check_and_set_property(DRIVER_PROP_NAME, "unloaded");
        property_set("ctl.start", "nanowifi_stop");
        wait_on_driver_status(DRV_UNLOADED, TIMEOUT_DRV_UNLOAD);
        return -1;
    }
}

int wifi_unload_driver()
{
    enum driver_status drv_status;
    int timeout;

    drv_status = get_driver_status();
    LOGD("wifi_unload_driver, driver_status = %s", driver_status_to_str(drv_status));

    if (drv_status != DRV_WIFI_ON)
        return 0;

    property_set("ctl.start", "nanowifi_stop");
    drv_status = wait_on_driver_status(DRV_UNLOADED, TIMEOUT_DRV_UNLOAD);;
    return (drv_status == DRV_UNLOADED) ? 0 : -1;
}

int ensure_config_file_exists()
{
    char buf[2048];
    int srcfd, destfd;
    int nread;

    if (access(SUPP_CONFIG_FILE, R_OK|W_OK) == 0) {
        return 0;
    } else if (errno != ENOENT) {
        LOGE("Cannot access \"%s\": %s", SUPP_CONFIG_FILE, strerror(errno));
        return -1;
    }

    srcfd = open(SUPP_CONFIG_TEMPLATE, O_RDONLY);
    if (srcfd < 0) {
        LOGE("Cannot open \"%s\": %s", SUPP_CONFIG_TEMPLATE, strerror(errno));
        return -1;
    }

    destfd = open(SUPP_CONFIG_FILE, O_CREAT|O_WRONLY, 0660);
    if (destfd < 0) {
        close(srcfd);
        LOGE("Cannot create \"%s\": %s", SUPP_CONFIG_FILE, strerror(errno));
        return -1;
    }

    while ((nread = read(srcfd, buf, sizeof(buf))) != 0) {
        if (nread < 0) {
            LOGE("Error reading \"%s\": %s", SUPP_CONFIG_TEMPLATE, strerror(errno));
            close(srcfd);
            close(destfd);
            unlink(SUPP_CONFIG_FILE);
            return -1;
        }
        write(destfd, buf, nread);
    }

    close(destfd);
    close(srcfd);

    /* chmod is needed because open() didn't set permisions properly */
    if (chmod(SUPP_CONFIG_FILE, 0660) < 0) {
        LOGE("Error changing permissions of %s to 0660: %s",
             SUPP_CONFIG_FILE, strerror(errno));
        unlink(SUPP_CONFIG_FILE);
        return -1;
    }

    if (chown(SUPP_CONFIG_FILE, AID_SYSTEM, AID_WIFI) < 0) {
        LOGE("Error changing group ownership of %s to %d: %s",
             SUPP_CONFIG_FILE, AID_WIFI, strerror(errno));
        unlink(SUPP_CONFIG_FILE);
        return -1;
    }
    return 0;
}

int wifi_start_p2p_supplicant()
{
	return -1;
}

int wifi_start_supplicant()
{
    char supp_status[PROPERTY_VALUE_MAX] = {'\0'};
    int count = 200; /* wait at most 20 seconds for completion */
#ifdef HAVE_LIBC_SYSTEM_PROPERTIES
    const prop_info *pi;
    unsigned serial = 0;
#endif

    /* Check whether already running */
    if (property_get(SUPP_PROP_NAME, supp_status, NULL)
            && strcmp(supp_status, "running") == 0) {
        return 0;
    }

    /* Before starting the daemon, make sure its config file exists */
    if (ensure_config_file_exists() < 0) {
        LOGE("Wi-Fi will not be enabled");
        return -1;
    }

    /* Clear out any stale socket files that might be left over. */
    wpa_ctrl_cleanup();

#ifdef HAVE_LIBC_SYSTEM_PROPERTIES
    /*
     * Get a reference to the status property, so we can distinguish
     * the case where it goes stopped => running => stopped (i.e.,
     * it start up, but fails right away) from the case in which
     * it starts in the stopped state and never manages to start
     * running at all.
     */
    pi = __system_property_find(SUPP_PROP_NAME);
    if (pi != NULL) {
        serial = pi->serial;
    }
#endif
    property_set("ctl.start", SUPPLICANT_NAME);
    sched_yield();

    while (count-- > 0) {
 #ifdef HAVE_LIBC_SYSTEM_PROPERTIES
        if (pi == NULL) {
            pi = __system_property_find(SUPP_PROP_NAME);
        }
        if (pi != NULL) {
            __system_property_read(pi, NULL, supp_status);
            if (strcmp(supp_status, "running") == 0) {
                return 0;
            } else if (pi->serial != serial &&
                    strcmp(supp_status, "stopped") == 0) {
                return -1;
            }
        }
#else
        if (property_get(SUPP_PROP_NAME, supp_status, NULL)) {
            if (strcmp(supp_status, "running") == 0)
                return 0;
        }
#endif
        usleep(100000);
    }
    return -1;
}

int wifi_stop_supplicant()
{
    char supp_status[PROPERTY_VALUE_MAX] = {'\0'};
    int count = 50; /* wait at most 5 seconds for completion */

    /* Check whether supplicant already stopped */
    if (property_get(SUPP_PROP_NAME, supp_status, NULL)
        && strcmp(supp_status, "stopped") == 0) {
        return 0;
    }

    property_set("ctl.stop", SUPPLICANT_NAME);
    sched_yield();

    while (count-- > 0) {
        if (property_get(SUPP_PROP_NAME, supp_status, NULL)) {
            if (strcmp(supp_status, "stopped") == 0)
                return 0;
        }
        usleep(100000);
    }
    return -1;
}

int wifi_connect_to_supplicant()
{
    char ifname[256];
    char supp_status[PROPERTY_VALUE_MAX] = {'\0'};
    int  supplicant_timeout = TIMEOUT_SUPPLICANT;

    /* Make sure supplicant is running */
    if (!property_get(SUPP_PROP_NAME, supp_status, NULL)
            || strcmp(supp_status, "running") != 0) {
        LOGE("Supplicant not running, cannot connect");
        return -1;
    }

    if (access(IFACE_DIR, F_OK) == 0) {
        snprintf(ifname, sizeof(ifname), "%s/%s", IFACE_DIR, iface);
    } else {
        strlcpy(ifname, iface, sizeof(ifname));
    }

    ctrl_conn = wpa_ctrl_open(ifname);
    while (ctrl_conn == NULL && supplicant_timeout > 0) {
        usleep(TIMEOUT_STEP);
        supplicant_timeout -= TIMEOUT_STEP;
        ctrl_conn = wpa_ctrl_open(ifname);
    }
    if (ctrl_conn == NULL) {
        LOGE("Unable to open connection to supplicant on \"%s\": %s",
             ifname, strerror(errno));
        return -1;
    }
    monitor_conn = wpa_ctrl_open(ifname);
    if (monitor_conn == NULL) {
        wpa_ctrl_close(ctrl_conn);
        ctrl_conn = NULL;
        return -1;

    }
    if (wpa_ctrl_attach(monitor_conn) != 0) {
        wpa_ctrl_close(monitor_conn);
        wpa_ctrl_close(ctrl_conn);
        ctrl_conn = monitor_conn = NULL;
        return -1;
    }
    return 0;
}

int wifi_send_command(struct wpa_ctrl *ctrl, const char *cmd, char *reply, size_t *reply_len)
{
    int ret;

    if (ctrl_conn == NULL) {
        LOGV("Not connected to wpa_supplicant - \"%s\" command dropped.\n", cmd);
        return -1;
    }
    ret = wpa_ctrl_request(ctrl, cmd, strlen(cmd), reply, reply_len, NULL);
	LOGD("wifi.c : cmd=%s, reply=%s\n", cmd, reply);

	if (strcmp(cmd, "DRIVER START") == 0)
	{
		LOGD("wifi.c : load driver after resume\n");
		wifi_load_driver();
	}
	if (strcmp(cmd, "DRIVER STOP") == 0)
	{
		LOGD("wifi.c : unload driver before suspend\n");
		wifi_unload_driver();
	}
	
    if (ret == -2) {
        LOGD("'%s' command timed out.\n", cmd);
        return -2;
    } else if (ret < 0 || strncmp(reply, "FAIL", 4) == 0) {
        return -1;
    }
    if (strncmp(cmd, "PING", 4) == 0) {
        reply[*reply_len] = '\0';
    }
    return 0;
}

int wifi_wait_for_event(char *buf, size_t buflen)
{
    size_t nread = buflen - 1;
    int fd;
    fd_set rfds;
    int result;
    struct timeval tval;
    struct timeval *tptr;
    
    if (monitor_conn == NULL) {
        LOGD("Connection closed\n");
        strncpy(buf, WPA_EVENT_TERMINATING " - connection closed", buflen-1);
        buf[buflen-1] = '\0';
        return strlen(buf);
    }

    result = wpa_ctrl_recv(monitor_conn, buf, &nread);
    if (result < 0) {
        LOGD("wpa_ctrl_recv failed: %s\n", strerror(errno));
        strncpy(buf, WPA_EVENT_TERMINATING " - recv error", buflen-1);
        buf[buflen-1] = '\0';
        return strlen(buf);
    }
    buf[nread] = '\0';
    /* LOGD("wait_for_event: result=%d nread=%d string=\"%s\"\n", result, nread, buf); */
    /* Check for EOF on the socket */
    if (result == 0 && nread == 0) {
        /* Fabricate an event to pass up */
        LOGD("Received EOF on supplicant socket\n");
        strncpy(buf, WPA_EVENT_TERMINATING " - signal 0 received", buflen-1);
        buf[buflen-1] = '\0';
        return strlen(buf);
    }
    /*
     * Events strings are in the format
     *
     *     <N>CTRL-EVENT-XXX 
     *
     * where N is the message level in numerical form (0=VERBOSE, 1=DEBUG,
     * etc.) and XXX is the event name. The level information is not useful
     * to us, so strip it off.
     */
    if (buf[0] == '<') {
        char *match = strchr(buf, '>');
        if (match != NULL) {
            nread -= (match+1-buf);
            memmove(buf, match+1, nread+1);
        }
    }
    return nread;
}

void wifi_close_supplicant_connection()
{
    if (ctrl_conn != NULL) {
        wpa_ctrl_close(ctrl_conn);
        ctrl_conn = NULL;
    }
    if (monitor_conn != NULL) {
        wpa_ctrl_close(monitor_conn);
        monitor_conn = NULL;
    }
}

int wifi_command(const char *command, char *reply, size_t *reply_len)
{
    LOGV("[WLAN DEBUG] wifi_command [%s]", command);
    return wifi_send_command(ctrl_conn, command, reply, reply_len);
}

const char *wifi_get_fw_path(int fw_type)
{
    switch (fw_type) {
    case WIFI_GET_FW_PATH_STA:
        return WIFI_DRIVER_FW_PATH_STA;
    case WIFI_GET_FW_PATH_AP:
        return WIFI_DRIVER_FW_PATH_AP;
    case WIFI_GET_FW_PATH_P2P:
        return WIFI_DRIVER_FW_PATH_P2P;
    }
    return NULL;
}

int wifi_change_fw_path(const char *fwpath)
{
    int len;
    int fd;
    int ret = 0;

    if (!fwpath)
        return ret;
    fd = open(WIFI_DRIVER_FW_PATH_PARAM, O_WRONLY);
    if (fd < 0) {
        LOGE("Failed to open wlan fw path param (%s)", strerror(errno));
        return -1;
    }
    len = strlen(fwpath) + 1;
    if (write(fd, fwpath, len) != len) {
        LOGE("Failed to write wlan fw path param (%s)", strerror(errno));
        ret = -1;
    }
    close(fd);
    return ret;
}


int ensure_entropy_file_exists()
{
    int ret;
    int destfd;

    ret = access(SUPP_ENTROPY_FILE, R_OK|W_OK);
    if ((ret == 0) || (errno == EACCES)) {
        if ((ret != 0) &&
            (chmod(SUPP_ENTROPY_FILE, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP) != 0)) {
            LOGE("Cannot set RW to \"%s\": %s", SUPP_ENTROPY_FILE, strerror(errno));
            return -1;
        }
        return 0;
    }
    destfd = open(SUPP_ENTROPY_FILE, O_CREAT|O_RDWR, 0660);
    if (destfd < 0) {
        LOGE("Cannot create \"%s\": %s", SUPP_ENTROPY_FILE, strerror(errno));
        return -1;
    }

    if (write(destfd, dummy_key, sizeof(dummy_key)) != sizeof(dummy_key)) {
        LOGE("Error writing \"%s\": %s", SUPP_ENTROPY_FILE, strerror(errno));
        close(destfd);
        return -1;
    }
    close(destfd);

    /* chmod is needed because open() didn't set permisions properly */
    if (chmod(SUPP_ENTROPY_FILE, 0660) < 0) {
        LOGE("Error changing permissions of %s to 0660: %s",
             SUPP_ENTROPY_FILE, strerror(errno));
        unlink(SUPP_ENTROPY_FILE);
        return -1;
    }

    if (chown(SUPP_ENTROPY_FILE, AID_SYSTEM, AID_WIFI) < 0) {
        LOGE("Error changing group ownership of %s to %d: %s",
             SUPP_ENTROPY_FILE, AID_WIFI, strerror(errno));
        unlink(SUPP_ENTROPY_FILE);
        return -1;
    }
    return 0;
}


