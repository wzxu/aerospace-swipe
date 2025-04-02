#include "Carbon/Carbon.h"
#include "Cocoa/Cocoa.h"
#include "aerospace.h"
#include "config.h"
#include "event_tap.h"
#include "haptic.h"
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

static void gestureCallback(touch* contacts, int numContacts,
	double timestamp)
{
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
		// TODO: check size vs ACTIVE_TOUCH_THRESHOLD
		activeCount++;
		sumX += contacts[i].x;
		sumVelX += contacts[i].velocity;
	}

	if (activeCount != config.fingers || (timestamp - lastSwipeTime) < SWIPE_COOLDOWN) {
		swiping = false;
		consecutiveRightFrames = 0;
		consecutiveLeftFrames = 0;
		pthread_mutex_unlock(&gestureMutex);
		return;
	}

	const float avgX = sumX / activeCount;
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
			lastSwipeTime = timestamp;
			swiping = false;
		}
	}

	pthread_mutex_unlock(&gestureMutex);
}

static CGEventRef key_handler(CGEventTapProxy proxy, CGEventType type,
	CGEventRef event, void* reference)
{
	switch (type) {
	case kCGEventTapDisabledByTimeout:
		NSLog(@"Timeout\n");
	case kCGEventTapDisabledByUserInput: {
		NSLog(@"restarting event-tap\n");
		CGEventTapEnable(((struct event_tap*)reference)->handle, true);
	} break;
	case 29: {
		Class NSEventClass = objc_getClass("NSEvent");
		SEL eventWithCGEventSel = sel_getUid("eventWithCGEvent:");
		id (*eventWithCGEventFunc)(id, SEL, CGEventRef) = (void*)objc_msgSend;
		id nsEvent = eventWithCGEventFunc(NSEventClass, eventWithCGEventSel, event);

		SEL allTouchesSel = sel_getUid("allTouches");
		id (*allTouchesFunc)(id, SEL) = (void*)objc_msgSend;
		id touches = allTouchesFunc(nsEvent, allTouchesSel);

		CFSetRef touchSet = (CFSetRef)touches;
		CFIndex count = CFSetGetCount(touchSet);

		id* nsTouchArray = malloc(sizeof(id) * count);
		if (!nsTouchArray)
			return event;
		CFSetGetValues(touchSet, (const void**)nsTouchArray);

		touch* nativeTouches = malloc(sizeof(touch) * count);
		if (!nativeTouches) {
			free(nsTouchArray);
			return event;
		}

		for (int i = 0; i < count; ++i)
			nativeTouches[i] = convert_nstouch(nsTouchArray[i]);

		gestureCallback(nativeTouches, count, nativeTouches[0].timestamp);

		free(nativeTouches);
		free(nsTouchArray);
		break;
	}
	default:
		break;
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

		// Continue with normal application startup.
		return NSApplicationMain(argc, argv);
	}
}
