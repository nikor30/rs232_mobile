#pragma once
// ============================================================================
//  802.1X / EAP naming across the two Arduino cores this project builds on:
//
//    Arduino core 2.x (ESP-IDF 4.4)   esp_wpa2.h        esp_wifi_sta_wpa2_ent_*
//    Arduino core 3.x (ESP-IDF 5.1+)  esp_eap_client.h  esp_eap_client_*
//                                                       esp_wifi_sta_enterprise_*
//
//  IDF 5.3 still ships esp_wpa2.h, but only as a deprecated shim that warns on
//  include and is announced for removal. So the code uses the new names and this
//  header maps them back for the old core - nothing here changes behaviour, it
//  only keeps one source tree building for the mock-up (ESP32, core 2.0.17) and
//  for the 5" panel board (ESP32-S3, core 3.1.1).
// ============================================================================
#include <esp_idf_version.h>

#if ESP_IDF_VERSION_MAJOR >= 5

#include <esp_eap_client.h>

#else

#include <esp_wpa2.h>

#define esp_wifi_sta_enterprise_enable            esp_wifi_sta_wpa2_ent_enable
#define esp_wifi_sta_enterprise_disable           esp_wifi_sta_wpa2_ent_disable
#define esp_eap_client_set_identity               esp_wifi_sta_wpa2_ent_set_identity
#define esp_eap_client_clear_identity             esp_wifi_sta_wpa2_ent_clear_identity
#define esp_eap_client_set_username               esp_wifi_sta_wpa2_ent_set_username
#define esp_eap_client_clear_username             esp_wifi_sta_wpa2_ent_clear_username
#define esp_eap_client_set_password               esp_wifi_sta_wpa2_ent_set_password
#define esp_eap_client_clear_password             esp_wifi_sta_wpa2_ent_clear_password
#define esp_eap_client_set_ca_cert                esp_wifi_sta_wpa2_ent_set_ca_cert
#define esp_eap_client_clear_ca_cert              esp_wifi_sta_wpa2_ent_clear_ca_cert
#define esp_eap_client_set_certificate_and_key    esp_wifi_sta_wpa2_ent_set_cert_key
#define esp_eap_client_clear_certificate_and_key  esp_wifi_sta_wpa2_ent_clear_cert_key
#define esp_eap_client_set_disable_time_check     esp_wifi_sta_wpa2_ent_set_disable_time_check
#define esp_eap_client_set_ttls_phase2_method     esp_wifi_sta_wpa2_ent_set_ttls_phase2_method

#endif
