#include "aerospace.h"
#include "config.h"
#include "haptic.h"
#include "multi.h"
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ACTIVE_TOUCH_THRESHOLD    0.05f
#define SWIPE_THRESHOLD		   0.15f
#define SWIPE_VELOCITY_THRESHOLD  0.5f
#define SWIPE_COOLDOWN            0.3f

static Aerospace *client = NULL;
static CFTypeRef haptic = NULL;
static Config config;
static pthread_mutex_t gestureMutex = PTHREAD_MUTEX_INITIALIZER;

static void switch_workspace(const char *ws) {
    if (config.wrap_around) {
        char *workspaces = aerospace_list_workspaces(client);
        if (!workspaces) {
            fprintf(stderr, "Error: Unable to retrieve workspace list.\n");
            return;
        }
        char *result = aerospace_workspace(client, config.wrap_around, ws, workspaces);
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

    if (config.haptic == true) haptic_actuate(haptic, 3);
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
        if (contacts[i].size > ACTIVE_TOUCH_THRESHOLD && contacts[i].state == 4) {
            activeCount++;
            sumX += contacts[i].normalized.pos.x;
            sumVelX += contacts[i].normalized.vel.x;
        }
    }

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
                switch_workspace(config.swipe_right);
                triggered = true;
                consecutiveRightFrames = 0;
            }
        } else if (avgVelX < -SWIPE_VELOCITY_THRESHOLD) {
            consecutiveLeftFrames++;
            consecutiveRightFrames = 0;
            if (consecutiveLeftFrames >= 2) {
                printf("Left swipe (by velocity) detected.\n");
                switch_workspace(config.swipe_left);
                triggered = true;
                consecutiveLeftFrames = 0;
            }
        }
        else if (delta > SWIPE_THRESHOLD) {
            printf("Right swipe (by position) detected.\n");
            switch_workspace(config.swipe_right);
            triggered = true;
        } else if (delta < -SWIPE_THRESHOLD) {
            printf("Left swipe (by position) detected.\n");
            switch_workspace(config.swipe_left);
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
	config = load_config();

    client = aerospace_new(NULL);
    if (!client) {
        fprintf(stderr, "Error: Failed to initialize Aerospace client.\n");
        return EXIT_FAILURE;
    }

    haptic = haptic_open_default();
    if (!haptic) {
		fprintf(stderr, "Error: Failed to initialize haptic actuator.\n");
		aerospace_close(client);
		return EXIT_FAILURE;
	}

    MTDeviceRef mtDevice = MTDeviceCreateDefault();
    if (!mtDevice) {
        fprintf(stderr, "Error: Failed to create MTDevice instance.\n");
        aerospace_close(client);
        haptic_close(haptic);
        return EXIT_FAILURE;
    }

    MTRegisterContactFrameCallback(mtDevice, gestureCallback, NULL);

    if (MTDeviceStart(mtDevice, 0) != 0) {
        fprintf(stderr, "Error: Failed to start MTDevice.\n");
        aerospace_close(client);
        haptic_close(haptic);
        return EXIT_FAILURE;
    }

    printf("Listening for three-finger swipes...\n");
    CFRunLoopRun();

    MTDeviceStop(mtDevice);
    aerospace_close(client);
    haptic_close(haptic);

    return EXIT_SUCCESS;
}
