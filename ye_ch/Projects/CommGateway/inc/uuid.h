/*
 * uuid —— 生成标准 UUID（RFC 4122 第 4 版，随机型），用作发给终端的 msg_id / taskid
 *
 * 格式：8-4-4-4-12 共 36 个小写十六进制字符，如 "3f2a9c1e-7b4d-4a10-9c3e-5d1f00a2b7c8"
 *   第 3 段第 1 位固定为 '4'（版本号），第 4 段第 1 位是 8/9/a/b（变体位 10xx），其余 122 位随机。
 * 随机数来自芯片自带的真随机数发生器 RNG，不同设备、每次开机生成的 ID 都不会重复；
 * RNG 起不来时自动改用伪随机数，保证总能生成。
 *
 * 用法：
 *     char id[UUID_STR_SIZE];
 *     uuid_generate(id);
 */
#ifndef UUID_H
#define UUID_H

#define UUID_STR_SIZE   37      /* 36 个字符 + 结束符 */

void uuid_init(void);                       /* 打开 RNG，main() 里调度器启动前调用一次 */
void uuid_generate(char out[UUID_STR_SIZE]);

#endif /* UUID_H */
