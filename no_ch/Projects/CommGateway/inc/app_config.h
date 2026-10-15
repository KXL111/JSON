#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#define CFG_LOCAL_IP            "10.9.0.57"
#define CFG_LOCAL_NETMASK       "255.255.255.0"
#define CFG_LOCAL_GATEWAY       "10.9.0.1"
#define CFG_LOCAL_MAC           { 0x02, 0x00, 0x0A, 0x09, 0x00, 0x39 }

#define CFG_TERM_LOCAL_PORT     4220
#define CFG_TERM_IP             "10.9.0.24"
#define CFG_TERM_PORT           5000
#define CFG_TERM_TS_MS          1

#define CFG_BD_BAUDRATE         38400U

#define CFG_TT_BAUDRATE         115200U

#define CFG_BD_TCQ_FREQ         2
#define CFG_BD_TCQ_CONFIRM      2
#define CFG_BD_FKI_TIMEOUT_MS   10000
#define CFG_BD_ICR_RETRY_MS     5000
#define CFG_BD_SIGNAL_STALE_MS  5000
#define CFG_BD_LINK_TIMEOUT_MS  5000

#define CFG_LOG_ENABLE          1
#define CFG_LOG_BAUDRATE        115200U

#define CFG_LOG_TERM_RAW        1
#define CFG_LOG_TERM_HEARTBEAT  1
#define CFG_LOG_BD_RAW          1
#define CFG_LOG_BD_PERIODIC     0
#define CFG_LOG_RAW_MAX         2048

#endif
