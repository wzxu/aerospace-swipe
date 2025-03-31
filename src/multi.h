#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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
  int identifier, state, foo3, foo4; // state 4 = touching
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

extern MTDeviceRef MTDeviceCreateDefault(void);
extern int MTDeviceStart(MTDeviceRef device, int unknown);
extern void MTDeviceStop(MTDeviceRef device);
extern void MTRegisterContactFrameCallback(MTDeviceRef device,
                                           MTContactCallbackFunction callback,
                                           void *context);

#ifdef __cplusplus
}
#endif
