/* ctr_save.c  The game's saves (backup.bin, eeprom.bin) written by a thread below the game's priority: the arcade
   updates its book-keeping each second it is played, and a file written through the SD card takes about 70 ms */

#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "ctr_front.h"
#include "naomi_host.h"

#define SLOTS 4
#define MOST 4096

static struct {
    char name[256];
    char data[MOST];
    int size, due;
} slot[SLOTS];
static LightLock lock;
static LightEvent wake;
static Thread writer;
static volatile int quit;

static void writer_main(void* arg) {
    static char name[256], data[MOST];
    (void)arg;
    for (;;) {
        int i, size = 0;
        LightLock_Lock(&lock);
        for (i = 0; i < SLOTS && !slot[i].due; i++) {
        }
        if (i < SLOTS) {
            memcpy(name, slot[i].name, sizeof name);
            memcpy(data, slot[i].data, (size_t)slot[i].size);
            size = slot[i].size;
            slot[i].due = 0;
        }
        LightLock_Unlock(&lock);
        if (i < SLOTS) {
            host_save_file(name, data, size);
        } else if (quit) {
            break;
        } else {
            LightEvent_Wait(&wake);
        }
    }
}

/* a copy of the file's bytes given to the writer; 0 when it can't take them */
int ctr_save_later(const char* name, const void* data, int size) {
    int i, free_at = -1;
    if (quit || size <= 0 || size > MOST || strlen(name) >= sizeof slot[0].name) {
        return 0;
    }
    if (writer == NULL) {
        s32 prio = 0x30;
        LightLock_Init(&lock);
        LightEvent_Init(&wake, RESET_ONESHOT);
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        writer = threadCreate(writer_main, NULL, 8 << 10, prio + 2 <= 0x3F ? prio + 2 : 0x3F, 0, false);
        if (writer == NULL) {
            quit = 1;
            return 0;
        }
    }
    LightLock_Lock(&lock);
    for (i = 0; i < SLOTS; i++) {
        if (strcmp(slot[i].name, name) == 0) {
            break;
        }
        if (slot[i].name[0] == '\0' && free_at < 0) {
            free_at = i;
        }
    }
    if (i == SLOTS) {
        i = free_at;
    }
    if (i >= 0) {
        strcpy(slot[i].name, name);
        memcpy(slot[i].data, data, (size_t)size);
        slot[i].size = size;
        slot[i].due = 1;
    }
    LightLock_Unlock(&lock);
    if (i < 0) {
        return 0;
    }
    LightEvent_Signal(&wake);
    return 1;
}

/* every save written and the writer stopped; later ones are written as they come */
void ctr_save_flush(void) {
    quit = 1;
    if (writer != NULL) {
        LightEvent_Signal(&wake);
        if (R_SUCCEEDED(threadJoin(writer, 2000000000ull))) {
            threadFree(writer);
        }
        writer = NULL;
    }
}
