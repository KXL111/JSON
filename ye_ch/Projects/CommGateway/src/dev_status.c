/*
 * dev_status.c —— 设备状态总表
 *
 * 写：bd_task.c 的 update_signal()（SQ / status）、on_icp()（卡信息、CD、maxlength）、
 *     set_task_state()（taskstate）；天通接入后由 tt_task.c 写 tt 部分
 * 读：term_task.c 的 reply_heartbeat() 用 dev_status_read() 取快照，组 42202 心跳回复
 */
#include <string.h>
#include "FreeRTOS.h"
#include "semphr.h"
#include "dev_status.h"

static dev_status_t      s_status;      /* 状态总表本体，只能经过下面的函数访问 */
static SemaphoreHandle_t s_lock;        /* 保护 s_status 的互斥锁 */

/*
 * 初始化状态表：全部清零，北斗、天通都设为离线（status = 1），创建互斥锁
 *
 * 清零后：SQ / CD / maxlength 为 0，msg 为空，taskstate 为空（心跳里是空对象），
 * 等各自的设备任务刷新。
 * 调用：main()，调度器启动前
 */
void dev_status_init(void)
{
    memset(&s_status, 0, sizeof(s_status));
    s_status.bd.status = 1;
    s_status.tt.status = 1;
    s_lock = xSemaphoreCreateMutex();
    configASSERT(s_lock != NULL);
}

/*
 * 加锁并返回状态表指针，调用方直接读写字段，用完必须调用 dev_status_unlock()
 *
 * FreeRTOS 互斥锁带优先级继承，一直等到拿到为止。持锁期间只做赋值，
 * 不要调用会阻塞的函数（收发网络、打日志），保证其他任务等锁的时间很短。
 *
 * 返回：状态表 s_status 的指针
 * 调用：bd_task.c 的 set_task_state()、on_icp()、update_signal()；本文件的 dev_status_read()
 */
dev_status_t *dev_status_lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    return &s_status;
}

/*
 * 释放状态表的锁，与 dev_status_lock() 成对使用
 */
void dev_status_unlock(void)
{
    xSemaphoreGive(s_lock);
}

/*
 * 拷贝一份完整的状态快照：加锁、整体结构体赋值（约 200 字节）、立即解锁
 *
 * 读的一方拿着快照慢慢组包，期间不占锁，也不会读到写了一半的数据。
 *
 * 参数：out —— 输出的快照
 * 调用：term_task.c 的 reply_heartbeat()
 */
void dev_status_read(dev_status_t *out)
{
    *out = *dev_status_lock();
    dev_status_unlock();
}
