// SERVO_ARM.h
//
// Two-servo arm aiming a narrow-beam LED (or laser) at a wall.
//
//   base servo ── arm (length d2) ── upper servo ── beam
//
// The base servo rotates the arm in a plane parallel to the wall. The
// upper servo sits on the arm tip and aims the beam. The kinematics
// (worked out and verified in gui/servo_arm_sim.html) keep the dot on a
// user-chosen angle on the wall while the arm itself can be at any of
// its swing positions.
//
// Parameters (both in the uniform [0, 65535] ↔ [0.0, 1.0] range):
//   ANGLE  (p1) — where on the wall the dot lands; maps to a beam
//                 angle φ in [MIN_BEAM_ANGLE_DEG, MAX_BEAM_ANGLE_DEG].
//                 φ=0 means the dot is straight out from the base pivot.
//   OFFSET (p2) — the base servo angle itself; maps to degrees in
//                 [MIN_BASE_POSITION, MAX_BASE_POSITION]. The upper
//                 servo is computed to keep the dot on the ANGLE target.
//
// Geometry (matches the simulator):
//   a1    = MIN_BASE + p2·(MAX_BASE − MIN_BASE)         // base angle
//   α     = −a1 · π/180                                  // world math angle
//   U     = (d2·cos(a1), −d2·sin(a1))                    // upper servo pos
//   φ     = MIN_BEAM + p1·(MAX_BEAM − MIN_BEAM)          // target angle
//   T     = (d1·tan(φ), d1)                              // target on wall
//   β_req = atan2(T.y − U.y, T.x − U.x)                  // beam direction
//   a2    = β_req + a1 − 90°   (clamped to [MIN_UPPER, MAX_UPPER])
//
// Demo mode: two independent triangle waves on p1 and p2 with different
// periods (6 s and 10 s) so the pattern doesn't loop obviously.

#pragma once

#include "Element.h"
#include "Protocol.h"

class SERVO_ARM : public Element {
public:
    enum Param : uint8_t { ANGLE = 0, OFFSET = 1 };

    SERVO_ARM();
    virtual ~SERVO_ARM() override;

    bool configure(JsonObjectConst cfg) override;
    bool initialise() override;
    void allStop() override;

    uint8_t parameterCount() const override { return 2; }
    uint8_t typeId() const override { return AOProtocol::TYPE_SERVO_ARM; }
    const char* typeName() const override { return "SERVO_ARM"; }

protected:
    void updateControl(uint32_t dtMs) override;
    void updateDemo(uint32_t dtMs) override;
    void onDemoStart() override;

private:
    // Config values (from config.json).
    int basePin = -1, upperPin = -1;
    int minBasePos = 0, maxBasePos = 180;
    int minUpperPos = 0, maxUpperPos = 180;
    int focusDistMm = 500;
    int armLengthMm = 150;
    int minBeamAngleDeg = -20, maxBeamAngleDeg = 20;

    // Hardware state — LEDC channels for the two servos.
    int8_t baseChannel = -1;
    int8_t upperChannel = -1;

    // Demo state: independent phase accumulators for each parameter,
    // reset on demo start.
    uint32_t demoP1PhaseMs = 0;
    uint32_t demoP2PhaseMs = 0;

    // Compute the two servo angles from parameter fractions in [0,1].
    // Writes into a1Out / a2Out; a2 is clamped to the upper servo's
    // configured safe range before returning.
    void computeAngles(float p1Frac, float p2Frac,
                       float& a1Out, float& a2Out) const;

    // Write an angle (in degrees, [0, 180]) to the given LEDC channel
    // using the standard 500-2500 µs pulse range.
    void writeServoAngle(int8_t channel, float angleDeg) const;
};
