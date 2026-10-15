#ifndef UUID_H
#define UUID_H

#define UUID_STR_SIZE   37

void uuid_init(void);
void uuid_generate(char out[UUID_STR_SIZE]);

#endif
