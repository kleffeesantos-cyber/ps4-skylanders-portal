/* Minimal stand-in for <orbis/libkernel.h> so the plugin can be built and run on a Linux PC (see test/host_sim.c). */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
typedef int32_t OrbisKernelModule;
typedef struct { void *address; uint64_t size; int32_t prot; } OrbisSeg;
typedef struct { size_t size; char name[256]; uint32_t segmentCount; OrbisSeg segmentInfo[4]; } OrbisKernelModuleInfo;
typedef pthread_t OrbisPthread;
typedef pthread_attr_t OrbisPthreadAttr;
int sceKernelUsleep(unsigned us);
int sceKernelOpen(const char *path, int flags, int mode);
long sceKernelRead(int fd, void *buf, size_t n);
long sceKernelWrite(int fd, const void *buf, size_t n);
int sceKernelClose(int fd);
int sceKernelGetdents(int fd, char *buf, int n);
int sceKernelMkdir(const char *path, int mode);
int sceKernelUnlink(const char *path);
int sceKernelVirtualQuery(const void *a, int f, void *info, size_t n);
int sceKernelMprotect(const void *a, size_t n, int prot);
int sceKernelGetModuleList(OrbisKernelModule *h, size_t max, size_t *n);
int sceKernelGetModuleInfo(OrbisKernelModule h, OrbisKernelModuleInfo *info);
int sceKernelDlsym(int32_t h, const char *name, void **addr);
int scePthreadAttrInit(OrbisPthreadAttr *a);
int scePthreadAttrSetstacksize(OrbisPthreadAttr *a, size_t n);
int scePthreadAttrSetdetachstate(OrbisPthreadAttr *a, int s);
int scePthreadAttrDestroy(OrbisPthreadAttr *a);
int scePthreadCreate(OrbisPthread *t, const OrbisPthreadAttr *a, void *(*fn)(void *), void *arg, const char *name);
