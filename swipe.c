#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <dlfcn.h>
#include <pthread.h>
#include <CoreFoundation/CoreFoundation.h>

#define WORKSPACE_NAME_SIZE 16
#define MAX_WORKSPACES 32
#define SWIPE_THRESHOLD 0.08f
#define SWIPE_VELOCITY_THRESHOLD 0.2f
#define SWIPE_COOLDOWN 0.35

static pthread_mutex_t gestureMutex = PTHREAD_MUTEX_INITIALIZER;

static char aerospace_path[256] = {0};

static const char* try_fallback_paths(void) {
    const char* fallback_paths[] = {
        "/opt/homebrew/bin/aerospace",
        "/usr/local/bin/aerospace",
        "/usr/bin/aerospace"
    };
    for (size_t i = 0; i < sizeof(fallback_paths)/sizeof(fallback_paths[0]); i++) {
        if (access(fallback_paths[i], X_OK) == 0) {
            strncpy(aerospace_path, fallback_paths[i], sizeof(aerospace_path) - 1);
            aerospace_path[sizeof(aerospace_path) - 1] = '\0';
            return aerospace_path;
        }
    }
    return NULL;
}

static const char* get_aerospace_path(void) {
    if (aerospace_path[0] != '\0') {
        return aerospace_path;
    }

    FILE *fp = popen("which aerospace", "r");
    if (fp) {
        if (fgets(aerospace_path, sizeof(aerospace_path), fp)) {
            aerospace_path[strcspn(aerospace_path, "\n")] = '\0';
        }
        pclose(fp);
    }

    if (aerospace_path[0] == '\0') {
        if (!try_fallback_paths()) {
            fprintf(stderr, "Error: Could not find the 'aerospace' binary in PATH or fallback locations.\n");
            return NULL;
        }
    }
    return aerospace_path;
}

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

typedef void (*MTContactCallbackFunction)(int device, MtTouch *data, int nFingers, double timestamp, int frame);

static int get_allowed_workspaces(char allowed[][WORKSPACE_NAME_SIZE]) {
    const char *aero = get_aerospace_path();
    if (!aero || aero[0] == '\0') {
        return 0;
    }

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s list-workspaces --monitor mouse --empty no", aero);
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        fprintf(stderr, "Error: Unable to execute '%s'\n", cmd);
        return 0;
    }
    int count = 0;
    while (count < MAX_WORKSPACES && fgets(allowed[count], WORKSPACE_NAME_SIZE, fp)) {
        allowed[count][strcspn(allowed[count], "\n")] = '\0';
        count++;
    }
    pclose(fp);
    return count;
}

static bool get_current_workspace(char *current, size_t size) {
    const char *aero = get_aerospace_path();
    if (!aero || aero[0] == '\0') {
        return false;
    }

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s list-workspaces --focused", aero);
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        fprintf(stderr, "Error: Unable to execute '%s'\n", cmd);
        return false;
    }
    if (!fgets(current, size, fp)) {
        pclose(fp);
        return false;
    }
    current[strcspn(current, "\n")] = '\0';
    pclose(fp);
    return true;
}

static void switch_workspace(const char *ws) {
    const char *aero = get_aerospace_path();
    if (!aero || aero[0] == '\0') {
        return;
    }

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s workspace %s", aero, ws);
    printf("Executing: %s\n", cmd);
    int ret = system(cmd);
    if (ret != 0) {
        fprintf(stderr, "Error: Command failed: %s\n", cmd);
    }
}

static void switch_workspace_if_allowed(int direction) {
    char current[WORKSPACE_NAME_SIZE];
    if (!get_current_workspace(current, sizeof(current))) {
        fprintf(stderr, "Error: Failed to retrieve current workspace.\n");
        return;
    }
    char allowed[MAX_WORKSPACES][WORKSPACE_NAME_SIZE];
    int allowed_count = get_allowed_workspaces(allowed);
    if (allowed_count == 0) {
        fprintf(stderr, "Warning: No allowed workspaces found.\n");
        return;
    }

    int current_index = -1;
    for (int i = 0; i < allowed_count; i++) {
        if (strcmp(allowed[i], current) == 0) {
            current_index = i;
            break;
        }
    }
    if (current_index < 0) {
        fprintf(stderr, "Warning: Current workspace '%s' not found in allowed list; defaulting to index 0.\n", current);
        current_index = 0;
    }

    int new_index = current_index + direction;
    if (new_index < 0 || new_index >= allowed_count) {
        printf("No workspace available in that direction.\n");
        return;
    }

    printf("Switching workspace from '%s' to '%s'\n", current, allowed[new_index]);
    switch_workspace(allowed[new_index]);
}

static void gestureCallback(int device, MtTouch *contacts, int numContacts, double timestamp, int frame) {
    pthread_mutex_lock(&gestureMutex);

    static bool swiping = false;
    static float startAvgX = 0.0f;
    static double lastSwipeTime = 0.0;

    if (numContacts != 3) {
        swiping = false;
        pthread_mutex_unlock(&gestureMutex);
        return;
    }

    if (timestamp - lastSwipeTime < SWIPE_COOLDOWN) {
        pthread_mutex_unlock(&gestureMutex);
        return;
    }

    float avgX = 0.0f;
    float avgVelX = 0.0f;
    for (int i = 0; i < numContacts; i++) {
        avgX += contacts[i].normalized.pos.x;
        avgVelX += contacts[i].normalized.vel.x;
    }
    avgX /= numContacts;
    avgVelX /= numContacts;

    if (!swiping) {
        swiping = true;
        startAvgX = avgX;
    } else {
        float delta = avgX - startAvgX;
        bool triggered = false;
        if (avgVelX > SWIPE_VELOCITY_THRESHOLD) {
            printf("Right swipe (by velocity) detected.\n");
            switch_workspace_if_allowed(1);
            triggered = true;
        } else if (avgVelX < -SWIPE_VELOCITY_THRESHOLD) {
            printf("Left swipe (by velocity) detected.\n");
            switch_workspace_if_allowed(-1);
            triggered = true;
        } else if (delta > SWIPE_THRESHOLD) {
            printf("Right swipe (by position) detected.\n");
            switch_workspace_if_allowed(1);
            triggered = true;
        } else if (delta < -SWIPE_THRESHOLD) {
            printf("Left swipe (by position) detected.\n");
            switch_workspace_if_allowed(-1);
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
    void *mtlib = dlopen("/System/Library/PrivateFrameworks/MultitouchSupport.framework/MultitouchSupport", RTLD_NOW);
    if (!mtlib) {
        fprintf(stderr, "Error: Unable to load MultitouchSupport framework.\n");
        return EXIT_FAILURE;
    }

    MTDeviceRef (*MTDeviceCreateDefault)(void) = dlsym(mtlib, "MTDeviceCreateDefault");
    int (*MTDeviceStart)(MTDeviceRef, int) = dlsym(mtlib, "MTDeviceStart");
    void (*MTDeviceStop)(MTDeviceRef) = dlsym(mtlib, "MTDeviceStop");
    void (*MTRegisterContactFrameCallback)(MTDeviceRef, MTContactCallbackFunction, void*) =
        dlsym(mtlib, "MTRegisterContactFrameCallback");

    if (!MTDeviceCreateDefault || !MTDeviceStart || !MTDeviceStop || !MTRegisterContactFrameCallback) {
        fprintf(stderr, "Error: Failed to load required functions from MultitouchSupport.\n");
        dlclose(mtlib);
        return EXIT_FAILURE;
    }

    MTDeviceRef device = MTDeviceCreateDefault();
    if (!device) {
        fprintf(stderr, "Error: Failed to create MTDevice instance.\n");
        dlclose(mtlib);
        return EXIT_FAILURE;
    }

    MTRegisterContactFrameCallback(device, gestureCallback, NULL);

    if (MTDeviceStart(device, 0) != 0) {
        fprintf(stderr, "Error: Failed to start MTDevice.\n");
        dlclose(mtlib);
        return EXIT_FAILURE;
    }

    printf("Listening for three-finger swipes...\n");

    CFRunLoopRun();

    MTDeviceStop(device);
    dlclose(mtlib);
    return EXIT_SUCCESS;
}
