#include "Carbon/Carbon.h"
#include "Cocoa/Cocoa.h"
#include "aerospace.h"
#include "config.h"
#import "event_tap.h"
#include "haptic.h"
#include <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#include <pthread.h>

#define ACTIVE_TOUCH_THRESHOLD 0.05f
#define SWIPE_THRESHOLD 0.15f
#define SWIPE_VELOCITY_THRESHOLD 0.75f
#define SWIPE_COOLDOWN 0.3f

static Aerospace* client = NULL;
static CFTypeRef haptic = NULL;
static Config config;
static pthread_mutex_t gestureMutex = PTHREAD_MUTEX_INITIALIZER;

static void switch_workspace(const char* ws)
{
	if (config.skip_empty || config.wrap_around) {
		char* workspaces = aerospace_list_workspaces(client, config.skip_empty);
		if (!workspaces) {
			fprintf(stderr, "Error: Unable to retrieve workspace list.\n");
			return;
		}
		char* result = aerospace_workspace(client, config.wrap_around, ws, workspaces);
		if (result) {
			fprintf(stderr, "Error: Failed to switch workspace to '%s'.\n", ws);
		} else {
			printf("Switched workspace successfully to '%s'.\n", ws);
		}
		free(workspaces);
	} else {
		char* result = aerospace_switch(client, ws);
		if (result) {
			fprintf(stderr, "Error: Failed to switch workspace: '%s'\n", result);
		} else {
			printf("Switched workspace successfully to '%s'.\n", ws);
		}
	}

	if (config.haptic == true)
		haptic_actuate(haptic, 3);
}

static void gestureCallback(touch* contacts, int numContacts)
{
	pthread_mutex_lock(&gestureMutex);
	static bool swiping = false;
	static float startAvgX = 0.0f;
	static double lastSwipeTime = 0.0;
	static int consecutiveRightFrames = 0;
	static int consecutiveLeftFrames = 0;

	if (numContacts != config.fingers || (contacts[0].timestamp - lastSwipeTime) < SWIPE_COOLDOWN) {
		swiping = false;
		consecutiveRightFrames = 0;
		consecutiveLeftFrames = 0;
		pthread_mutex_unlock(&gestureMutex);
		return;
	}

	float sumX = 0.0f;
	float sumVelX = 0.0f;

	for (int i = 0; i < numContacts; ++i) {
		sumX += contacts[i].x;
		sumVelX += contacts[i].velocity;
	}

	const float avgX = sumX / numContacts;
	const float avgVelX = sumVelX / numContacts;

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
				NSLog(@"Right swipe (by velocity) detected.\n");
				switch_workspace(config.swipe_right);
				triggered = true;
				consecutiveRightFrames = 0;
			}
		} else if (avgVelX < -SWIPE_VELOCITY_THRESHOLD) {
			consecutiveLeftFrames++;
			consecutiveRightFrames = 0;
			if (consecutiveLeftFrames >= 2) {
				NSLog(@"Left swipe (by velocity) detected.\n");
				switch_workspace(config.swipe_left);
				triggered = true;
				consecutiveLeftFrames = 0;
			}
		} else if (delta > SWIPE_THRESHOLD) {
			NSLog(@"Right swipe (by position) detected.\n");
			switch_workspace(config.swipe_right);
			triggered = true;
		} else if (delta < -SWIPE_THRESHOLD) {
			NSLog(@"Left swipe (by position) detected.\n");
			switch_workspace(config.swipe_left);
			triggered = true;
		}

		if (triggered) {
			lastSwipeTime = contacts[0].timestamp;
			swiping = false;
		}
	}

	pthread_mutex_unlock(&gestureMutex);
}

static CGEventRef key_handler(CGEventTapProxy proxy,
	CGEventType type,
	CGEventRef event,
	void* reference)
{
	if (!AXIsProcessTrusted()) {
		NSLog(@"Accessibility permission lost. Disabling event tap to allow system events.");
		event_tap_end((struct event_tap*)reference);
		return event;
	}

	switch (type) {
	case kCGEventTapDisabledByTimeout:
		NSLog(@"Timeout.\n");
	case kCGEventTapDisabledByUserInput:
		NSLog(@"Re‐enabling event tap.\n");
		CGEventTapEnable(((struct event_tap*)reference)->handle, true);
		break;
	case NSEventTypeGesture: {
		NSEvent* nsEvent = [NSEvent eventWithCGEvent:event];
		NSSet<NSTouch*>* touches = nsEvent.allTouches;
		NSUInteger count = touches.count;

		if (count == 0)
			return event;

		touch* nativeTouches = malloc(sizeof(touch) * count);
		if (nativeTouches == NULL)
			return event;

		NSUInteger i = 0;
		for (NSTouch* aTouch in touches)
			nativeTouches[i++] = [TouchConverter convert_nstouch:aTouch];

		dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
			gestureCallback(nativeTouches, count);
			free(nativeTouches);
		});

		return event;
	}
	}

	return event;
}

static void acquire_lockfile(void)
{
	char* user = getenv("USER");
	if (!user)
		printf("Error: User variable not set.\n"), exit(1);

	char buffer[256];
	snprintf(buffer, 256, "/tmp/aerospace-swipe-%s.lock", user);

	int handle = open(buffer, O_CREAT | O_WRONLY, 0600);
	if (handle == -1) {
		printf("Error: Could not create lock-file.\n");
		exit(1);
	}

	struct flock lockfd = {
		.l_start = 0,
		.l_len = 0,
		.l_pid = getpid(),
		.l_type = F_WRLCK,
		.l_whence = SEEK_SET
	};

	if (fcntl(handle, F_SETLK, &lockfd) == -1) {
		printf("Error: Could not acquire lock-file.\naerospace-swipe already running?\n");
		exit(1);
	}
}

void waitForAccessibilityAndRestart(void)
{
	while (!AXIsProcessTrusted()) {
		NSLog(@"Waiting for accessibility permission...");
		sleep(1);
	}

	NSLog(@"Accessibility permission granted. Restarting app...");

	NSString* bundlePath = [[NSBundle mainBundle] bundlePath];
	[[NSWorkspace sharedWorkspace] openApplicationAtURL:[NSURL fileURLWithPath:bundlePath] configuration:[NSWorkspaceOpenConfiguration configuration] completionHandler:nil];
	exit(0);
}

int main(int argc, const char* argv[])
{
	signal(SIGCHLD, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);

	acquire_lockfile();

	@autoreleasepool {
		NSDictionary* options = @{(__bridge id)kAXTrustedCheckOptionPrompt : @YES};

		if (!AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)options)) {
			NSLog(@"Accessibility permission not granted. Prompting user...");
			AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)options);

			dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
				waitForAccessibilityAndRestart();
			});

			CFRunLoopRun();
		}

		NSLog(@"Accessibility permission granted. Continuing app initialization...");

		config = load_config();
		client = aerospace_new(NULL);
		if (!client) {
			fprintf(stderr, "Error: Failed to initialize Aerospace client.\n");
			exit(EXIT_FAILURE);
		}
		haptic = haptic_open_default();
		if (!haptic) {
			fprintf(stderr, "Error: Failed to initialize haptic actuator.\n");
			aerospace_close(client);
			exit(EXIT_FAILURE);
		}

		event_tap_begin(&g_event_tap, key_handler);

		return NSApplicationMain(argc, argv);
	}
}
