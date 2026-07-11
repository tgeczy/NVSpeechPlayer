/*
TGSpeechBox — All-pole resonator and pitch-synchronous F1 resonator.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

#ifndef TGSPEECHBOX_RESONATOR_H
#define TGSPEECHBOX_RESONATOR_H

#include "dspCommon.h"

class Resonator {
private:
    int sampleRate;
    double frequency;
    double bandwidth;
    bool anti;
    bool setOnce;

    // All-pole resonator: DF1 output history and coefficients
    double y1, y2;             // output delay line
    double dfB0, dfFb1, dfFb2; // feedforward + feedback coefficients

    // FIR anti-resonator state and coefficients
    // The Klatt anti-resonator is an all-zero (FIR) filter -- the inverse
    // of a resonator transfer function.  Its zeros sit OFF the unit circle,
    // so the null depth is finite and controlled by bandwidth.
    double firA, firB, firC;   // FIR coefficients
    double z1, z2;             // FIR delay line

    // Flag: true when filter is disabled (passthrough)
    bool disabled;

public:
    Resonator(int sampleRate, bool anti=false)
        : sampleRate(sampleRate), frequency(0.0), bandwidth(0.0),
          anti(anti), setOnce(false),
          y1(0.0), y2(0.0),
          dfB0(0.0), dfFb1(0.0), dfFb2(0.0),
          firA(1.0), firB(0.0), firC(0.0), z1(0.0), z2(0.0),
          disabled(true) {}

    void setParams(double frequency, double bandwidth) {
        if(!setOnce||(frequency!=this->frequency)||(bandwidth!=this->bandwidth)) {
            this->frequency=frequency;
            this->bandwidth=bandwidth;

            // Memoization above keys on the raw request; the clamp below only
            // shapes what we synthesize from it.
            clampResonatorToNyquist(frequency, bandwidth, sampleRate);

            const double nyquist = 0.5 * (double)sampleRate;
            const bool invalid = (!std::isfinite(frequency) || !std::isfinite(bandwidth));
            const bool off = (frequency <= 0.0 || bandwidth <= 0.0 || frequency >= nyquist);

            if (invalid || off) {
                disabled = true;
                if (anti) {
                    firA = 1.0; firB = 0.0; firC = 0.0;
                } else {
                    dfB0 = 0.0; dfFb1 = 0.0; dfFb2 = 0.0;
                }
                setOnce = true;
                return;
            }

            disabled = false;

            if (anti) {
                // FIR anti-resonator: places zeros at z = r * e^(+/-j*theta)
                // where r = exp(-pi*bw/sr), theta = 2*pi*f/sr.
                // Transfer function: H(z) = (1/a)(1 - 2r*cos(theta)*z^-1 + r^2*z^-2)
                // where a is the DC gain of the corresponding resonator,
                // giving unity passband gain away from the zero.
                double r = exp(-M_PI / sampleRate * bandwidth);
                double cosTheta = cos(2.0 * M_PI * frequency / (double)sampleRate);
                double resA = 1.0 - 2.0 * r * cosTheta + r * r;
                // Guard against division by ~0 for extreme params
                if (!std::isfinite(resA) || fabs(resA) < 1e-12) {
                    firA = 1.0; firB = 0.0; firC = 0.0;
                } else {
                    double invA = 1.0 / resA;
                    firA = invA;
                    firB = -2.0 * r * cosTheta * invA;
                    firC = r * r * invA;
                }
            } else {
                // Bilinear-transform frequency warp
                double g = tan(M_PI * frequency / (double)sampleRate);
                double g2 = g * g;

                // Compute damping k to exactly match Klatt pole radius.
                //
                // The Klatt biquad places poles at radius r = exp(-pi*bw/sr).
                // For the DF1 form y[n] = b0*x[n] + fb1*y[n-1] + fb2*y[n-2],
                // the pole-magnitude squared equals (1-kg+g^2)/(1+kg+g^2).
                // Setting this equal to R = r^2 = exp(-2*pi*bw/sr) and solving
                // for k gives the formula below.
                //
                // This replaces the previous k=bw/f + Nyquist damping hack.
                // At low frequencies (g<<1) this reduces to ~bw/f.  Near
                // Nyquist it naturally increases damping -- no arbitrary ramp
                // needed.
                double R = exp(-2.0 * M_PI * bandwidth / (double)sampleRate);
                double k = (1.0 - R) * (1.0 + g2) / (g * (1.0 + R));

                // Convert to all-pole DF1 coefficients.
                // H(z) = b0 / (1 - fb1*z^-1 - fb2*z^-2)
                // Unity DC gain: b0 = 1 - fb1 - fb2 = 4*g^2/D
                double D = 1.0 + k * g + g2;
                dfB0  = 4.0 * g2 / D;
                dfFb1 = 2.0 * (1.0 - g2) / D;
                dfFb2 = -(1.0 - k * g + g2) / D;
            }
        }
        this->setOnce=true;
    }

    double resonate(double in, double frequency, double bandwidth, bool allowUpdate=true) {
        if(allowUpdate) setParams(frequency, bandwidth);

        if (disabled) return in;

        if (anti) {
            // FIR anti-resonator: all-zero filter
            // out = firA*in + firB*z1 + firC*z2
            // Delay line stores past INPUTS (not outputs) -- this is FIR.
            double out = firA * in + firB * z1 + firC * z2;
            z2 = z1;
            z1 = in;
            return out;
        } else {
            // All-pole resonator: DF1 with SVF-derived coefficients
            // y[n] = b0*x[n] + fb1*y[n-1] + fb2*y[n-2]
            double out = dfB0 * in + dfFb1 * y1 + dfFb2 * y2;
            y2 = y1;
            y1 = out;
            return out;
        }
    }

    void reset() {
        y1=0.0;
        y2=0.0;
        z1=0.0;
        z2=0.0;
        setOnce=false;
    }

    // Drain residual energy during silence (e.g. preFormantGain ≈ 0).
    // Real vocal tracts don't ring through a closed glottis.
    void decay(double factor) {
        y1 *= factor;
        y2 *= factor;
    }
};

// ============================================================================
// Pitch-synchronous F1 resonator (all-pole with SVF parameterization)
// ============================================================================
// Models the acoustic coupling between glottal source and vocal tract
// during the open phase of voicing.  F1 and B1 are modulated by deltas
// when the glottis is open, with smoothing to prevent clicks at the
// glottal boundary transitions.

class PitchSyncResonator {
private:
    int sampleRate;

    // All-pole DF1 state and coefficients
    double y1, y2;
    double dfB0, dfFb1, dfFb2;
    bool disabled;

    double frequency, bandwidth;
    bool setOnce;

    // Pitch-sync modulation state
    double deltaFreq, deltaBw;
    double lastTargetFreq, lastTargetBw;

    // Smoothing to prevent clicks at glottal open/close boundaries
    double smoothFreq, smoothBw;
    double smoothAlpha;

    void computeCoeffs(double freq, double bw) {
        clampResonatorToNyquist(freq, bw, sampleRate);
        const double nyquist = 0.5 * (double)sampleRate;
        if (!std::isfinite(freq) || !std::isfinite(bw) ||
            freq <= 0.0 || bw <= 0.0 || freq >= nyquist) {
            disabled = true;
            dfB0 = 0.0; dfFb1 = 0.0; dfFb2 = 0.0;
            return;
        }
        disabled = false;

        // Bilinear-transform frequency warp
        double g = tan(M_PI * freq / (double)sampleRate);
        double g2 = g * g;

        // Exact Klatt pole-radius match (see Resonator::setParams)
        double R = exp(-2.0 * M_PI * bw / (double)sampleRate);
        double k = (1.0 - R) * (1.0 + g2) / (g * (1.0 + R));

        // Convert to all-pole DF1 coefficients
        double D = 1.0 + k * g + g2;
        dfB0  = 4.0 * g2 / D;
        dfFb1 = 2.0 * (1.0 - g2) / D;
        dfFb2 = -(1.0 - k * g + g2) / D;
    }

public:
    PitchSyncResonator(int sr) : sampleRate(sr),
        y1(0.0), y2(0.0),
        dfB0(0.0), dfFb1(0.0), dfFb2(0.0),
        disabled(true), frequency(0.0), bandwidth(0.0), setOnce(false),
        deltaFreq(0.0), deltaBw(0.0), lastTargetFreq(0.0), lastTargetBw(0.0),
        smoothFreq(0.0), smoothBw(0.0), smoothAlpha(0.0) {
        // Smooth over ~2ms to prevent clicks at glottal transitions
        double smoothMs = 2.0;
        smoothAlpha = 1.0 - exp(-1.0 / (sampleRate * smoothMs * 0.001));
    }

    void reset() {
        y1 = 0.0;
        y2 = 0.0;
        setOnce = false;
        smoothFreq = 0.0;
        smoothBw = 0.0;
    }

    void decay(double factor) {
        y1 *= factor;
        y2 *= factor;
    }

    void setPitchSyncParams(double dF1, double dB1) {
        deltaFreq = dF1;
        deltaBw = dB1;
    }

    double resonate(double in, double freq, double bw, bool glottisOpen) {
        // Determine target F1/B1 based on glottal phase
        double targetFreq, targetBw;
        if (deltaFreq != 0.0 || deltaBw != 0.0) {
            // Pitch-sync modulation enabled
            if (glottisOpen) {
                targetFreq = freq + deltaFreq;
                targetBw = bw + deltaBw;
            } else {
                targetFreq = freq;
                targetBw = bw;
            }

            // Smooth the transitions to prevent clicks
            if (smoothFreq == 0.0) smoothFreq = targetFreq;
            if (smoothBw == 0.0) smoothBw = targetBw;
            smoothFreq += (targetFreq - smoothFreq) * smoothAlpha;
            smoothBw += (targetBw - smoothBw) * smoothAlpha;

            targetFreq = smoothFreq;
            targetBw = smoothBw;
        } else {
            targetFreq = freq;
            targetBw = bw;
        }

        // Only recompute coefficients when params actually change
        if (!setOnce || targetFreq != lastTargetFreq || targetBw != lastTargetBw) {
            lastTargetFreq = targetFreq;
            lastTargetBw = targetBw;
            computeCoeffs(targetFreq, targetBw);
            setOnce = true;
        }

        if (disabled) return in;

        // All-pole DF1 tick
        double out = dfB0 * in + dfFb1 * y1 + dfFb2 * y2;
        y2 = y1;
        y1 = out;
        return out;
    }
};

#endif // TGSPEECHBOX_RESONATOR_H
