#include "aerospace.h"
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SWIPE_THRESHOLD 0.15f
#define SWIPE_VELOCITY_THRESHOLD 0.5f
#define SWIPE_COOLDOWN 0.3
#define NATURAL_SWIPE 0
#define SKIP_EMPTY 1
#define WRAP_AROUND 1

#if NATURAL_SWIPE
#define SWIPE_LEFT  "next"
#define SWIPE_RIGHT "prev"
#else
#define SWIPE_LEFT  "prev"
#define SWIPE_RIGHT "next"
#endif

#define ACTIVE_TOUCH_THRESHOLD    0.05f

static Aerospace *client = NULL;
static pthread_mutex_t gestureMutex = PTHREAD_MUTEX_INITIALIZER;

typedef void *MTDeviceRef;

typedef struct {
    float x, y;
} mtPoint;

typedef struct {
    mtPoint pos, vel;
} mtReadout;

typedef struct {
    int frame;
    double timestamp;
    int identifier, state, foo3, foo4;
    mtReadout normalized;
    float size;
    int zero1;
    float angle, majorAxis, minorAxis;
    mtReadout mm;
    int zero2[2];
    float unk2;
} MtTouch;

typedef void (*MTContactCallbackFunction)(int device, MtTouch *data,
                                          int nFingers, double timestamp,
                                          int frame);

static void switch_workspace(const char *ws) {
    if (WRAP_AROUND) {
        char *workspaces = aerospace_list_workspaces(client);
        if (!workspaces) {
            fprintf(stderr, "Error: Unable to retrieve workspace list.\n");
            return;
        }
        char *result = aerospace_workspace(client, WRAP_AROUND, ws, workspaces);
        if (result) {
            fprintf(stderr, "Error: Failed to switch workspace to '%s'.\n", ws);
        } else {
            printf("Switched workspace successfully to '%s'.\n", ws);
        }
        free(workspaces);
    } else {
        char *result = aerospace_switch(client, ws);
        if (result) {
            fprintf(stderr, "Error: Failed to switch workspace: '%s'\n", result);
        } else {
            printf("Switched workspace successfully to '%s'.\n", ws);
        }
    }
}


static void gestureCallback(int _device, MtTouch *contacts, int numContacts,
                            double timestamp, int _frame) {
    pthread_mutex_lock(&gestureMutex);

    static bool swiping = false;
    static float startAvgX = 0.0f;
    static double lastSwipeTime = 0.0;
    static int consecutiveRightFrames = 0;
    static int consecutiveLeftFrames = 0;

    int activeCount = 0;
    float sumX = 0.0f;
    float sumVelX = 0.0f;
    for (int i = 0; i < numContacts; ++i) {
        if (contacts[i].size > ACTIVE_TOUCH_THRESHOLD) {
            activeCount++;
            sumX += contacts[i].normalized.pos.x;
            sumVelX += contacts[i].normalized.vel.x;
        }
    }

    // Process gesture only if exactly three active touches are present
    // and if not in the cooldown period.
    if (activeCount != 3 || (timestamp - lastSwipeTime) < SWIPE_COOLDOWN) {
        swiping = false;
        consecutiveRightFrames = 0;
        consecutiveLeftFrames = 0;
        pthread_mutex_unlock(&gestureMutex);
        return;
    }

    const float avgX    = sumX / activeCount;
    const float avgVelX = sumVelX / activeCount;

    if (!swiping) {
        swiping = true;
        startAvgX = avgX;
        consecutiveRightFrames = 0;
        consecutiveLeftFrames = 0;
    } else {
        const float delta = avgX - startAvgX;
        bool triggered = false;

        if (avgVelX > SWIPE_VELOCITY_THRESHOLD) {
            consecutiveRightFrames++;
            consecutiveLeftFrames = 0;
            if (consecutiveRightFrames >= 2) {
                printf("Right swipe (by velocity) detected.\n");
                switch_workspace(SWIPE_RIGHT);
                triggered = true;
                consecutiveRightFrames = 0;
            }
        } else if (avgVelX < -SWIPE_VELOCITY_THRESHOLD) {
            consecutiveLeftFrames++;
            consecutiveRightFrames = 0;
            if (consecutiveLeftFrames >= 2) {
                printf("Left swipe (by velocity) detected.\n");
                switch_workspace(SWIPE_LEFT);
                triggered = true;
                consecutiveLeftFrames = 0;
            }
        }
        else if (delta > SWIPE_THRESHOLD) {
            printf("Right swipe (by position) detected.\n");
            switch_workspace(SWIPE_RIGHT);
            triggered = true;
        } else if (delta < -SWIPE_THRESHOLD) {
            printf("Left swipe (by position) detected.\n");
            switch_workspace(SWIPE_LEFT);
            triggered = true;
        }

        if (triggered) {
            lastSwipeTime = timestamp;
            swiping = false;
        }
    }

    pthread_mutex_unlock(&gestureMutex);
}

int main(void) {
    client = aerospace_new(NULL);
    if (!client) {
        fprintf(stderr, "Error: Failed to initialize Aerospace client.\n");
        return EXIT_FAILURE;
    }

    void *mtlib = dlopen("/System/Library/PrivateFrameworks/MultitouchSupport.framework/MultitouchSupport", RTLD_NOW);
    if (!mtlib) {
        fprintf(stderr, "Error: Unable to load MultitouchSupport framework: %s\n", dlerror());
        aerospace_close(client);
        return EXIT_FAILURE;
    }

    MTDeviceRef (*MTDeviceCreateDefault)(void) =
        (MTDeviceRef (*)(void))dlsym(mtlib, "MTDeviceCreateDefault");
    int (*MTDeviceStart)(MTDeviceRef, int) =
        (int (*)(MTDeviceRef, int))dlsym(mtlib, "MTDeviceStart");
    void (*MTDeviceStop)(MTDeviceRef) =
        (void (*)(MTDeviceRef))dlsym(mtlib, "MTDeviceStop");
    void (*MTRegisterContactFrameCallback)(MTDeviceRef, MTContactCallbackFunction, void *) =
        (void (*)(MTDeviceRef, MTContactCallbackFunction, void *))dlsym(mtlib, "MTRegisterContactFrameCallback");

    if (!MTDeviceCreateDefault || !MTDeviceStart || !MTDeviceStop || !MTRegisterContactFrameCallback) {
        fprintf(stderr, "Error: Failed to load required functions from MultitouchSupport framework.\n");
        dlclose(mtlib);
        aerospace_close(client);
        return EXIT_FAILURE;
    }

    MTDeviceRef mtDevice = MTDeviceCreateDefault();
    if (!mtDevice) {
        fprintf(stderr, "Error: Failed to create MTDevice instance.\n");
        dlclose(mtlib);
        aerospace_close(client);
        return EXIT_FAILURE;
    }

    MTRegisterContactFrameCallback(mtDevice, gestureCallback, NULL);

    if (MTDeviceStart(mtDevice, 0) != 0) {
        fprintf(stderr, "Error: Failed to start MTDevice.\n");
        dlclose(mtlib);
        aerospace_close(client);
        return EXIT_FAILURE;
    }

    printf("Listening for three-finger swipes...\n");
    CFRunLoopRun();

    MTDeviceStop(mtDevice);
    dlclose(mtlib);
    aerospace_close(client);

    return EXIT_SUCCESS;
}
