// Element.h
//
// Abstract base class for all physical Elements on a Peripheral board.
// An Element controls one logical bit of hardware (a motor, a servo, etc.).
//
// Lifecycle:
//   1. Create via ElementFactory given a type name.
//   2. configure(cfg)   - copy values out of the JSON into members.
//                         No hardware touched yet.
//   3. initialise()     - actual hardware setup (attach PWM, attach servo
//                         etc.). Split from configure() to help debugging.
//   4. setParameters() / setParameter() - apply new target parameter
//                         values. Base class stores them in _params[]
//                         and calls the onParametersChanged() hook.
//   5. update(dtMs)     - called every loop tick; base dispatches to
//                         updateControl() / updateDemo() depending on
//                         the demo flag.
//   6. allStop()        - put hardware in a safe state.
//   7. startDemo() / stopDemo() - toggle continuous demo pattern.
//
// Parameters are uint16 values in [0, 65535] ~ [0.0, 1.0] (see Protocol.h).

#pragma once

#include <stdint.h>
#include <ArduinoJson.h>

class Element {
public:
    static const uint8_t MAX_PARAMS = 8;

    virtual ~Element() = default;

    // --- Lifecycle ---

    virtual bool configure(JsonObjectConst cfg) = 0;
    virtual bool initialise() = 0;

    // --- Update dispatcher (not overridden) ---

    void update(uint32_t dtMs) {
        if (demoActive) updateDemo(dtMs);
        else            updateControl(dtMs);
    }

    // --- Parameter API ---

    // Set all parameters at once. Count must match parameterCount().
    // Concrete in the base class: stores values in _params[] and fires
    // the onParametersChanged() hook. Virtual so Elements can override
    // for custom validation if needed.
    virtual bool setParameters(const uint16_t* vals, uint8_t count) {
        if (count != parameterCount()) return false;
        for (uint8_t i = 0; i < count; i++) _params[i] = vals[i];
        onParametersChanged();
        return true;
    }

    // Set a single parameter by zero-based index.
    virtual bool setParameter(uint8_t index, uint16_t val) {
        if (index >= parameterCount()) return false;
        _params[index] = val;
        onParametersChanged();
        return true;
    }

    // Current parameter value (zero-based index, bounds-checked).
    uint16_t parameter(uint8_t index) const {
        return index < MAX_PARAMS ? _params[index] : 0;
    }

    // --- Safety / demo ---

    virtual void allStop() = 0;

    void startDemo() {
        if (!demoActive) { demoActive = true; onDemoStart(); }
    }

    void stopDemo() {
        if (demoActive) { demoActive = false; onDemoStop(); }
    }

    bool isDemoActive() const { return demoActive; }

    // --- Type info (must match control_schema.json) ---

    virtual uint8_t parameterCount() const = 0;
    virtual uint8_t typeId() const = 0;
    virtual const char* typeName() const = 0;

protected:
    // Element-specific behaviour. dtMs is elapsed since the last update()
    // call. Implementations drive hardware from here.
    virtual void updateControl(uint32_t dtMs) = 0;
    virtual void updateDemo(uint32_t dtMs) = 0;

    // Hooks. Default implementations are no-ops.
    virtual void onDemoStart() {}
    virtual void onDemoStop()  {}
    virtual void onParametersChanged() {}

    // Parameter storage. Elements read via parameter(i) or directly.
    uint16_t _params[MAX_PARAMS] = {0};

private:
    bool demoActive = false;
};
