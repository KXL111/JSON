/*
 * wdg —— 看门狗（防跑飞）
 *
 * 硬件用独立看门狗 IWDG（约 4 秒溢出），只由监视任务 wdg_task 喂。
 * 需要被监视的任务先 wdg_register() 登记，然后在主循环里周期调用 wdg_feed()；
 * 任何一个登记过的任务超过 3 秒没喂，监视任务就停止喂硬件狗，系统随后复位。
 * 这样不仅能发现程序跑飞，也能发现某个任务卡死。
 */
#ifndef WDG_H
#define WDG_H

void wdg_init(void);                        /* 启动 IWDG，main 里尽早调用 */
int  wdg_register(const char *name);        /* 登记当前任务，返回编号 */
void wdg_feed(int id);                      /* 任务报平安 */
void wdg_task(void *arg);                   /* 监视任务 */

#endif /* WDG_H */
