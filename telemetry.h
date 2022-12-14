//Always use include guards
#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <Arduino.h>

//All function prototypes from telemetry.cpp.
void upload_telemetry(float temperature);

//Should be called frequently to keep the connection active
void telemetry_poll();

#endif /* TELEMETRY_H */