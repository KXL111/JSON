#ifndef APP_MSG_H
#define APP_MSG_H

#define APP_ID_LEN      64
#define APP_PEER_LEN    24
#define APP_TEXT_LEN    3072

typedef enum {
    DEV_BD = 0,
    DEV_TT,
    DEV_COUNT
} dev_id_t;

typedef enum {
    TEXT_SEND = 0,
    TEXT_RESULT,
    TEXT_RECV
} text_kind_t;

typedef struct {
    text_kind_t kind;
    dev_id_t    dev;
    char        msg_id[APP_ID_LEN];
    char        taskid[APP_ID_LEN];
    char        peer[APP_PEER_LEN];
    int         status;
    char        text[APP_TEXT_LEN];
} text_msg_t;

#endif
