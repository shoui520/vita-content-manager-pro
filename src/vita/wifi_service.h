#ifndef VCM_WIFI_SERVICE_H
#define VCM_WIFI_SERVICE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Runs on the PAF worker thread until vcm_wifi_stop() is called. */
int vcm_wifi_serve(void);
void vcm_wifi_stop(void);
int vcm_wifi_listening(void);
int vcm_wifi_ip_address(char *address, unsigned int capacity);
/* Only the foreground app's USB-mode control may enable MTP catalogue handoff. */
void vcm_usb_mode_set(int enabled);
int vcm_usb_mode_enabled(void);
/* Set only after the local MTP bridge receives a complete media catalogue. */
int vcm_usb_catalog_served(void);
void vcm_usb_catalog_connection_lost(void);

#ifdef __cplusplus
}
#endif

#endif
