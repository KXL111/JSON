#ifndef WDG_H
#define WDG_H

void wdg_init(void);
int  wdg_register(const char *name);
void wdg_feed(int id);
void wdg_task(void *arg);

#endif
