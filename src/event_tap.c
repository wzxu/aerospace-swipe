#include "event_tap.h"
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>

const int NSEventMaskGesture = 29;

touch convert_nstouch(id nsTouch)
{
	touch nt;

	CGPoint pos = ((CGPoint(*)(id, SEL))objc_msgSend)(nsTouch, sel_getUid("normalizedPosition"));
	nt.x = pos.x;
	nt.y = pos.y;

	nt.phase = ((int (*)(id, SEL))objc_msgSend)(nsTouch, sel_getUid("phase"));
	nt.timestamp = ((double (*)(id, SEL))objc_msgSend)(nsTouch, sel_getUid("timestamp"));

	id touchIdentity = ((id(*)(id, SEL))objc_msgSend)(nsTouch, sel_getUid("identity"));

	if (!touchStates)
		touchStates = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, NULL);

	double velocity_x = 0.0;
	touch_state* prevState = (touch_state*)CFDictionaryGetValue(touchStates, touchIdentity);
	if (prevState) {
		double dt = nt.timestamp - prevState->timestamp;
		if (dt > 0)
			velocity_x = (nt.x - prevState->x) / dt;
		free(prevState);
	}
	nt.velocity = velocity_x;

	touch_state* newState = malloc(sizeof(touch_state));
	if (newState) {
		newState->x = nt.x;
		newState->y = nt.y;
		newState->timestamp = nt.timestamp;
		CFDictionarySetValue(touchStates, touchIdentity, newState);
	}

	if (nt.phase == 8)
		CFDictionaryRemoveValue(touchStates, nsTouch);

	return nt;
}

bool event_tap_enabled(struct event_tap* event_tap)
{
	bool result = (event_tap->handle && CGEventTapIsEnabled(event_tap->handle));
	return result;
}

bool event_tap_begin(struct event_tap* event_tap, CGEventRef (*reference)(CGEventTapProxy proxy, CGEventType type, CGEventRef event, void* userdata))
{
	event_tap->mask = 1 << NSEventMaskGesture;
	event_tap->handle = CGEventTapCreate(kCGHIDEventTap,
		kCGHeadInsertEventTap,
		kCGEventTapOptionDefault,
		event_tap->mask,
		*reference,
		event_tap);

	bool result = event_tap_enabled(event_tap);
	if (result) {
		event_tap->runloop_source = CFMachPortCreateRunLoopSource(
			kCFAllocatorDefault,
			event_tap->handle,
			0);
		CFRunLoopAddSource(CFRunLoopGetMain(),
			event_tap->runloop_source,
			kCFRunLoopDefaultMode);
	}

	return result;
}

void event_tap_end(struct event_tap* event_tap)
{
	if (event_tap_enabled(event_tap)) {
		CGEventTapEnable(event_tap->handle, false);
		CFMachPortInvalidate(event_tap->handle);
		CFRunLoopRemoveSource(CFRunLoopGetMain(),
			event_tap->runloop_source,
			kCFRunLoopCommonModes);
		CFRelease(event_tap->runloop_source);
		CFRelease(event_tap->handle);
		event_tap->handle = NULL;
	}
}
