// SoundSinth: the family-independent part of the batch renderer of
// fm_engine_base::generate_channels: the shared constant tables, the
// envelope stepping and the channel kernels. The register class only
// prepares the operators and answers the phase step and AM offset queries
// of a piece. ymfm_batch.cpp has the per-piece driver, ymfm_batch_hot.cpp
// the per-sample loops (the firmware keeps that file in SRAM).
// BSD-3-Clause, as the rest of ymfm.

#ifndef YMFM_BATCH_H
#define YMFM_BATCH_H

#pragma once

#include "ymfm.h"

namespace ymfm
{

// constant tables of all FM families, one copy for all chips
struct fm_tables
{
	uint16_t waveform[2][0x400];  // the sine and the derived square (OPL3 waveforms 0, 7): log-sin attenuation, sign in bit 15
	uint16_t power[256];          // attenuation_to_volume() mantissas
	uint32_t increment[64];       // attenuation_increment() steps, 4 bits per index
	int16_t opm_lfo[3][256];      // OPM LFO saw, square, triangle: AM in the low 8 bits, PM in the high 8
};
void build_fm_tables(fm_tables &tables);

// a waveform of the OPL families as a lookup in fm_tables::waveform:
// waveforms 1-6 are the sine with the index doubled, masked or muted
struct wave_ref
{
	uint16_t const *table;
	uint16_t imask;  // index bits kept after the shift
	uint16_t mute;   // index bits that give zero
	uint16_t shift;  // 1: two periods in the first half
	uint16_t zero;   // the sine at index 0
};
wave_ref batch_wave(uint16_t const (*tables)[0x400], uint32_t waveform);

// the waveform value at a phase: attenuation, sign in bit 15
inline uint32_t batch_wave_sample(wave_ref const &w, uint32_t phase)
{
	uint32_t index = phase & 0x3ff;
	uint32_t s = w.table[(index << w.shift) & w.imask];
	return (index & w.mute) ? w.zero : s;
}

// envelope counter of clock() over a batch: each chip sample adds 4
// (divider 1) or steps the low 2 bits 1, 2, carry (divider 3); the envelope
// clocks when they are 0, with the high bits as the argument
struct batch_clock
{
	uint32_t high, low;  // m_env_counter before the batch: >> 2 and & 3
	uint32_t div;        // EG_CLOCK_DIVIDER: 1 or 3

	bool at(uint32_t chip, uint32_t &arg) const
	{
		uint32_t q = low + chip + 1;
		if (div == 1)
		{
			arg = high + q;
			return true;
		}
		if (q % 3 != 0)
			return false;
		arg = high + q / 3;
		return true;
	}

	// first chip sample >= from where the envelope clocks: the state
	// transitions of clock_envelope() happen only there
	uint32_t tick(uint32_t from) const
	{
		uint32_t q = low + from + 1;
		return from + (div - q % div) % div;
	}

	// first chip sample >= from where the envelope clocks with an argument
	// divisible by period (a power of 2)
	uint32_t next(uint32_t from, uint32_t period) const
	{
		uint32_t arg;
		return next(from, period, arg);
	}
	// the same, arg is the argument of that clock
	uint32_t next(uint32_t from, uint32_t period, uint32_t &arg) const
	{
		arg = high + (low + from + div) / div;
		arg += (0 - arg) & (period - 1);
		return (arg - high) * div - low - 1;
	}

	// m_env_counter after chip sample chip
	uint32_t counter(uint32_t chip) const
	{
		uint32_t q = low + chip + 1;
		return ((high + q / div) << 2) + q % div;
	}
};

// operator state the batch renderer steps: the live envelope of the
// operator, its prepared values, the phase as 10.10 << 12 plus 12 more
// fraction bits
static constexpr uint32_t BATCH_PHASE_DYNAMIC = 1;  // opdata_cache::PHASE_STEP_DYNAMIC
static constexpr uint32_t BATCH_EG_QUIET = 0x380;   // fm_operator::EG_QUIET
static constexpr uint32_t BATCH_ENV_QUIET = 0x1000; // batch_envelope(): the operator is quiet - an attenuation that shifts every power[] value out

struct batch_op
{
	uint16_t *att;          // m_env_attenuation
	envelope_state *state;  // m_env_state
	uint8_t const *rate;    // eg_rate of the cache, per state
	uint32_t sustain;       // eg_sustain
	uint32_t shift;         // eg_shift
	uint32_t phase_step;    // phase_step, BATCH_PHASE_DYNAMIC: asked per piece
	uint32_t total_level;
	uint32_t phase;
	uint8_t *inverted;      // m_ssg_inverted
	uint8_t ssg;            // SSG-EG: 0 - off, 8 | mode
};

// the register class answers per piece
struct batch_source
{
	virtual uint32_t phase_step(uint32_t k, int32_t pm) const = 0;  // operator k at PM LFO value pm
	virtual uint32_t am_offset(uint32_t k, uint32_t am) const = 0;  // operator k at AM LFO state am, 0 if off
};

// the batch: per output sample and per chip sample grids of
// generate_channels
struct batch_frame
{
	batch_clock clock;      // envelope counter before the batch
	uint16_t const *chips;  // first chip sample of each output sample
	uint8_t const *owner;   // output sample of each chip sample
	uint8_t const *am;      // AM LFO state per output sample (lfo_am_raw)
	int8_t const *pm;       // PM LFO value per output sample
	uint8_t const *noise;   // noise bit per output sample
	uint8_t const *frac;    // output sample past its last chip sample, 1/256 of a chip sample
	uint32_t ratio;         // chip samples per output sample, Q24
	fm_tables const *tables;
};

// the envelope at the full attenuation in sustain or release: no clock
// changes it or its state
inline bool batch_envelope_ended(uint32_t att, uint32_t state)
{
	return att == 0x3ff && state >= EG_SUSTAIN;
}

// the transitions at the top of clock_envelope(), done there on every clock
inline void batch_settle(batch_op &op)
{
	if (*op.state == EG_ATTACK && *op.att == 0)
		*op.state = EG_DECAY;
	if (*op.state == EG_DECAY && *op.att >= op.sustain)
		*op.state = EG_SUSTAIN;
}

// the attenuation as compute_volume() adds it, BATCH_ENV_QUIET when quiet;
// add is the AM offset plus the total level
inline uint32_t batch_envelope(batch_op const &op, uint32_t add)
{
	if (*op.att > BATCH_EG_QUIET)
		return BATCH_ENV_QUIET;
	uint32_t e = (*op.att >> op.shift) + add;
	return ((e < 0x3ff) ? e : 0x3ff) << 2;
}

// the sine alone, for the families with one waveform
struct wave_sine
{
	uint16_t const *table;
	wave_sine() = default;
	wave_sine(wave_ref const &w) : table(w.table) {}
};
inline uint32_t batch_wave_sample(wave_sine const &w, uint32_t phase)
{
	return w.table[phase & 0x3ff];
}

// compute_volume() from a waveform value and an attenuation of
// batch_envelope(): the sum stays below the sign in bit 15
inline int32_t batch_volume(uint16_t const *power, uint32_t s, uint32_t env)
{
	uint32_t a = s + env;
	int32_t r = power[a & 0xff] >> ((a >> 8) & 0x7f);
	return bitfield(a, 15) ? -r : r;
}
inline int32_t batch_volume(uint16_t const *power, wave_ref const &wave, uint32_t phase, uint32_t env)
{
	return batch_volume(power, batch_wave_sample(wave, phase), env);
}

// clock_envelope() without SSG-EG, depress and reverb for runs of chip
// samples; the transitions are already done, the rate and step pattern are
// kept per state. The envelope is a local copy, store() writes it back.
struct envelope_stepper
{
	batch_clock clock;
	uint32_t const *increment;
	uint8_t const *rates;
	uint32_t sustain, shift, limit;
	uint16_t att;
	uint8_t ssg;
	bool inverted;
	uint32_t state, rate = 0, period_mask = 0, index_shift = 0;
	envelope_stepper(batch_op const &op, batch_frame const &f, uint32_t l) :
		clock(f.clock), increment(f.tables->increment), rates(op.rate), sustain(op.sustain), shift(op.shift), limit(l), att(*op.att), ssg(op.ssg), inverted(*op.inverted != 0), state(*op.state) { prepare(); }
	void prepare();
	// first chip sample >= chip where the envelope steps, at is the clock argument; ~0 if never
	uint32_t next(uint32_t chip, uint32_t &at) const { return (rate < 2 || batch_envelope_ended(att, state)) ? ~0u : clock.next(chip, period_mask + 1, at); }
	// a step on every chip sample: walking them is cheaper than the jumps
	bool dense() const { return period_mask == 0 && clock.div == 1 && rate >= 2 && !batch_envelope_ended(att, state); }
	// the step after the one at chip with the argument at: the clocks are div chip samples apart
	uint32_t after(uint32_t chip, uint32_t &at) const
	{
		if (rate < 2 || batch_envelope_ended(att, state))
			return ~0u;
		uint32_t a = at + 1;
		a += (0 - a) & period_mask;
		chip += (a - at) * clock.div;
		at = a;
		return chip;
	}
	void apply(uint32_t chip, uint32_t at);
	void step(uint32_t chip, uint32_t chip_end);
	// SSG-EG, one chip sample: clock_ssg_eg_state() and the envelope clock;
	// true - the phase restarts at this sample (ymfm_batch.cpp)
	bool ssg_clock(uint32_t chip);
	void store(batch_op &op) const { *op.att = att; *op.state = envelope_state(state); *op.inverted = inverted; }
	// batch_envelope() of the local copy, inverted by SSG-EG
	uint32_t value(uint32_t add) const
	{
		if (att > BATCH_EG_QUIET)
			return BATCH_ENV_QUIET;
		uint32_t e = att >> shift;
		if (inverted)
			e = (0x200 - e) & 0x3ff;
		e += add;
		return ((e < 0x3ff) ? e : 0x3ff) << 2;
	}
};

// envelope_stepper over chip samples [chip, chip_end) of op
void batch_step_envelope(batch_op &op, batch_frame const &f, uint32_t limit, uint32_t chip, uint32_t chip_end);

// batch_envelope() of output samples [c, c_end) of op into buf, stepping the
// envelope between them; the chip samples of sample from are already done
void batch_fill_envelope(batch_op &op, batch_frame const &f, uint32_t limit, uint32_t from, uint32_t c, uint32_t c_end, uint32_t add, uint16_t *buf);

// The same of an SSG-EG operator, chip sample by chip sample (ymfm_batch.cpp).
// The step: true - the phase restarts in the range. The fill: the mask of
// the output samples from c where it restarts.
bool batch_step_ssg(batch_op &op, batch_frame const &f, uint32_t limit, uint32_t chip, uint32_t chip_end);
uint32_t batch_fill_ssg(batch_op &op, batch_frame const &f, uint32_t limit, uint32_t from, uint32_t c, uint32_t c_end, uint32_t add, uint16_t *buf);

// The channel kernels. 2-op and 4-op go operator by operator: each operator
// renders a chunk of output samples into its own row, the next one reads the
// rows it is modulated by. The rhythm kernel goes sample by sample:
// kernel(j, ph, env) makes output sample j from the operator phases (10.10
// shifted up by 12) and attenuations. kernel.idle(i, end) stands in while
// the channel is inactive.
//
// The feedback loop of operator 1 is a recursion per chip sample: run per
// output sample it oscillates at a third of the output rate instead of a
// third of the chip rate. With feedback on and the chip rate off the output
// rate, operator 1 goes once per chip sample and its row takes the output
// between the last two chip samples: frac / 256 of the way to the last one.
// Otherwise it goes once per output sample.

// 2-op, 4-op and bass drum carry the feedback history
struct feedback_kernel
{
	int16_t *out;
	uint16_t const *power;
	uint32_t feedback;
	int32_t fb0, fb1, fbin;
	bool recursive() const { return feedback != 0; }
	void idle(uint32_t i, uint32_t end)
	{
		for (uint32_t j = i; j < end; j++)
		{
			fb0 = fb1;
			fb1 = fbin;
			out[j] = 0;
		}
	}
};

struct kernel_2op : feedback_kernel
{
	wave_ref w[2];
	bool any, serial, delay;  // delay: MODULATOR_DELAY
	int32_t rshift, clipmax;
	int32_t pmod;             // -1; 0 - serial without the modulator on the carrier phase (bass drum, CNT set)
};

// high hat + snare (channel 7) and tom + cymbal (channel 8): the phases of
// operators 0 and 3 (13 and 17 of OPL) select the hat and cymbal phases
struct kernel_rhythm
{
	int16_t *out7, *out8;
	uint16_t const *power;
	uint8_t const *noise;
	wave_ref w13, w16, w14, w17;
	bool act7, act8, any7, any8;
	int32_t rshift, clipmax;
	void idle(uint32_t i, uint32_t end)
	{
		for (uint32_t j = i; j < end; j++)
		{
			if (out7 != nullptr)
				out7[j] = 0;
			if (out8 != nullptr)
				out8[j] = 0;
		}
	}
	bool recursive() const { return false; }
	void operator()(uint32_t j, uint32_t const *ph, uint32_t const *e)
	{
		uint32_t op13phase = ph[0] >> 22;
		uint32_t op17phase = ph[3] >> 22;
		uint32_t phase_select = (bitfield(op13phase, 2) ^ bitfield(op13phase, 7)) | bitfield(op13phase, 3) | (bitfield(op17phase, 5) ^ bitfield(op17phase, 3));
		if (out7 != nullptr)
		{
			int32_t result = 0;
			if (act7)
			{
				uint32_t noise_state = noise[j];
				uint32_t phase = (phase_select << 9) | (0xd0 >> (2 * (noise_state ^ phase_select)));
				result = batch_volume(power, w13, phase, e[0]) >> rshift;
				phase = (0x100 << bitfield(op13phase, 8)) ^ (noise_state << 8);
				result += batch_volume(power, w16, phase, e[1]) >> rshift;
				result = any7 ? clamp(result, -clipmax - 1, clipmax) * 2 : 0;
			}
			out7[j] = int16_t(result);
		}
		if (out8 != nullptr)
		{
			int32_t result = 0;
			if (act8)
			{
				result = batch_volume(power, w14, ph[2] >> 22, e[2]) >> rshift;
				uint32_t phase = 0x100 | (phase_select << 9);
				result += batch_volume(power, w17, phase, e[3]) >> rshift;
				result = any8 ? clamp(result, -clipmax - 1, clipmax) * 2 : 0;
			}
			out8[j] = int16_t(result);
		}
	}
};

// 4-op of every family (OPL3 pairs, OPN, OPM): the connections of
// output_4op() as sets of operators 1-3 (bits 0-2) - the outputs that
// modulate operators 2, 3, 4 (one or the sum of two) and the ones added to
// the output of operator 4.
uint32_t batch_algorithm(uint32_t algorithm);  // 0-7 of OPN/OPM, 8-11 of OPL3: in2 | in3 << 4 | in4 << 8 | outs << 12
struct kernel_4op : feedback_kernel
{
	wave_ref w[4];
	uint32_t connections;
	bool any;
	int32_t rshift, clipmax;
};

// output samples [from, to) of N operators through kernel (ymfm_batch.cpp);
// active false: output() skips the channel, the state moves on
template<uint32_t N, class Kernel>
void batch_render(batch_op *ops, batch_source const &src, batch_frame const &frame, uint32_t from, uint32_t to, bool active, Kernel &kernel);

// a piece of batch_render: phase steps and slow attenuations are constant
template<uint32_t N>
struct batch_piece
{
	batch_op *ops;
	batch_frame const *frame;
	uint32_t limit;         // the chip sample after the batch part
	uint32_t from, to;      // output samples
	uint32_t ph[N], step[N], add[N], env[N];
	uint32_t feed_step;     // operator 1 per feed: step[0], per chip sample - not scaled by the ratio
	bool chip;              // operator 1 per chip sample
	bool fast[N];           // envelope stepping every 64 chip samples or more often
	bool any_fast;
};

// the per-sample loops of a piece (ymfm_batch_hot.cpp)
void batch_run(batch_piece<2> &p, kernel_2op &kernel);
void batch_run(batch_piece<4> &p, kernel_rhythm &kernel);
void batch_run(batch_piece<4> &p, kernel_4op &kernel);

}

#endif // YMFM_BATCH_H
