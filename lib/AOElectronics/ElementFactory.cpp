// ElementFactory.cpp

#include "ElementFactory.h"
#include "PWM_MOTOR.h"
#include "SERVO_MOTOR.h"
#include "SERVO_ARM.h"
#include "BLDC_MOTOR.h"
#include <Arduino.h>
#include <string.h>

Element* ElementFactory::createElement(const char* typeName) {
    if (typeName == nullptr) return nullptr;

    if (strcmp(typeName, "PWM_MOTOR")   == 0) return new PWM_MOTOR();
    if (strcmp(typeName, "SERVO_MOTOR") == 0) return new SERVO_MOTOR();
    if (strcmp(typeName, "SERVO_ARM")   == 0) return new SERVO_ARM();
    if (strcmp(typeName, "BLDC_MOTOR")  == 0) return new BLDC_MOTOR();
    // TODO: CONTINUOUS_SERVO — add here once implemented.
    // See control_schema.json for the authoritative list.

    Serial.printf("# [ElementFactory] unknown element type: %s\n", typeName);
    return nullptr;
}
