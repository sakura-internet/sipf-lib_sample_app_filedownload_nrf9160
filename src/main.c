/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <nrf_modem_at.h>
#include <modem/lte_lc.h>
#include <modem/modem_info.h>
#include <modem/modem_key_mgmt.h>
#include <modem/nrf_modem_lib.h>
#include <modem/pdn.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>

#include "sipf/sipf_client_http.h"
#include "sipf/sipf_auth.h"
#include "sipf/sipf_file.h"
#include "uart_broker.h"

#include "version.h"

LOG_MODULE_REGISTER(sipf, CONFIG_SIPF_LOG_LEVEL);

/** peripheral **/
#define LED_HEARTBEAT_MS (500)
static const struct gpio_dt_spec led_boot = GPIO_DT_SPEC_GET(DT_ALIAS(led_boot), gpios);
static const struct gpio_dt_spec led_state = GPIO_DT_SPEC_GET(DT_ALIAS(led_state), gpios);
static const struct gpio_dt_spec btn_send = GPIO_DT_SPEC_GET(DT_ALIAS(btn_send), gpios);

/**********/

/** TLS **/
#define TLS_SEC_TAG 42
static const char cert[] = {
#include "sipf/cert/sipf.iot.sakura.ad.jp"
};
BUILD_ASSERT(sizeof(cert) < KB(4), "Certificate too large");
/*********/

static K_SEM_DEFINE(lte_connected, 0, 1);
static K_SEM_DEFINE(reset_request, 0, 1);
static const struct device *uart_dev;

/* Auth info */
#define SZ_USER_NAME (255)
#define SZ_PASSWORD (255)
static char user_name[SZ_USER_NAME];
static char password[SZ_PASSWORD];

/* Initialize AT communications */
int at_comms_init(void)
{
    return 0;
}

static int button_init(void)
{
    if (!device_is_ready(btn_send.port)) {
        return -ENODEV;
    }
    gpio_pin_configure_dt(&btn_send, GPIO_INPUT);

    return 0;
}

/** LED **/
static int led_init(void)
{
    if (!device_is_ready(led_boot.port)) {
        return -ENODEV;
    }
    gpio_pin_configure_dt(&led_boot, GPIO_OUTPUT_INACTIVE);

    if (!device_is_ready(led_state.port)) {
        return -ENODEV;
    }
    gpio_pin_configure_dt(&led_state, GPIO_OUTPUT_INACTIVE);

    return 0;
}
/***********/

/** MODEM **/
#define REGISTER_TIMEOUT_MS (120000)
#define REGISTER_TRY (3)

static int cert_provision(void)
{
    int err;
    bool exists;

    err = modem_key_mgmt_exists(TLS_SEC_TAG, MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN, &exists);
    if (err) {
        LOG_ERR("Failed to check for certificates err %d", err);
        return err;
    }

    if (exists) {
        /* For the sake of simplicity we delete what is provisioned
         * with our security tag and reprovision our certificate.
         */
        err = modem_key_mgmt_delete(TLS_SEC_TAG, MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN);
        if (err) {
            LOG_ERR("Failed to delete existing certificate, err %d", err);
        }
    }

    LOG_DBG("Provisioning certificate");

    /*  Provision certificate to the modem */
    err = modem_key_mgmt_write(TLS_SEC_TAG, MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN, cert, sizeof(cert) - 1);
    if (err) {
        LOG_ERR("Failed to provision certificate, err %d", err);
        return err;
    }

    return 0;
}

static void lte_handler(const struct lte_lc_evt *const evt)
{
    LOG_DBG("evt->type=%d", evt->type);
    switch (evt->type) {
    case LTE_LC_EVT_NW_REG_STATUS:
        LOG_DBG("- evt->nw_reg_status=%d\n", evt->nw_reg_status);
        if (evt->nw_reg_status == LTE_LC_NW_REG_SEARCHING) {
            UartBrokerPrint("SEARCHING\r\n");
            break;
        }
        if ((evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME) || (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING)) {
            UartBrokerPrint("REGISTERD\r\n");
            k_sem_give(&lte_connected);
            break;
        }
        break;
    case LTE_LC_EVT_CELL_UPDATE:
        LOG_DBG("- mcc=%d, mnc=%d", evt->cell.mcc, evt->cell.mnc);
        break;
    case LTE_LC_EVT_LTE_MODE_UPDATE:
        LOG_DBG("- evt->lte_mode=%d", evt->lte_mode);
        break;
    case LTE_LC_EVT_MODEM_EVENT:
        LOG_DBG("- evt->modem_evt=%d", evt->modem_evt);
        break;
    default:
        break;
    }
}

static int init_modem_and_lte(void)
{
    int err = 0;

    err = nrf_modem_lib_init(NORMAL_MODE);
    if (err) {
        LOG_ERR("Failed to initialize modem library!");
        return err;
    }

    /* Provision certificates before connecting to the LTE network */
    err = cert_provision();
    if (err) {
        LOG_ERR("Faild to cert_provision(): %d", err);
        return err;
    }

    /* PDN */
    uint8_t cid;
    err = pdn_ctx_create(&cid, NULL);
    if (err != 0) {
        LOG_ERR("Failed to pdn_ctx_create(), err %d", err);
        return err;
    }
    // set APN
    err = pdn_ctx_configure(cid, "sakura", PDN_FAM_IPV4, NULL);
    if (err != 0) {
        LOG_ERR("Failed to pdn_ctx_configure(), err %d", err);
        return err;
    }
    LOG_DBG("Setting APN OK");

    /* CONNECT */
    for (int i = 0; i < REGISTER_TRY; i++) {
        LOG_DBG("Initialize LTE");
        err = lte_lc_init();
        if (err) {
            LOG_ERR("Failed to initializes the modem, err %d", err);
            return err;
        }
        LOG_DBG("Initialize LTE OK");

        lte_lc_modem_events_enable();

        LOG_INF("[%d] Trying to attach to LTE network (TIMEOUT: %d ms)", i, REGISTER_TIMEOUT_MS);
        UartBrokerPrint("Trying to attach to LTE network (TIMEOUT: %d ms)\r\n", REGISTER_TIMEOUT_MS);
        err = lte_lc_connect_async(lte_handler);
        if (err) {
            LOG_ERR("Failed to attatch to the LTE network, err %d", err);
            return err;
        }
        err = k_sem_take(&lte_connected, K_MSEC(REGISTER_TIMEOUT_MS));
        if (err == -EAGAIN) {
            UartBrokerPrint("TIMEOUT\r\n");
            lte_lc_offline();
            lte_lc_deinit();
            continue;
        } else if (err == 0) {
            // connected

            // PSMの設定
            err = lte_lc_psm_req(true);
            if (err) {
                LOG_ERR("PSM request failed, error: %d", err);
            } else {
                LOG_DBG("PSM is enabled");
            }
            return 0;
        } else {
            //
            return err;
        }
    }

    LOG_ERR("Faild to attach to LTE Network");
    return -1;
}
/**********/

uint8_t work_buff[1024];

void main(void)
{
    int err;

    int64_t ms_now, ms_timeout;

    // UartBrokerの初期化(以降、Debug系の出力も可能)
    uart_dev = DEVICE_DT_GET(UART_LABEL);
    UartBrokerInit(uart_dev);
    UartBrokerPrint("*** SIPF SDK Sample for nRFConnect\r\n");

#ifdef CONFIG_LTE_LOCK_PLMN
    UartBrokerPuts("* PLMN: " CONFIG_LTE_LOCK_PLMN_STRING "\r\n");
#endif
#ifdef CONFIG_SIPF_AUTH_DISABLE_SSL
    UartBrokerPuts("* Disable SSL, AUTH endpoint.\r\n");
#endif
#ifdef CONFIG_SIPF_CONNECTOR_DISABLE_SSL
    UartBrokerPuts("* Disable SSL, CONNECTOR endpoint.\r\n");
#endif
    // LEDの初期化
    led_init();
    gpio_pin_set_dt(&led_boot, 1);

    // ボタンの初期化
    if (button_init() != 0) {
        gpio_pin_set_dt(&led_boot,0);
        goto err;
    }

    //モデムの初期化&LTE接続
    err = init_modem_and_lte();
    if (err) {
        goto err;
    }

    // 認証モードをSIM認証にする
    for (;;) {
        UartBrokerPuts("Set AuthMode to `SIM Auth'... \r\n");
        err = SipfAuthRequest(user_name, sizeof(user_name), password, sizeof(user_name));
        LOG_DBG("SipfAuthRequest(): %d", err);
        if (err < 0) {
            // IPアドレス認証に失敗した
            UartBrokerPuts("faild(Retry after 10s)\r\n");
            k_sleep(K_MSEC(10000));
            continue;
        }
        UartBrokerPuts("OK\r\n");
        break;
    }
    err = SipfClientHttpSetAuthInfo(user_name, password);
    if (err < 0) {
        // 認証情報の設定に失敗した
        goto err;
    }

    UartBrokerPuts("+++ Ready +++\r\n");
    gpio_pin_set_dt(&led_state, 1);
    ms_timeout = k_uptime_get() + LED_HEARTBEAT_MS;

    int btn_prev = 0;
    uint32_t cnt_push_btn = 0;
    for (;;) {
        // Heart Beat
        ms_now = k_uptime_get();
        if ((ms_timeout - ms_now) < 0) {
            ms_timeout = ms_now + LED_HEARTBEAT_MS;
            gpio_pin_toggle_dt(&led_state);
        }

        int btn_val = gpio_pin_get_dt(&btn_send);
        if (btn_val < 0) {
            LOG_ERR("button_read() failed.");
        } else {
            if ((btn_prev == 0) && (btn_val == 1)) {
                UartBrokerPuts("File put Button Pushed\r\n");
                // 送信ボタンが押された
                cnt_push_btn++;

                int payload_len = sprintf((char*)work_buff, "hello %d times.\r\n", cnt_push_btn);
                gpio_pin_set_dt(&led_state, 1);
                if (SipfFileUpload("sipf_file_sample.txt", work_buff, NULL, payload_len) == 0) {
                    UartBrokerPuts("SUCCESS!\r\n");
                } else {
                    UartBrokerPuts("FAILED\r\n");
                }
                gpio_pin_set_dt(&led_state, 0);
            }
            btn_prev = btn_val;
        }

        k_sleep(K_MSEC(10));
    }
err:
    for (;;) {
        gpio_pin_toggle_dt(&led_boot);
        k_sleep(K_MSEC(100));
    }
}
