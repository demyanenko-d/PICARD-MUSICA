// SoundSinth: the per-sample loops of the batch renderer, the same for all
// FM families; the firmware keeps this file in SRAM. BSD-3-Clause, as the
// rest of ymfm.

#include "ymfm_batch.h"

#if defined(_MSC_VER)
#define BATCH_NOINLINE __declspec(noinline)
#else
#define BATCH_NOINLINE __attribute__((noinline))
#endif

namespace ymfm
{

void envelope_stepper::prepare()
{
	rate = rates[state];
	uint32_t rate_shift = rate >> 2;
	period_mask = (rate_shift >= 11) ? 0 : (1u << (11 - rate_shift)) - 1;
	index_shift = (rate_shift >= 11) ? 0 : 11 - rate_shift;
}

// the envelope clock at chip sample chip with the argument at; one copy
// for every caller
BATCH_NOINLINE void envelope_stepper::apply(uint32_t chip, uint32_t at)
{
	uint32_t inc = (increment[rate] >> (4 * ((at >> index_shift) & 7))) & 15;
	if (state == EG_ATTACK)
	{
		if (rate < 62)
			att += (~att * inc) >> 4;
	}
	else
	{
		att += inc;
		if (att >= 0x400)
			att = 0x3ff;
	}
	// the transitions wait for the next envelope clock: past limit a
	// prepare may change the sustain level first
	if (chip + clock.div < limit)
	{
		uint32_t was = state;
		if (state == EG_ATTACK && att == 0)
			state = EG_DECAY;
		if (state == EG_DECAY && att >= sustain)
			state = EG_SUSTAIN;
		if (state != was)
			prepare();
	}
}

BATCH_NOINLINE void envelope_stepper::step(uint32_t chip, uint32_t chip_end)
{
	uint32_t at;
	for (; chip < chip_end && dense(); chip++)
		if (clock.at(chip, at))
			apply(chip, at);
	if (chip >= chip_end)
		return;
	for (chip = next(chip, at); chip < chip_end; chip = after(chip, at))
		apply(chip, at);
}

void batch_step_envelope(batch_op &op, batch_frame const &f, uint32_t limit, uint32_t chip, uint32_t chip_end)
{
	envelope_stepper st(op, f, limit);
	st.step(chip, chip_end);
	st.store(op);
}

void batch_fill_envelope(batch_op &op, batch_frame const &f, uint32_t limit, uint32_t from, uint32_t c, uint32_t c_end, uint32_t add, uint16_t *buf)
{
	uint16_t const *chips = f.chips;
	envelope_stepper st(op, f, limit);
	if (st.dense())
	{
		for (uint32_t j = c; j < c_end; j++)
		{
			if (j != from)
				st.step(chips[j], chips[j + 1]);
			buf[j - c] = uint16_t(st.value(add));
		}
		st.store(op);
		return;
	}
	// the value holds until the output sample of the next step
	uint32_t at;
	uint32_t chip = st.next(chips[(c == from) ? c + 1 : c], at), chip_end = chips[c_end];
	for (uint32_t j = c;;)
	{
		uint32_t jt = (chip < chip_end) ? f.owner[chip] : c_end;
		uint16_t v = uint16_t(st.value(add));
		for (; j < jt; j++)
			buf[j - c] = v;
		if (chip >= chip_end)
			break;
		st.apply(chip, at);
		chip = st.after(chip, at);
	}
	st.store(op);
}

// an operator over a chunk of output samples: the phase, its step and the
// row of attenuations
struct op_pass
{
	uint32_t ph, step;
	uint16_t const *env;
};

static constexpr uint32_t CHUNK = 32;

// a waveform that is one whole table: the sine, the OPL3 square
static bool whole_table(wave_ref const &w)
{
	return w.imask == 0x3ff && w.mute == 0 && w.shift == 0;
}

// the attenuation rows of a piece: one value for a slow envelope
template<uint32_t N>
static void piece_envelopes(batch_piece<N> const &p, uint16_t (*row)[CHUNK])
{
	for (uint32_t k = 0; k < N; k++)
		if (!p.fast[k])
			std::fill(row[k], row[k] + CHUNK, uint16_t(p.env[k]));
}

// the attenuation rows of a chunk: per sample for a fast envelope; restarts -
// per operator, the samples where SSG-EG restarts the phase
template<uint32_t N>
static void chunk_envelopes(batch_piece<N> const &p, uint16_t (*row)[CHUNK], uint32_t c, uint32_t n, uint32_t *restarts = nullptr)
{
	for (uint32_t k = 0; k < N; k++)
	{
		if (!p.fast[k])
			continue;
		if (restarts != nullptr && p.ops[k].ssg != 0)
			restarts[k] = batch_fill_ssg(p.ops[k], *p.frame, p.limit, p.from, c, c + n, p.add[k], row[k]);
		else
			batch_fill_envelope(p.ops[k], *p.frame, p.limit, p.from, c, c + n, p.add[k], row[k]);
	}
}

// n samples of an operator in runs: the phase restarts at the samples of the
// mask; pass(at, count) renders a run
template<class Pass>
static void in_runs(op_pass &o, uint32_t restarts, uint32_t n, Pass &&pass)
{
	uint16_t const *const env = o.env;
	for (uint32_t at = 0; at < n; )
	{
		uint32_t end = n;
		if (restarts != 0)
		{
			if (bitfield(restarts, at))
				o.ph = 0;
			for (end = at + 1; end < n && !bitfield(restarts, end); end++)
				;
			o.env = env + at;
		}
		pass(at, end - at);
		at = end;
	}
	o.env = env;
}

// the same without rendering: only the phase moves
static void skip_runs(op_pass &o, uint32_t restarts, uint32_t n)
{
	if (restarts != 0)
	{
		uint32_t last = n - 1;
		while (!bitfield(restarts, last))
			last--;
		o.ph = 0;
		n -= last;
	}
	o.ph += o.step * n;
}

// operator 1 with its feedback into dst: once per output sample, or per chip
// sample (chips not null) and then the output between the last two
template<class Wave>
BATCH_NOINLINE static void pass_fb(feedback_kernel &k, Wave const &wave, op_pass &o, int16_t *__restrict dst, bool delay, uint16_t const *chips, uint8_t const *frac, uint32_t n)
{
	Wave const w = wave;
	uint16_t const *const power = k.power;
	uint16_t const *const env = o.env;
	// (fb0 + fb1) >> (10 - feedback) as a multiplication: zero without feedback
	uint32_t const step = o.step;
	int32_t const fmul = (k.feedback != 0) ? 1 << (k.feedback - 1) : 0;
	uint32_t ph = o.ph;
	int32_t fb0 = k.fb0, fb1 = k.fb1, fbin = k.fbin;
	if (chips == nullptr)
	{
		for (uint32_t i = 0; i < n; i++)
		{
			ph += step;
			int32_t const v = batch_volume(power, batch_wave_sample(w, (ph >> 22) + (((fb1 + fbin) * fmul) >> 9)), env[i]);
			fb0 = fb1;
			fb1 = fbin;
			fbin = v;
			dst[i] = int16_t(delay ? fb1 : fbin);
		}
	}
	else
	{
		uint32_t chip = chips[0];
		for (uint32_t i = 0; i < n; i++)
		{
			uint32_t const e = env[i], end = chips[i + 1];
			for (; chip != end; chip++)
			{
				ph += step;
				int32_t const v = batch_volume(power, batch_wave_sample(w, (ph >> 22) + (((fb1 + fbin) * fmul) >> 9)), e);
				fb0 = fb1;
				fb1 = fbin;
				fbin = v;
			}
			int32_t a = delay ? fb0 : fb1, b = delay ? fb1 : fbin;
			dst[i] = int16_t(a + (((b - a) * frac[i]) >> 8));
		}
	}
	o.ph = ph;
	k.fb0 = fb0;
	k.fb1 = fb1;
	k.fbin = fbin;
}

// an operator modulated by the row mod into dst, shifted down by rs
template<class Wave>
BATCH_NOINLINE static void pass_op(uint16_t const *power, Wave const &wave, op_pass &o, int16_t const *mod, int16_t *__restrict dst, uint32_t rs, uint32_t n)
{
	Wave const w = wave;
	uint16_t const *const env = o.env;
	uint32_t const step = o.step;
	uint32_t ph = o.ph;
	for (uint32_t i = 0; i < n; i++)
	{
		ph += step;
		dst[i] = int16_t(batch_volume(power, batch_wave_sample(w, (ph >> 22) + (mod[i] >> 1)), env[i]) >> rs);
	}
	o.ph = ph;
}

// the pass of the waveform: a whole table goes without the masks
static void run_fb(feedback_kernel &k, wave_ref const &w, op_pass &o, int16_t *dst, bool delay, uint16_t const *chips, uint8_t const *frac, uint32_t n)
{
	if (whole_table(w))
		pass_fb(k, wave_sine(w), o, dst, delay, chips, frac, n);
	else
		pass_fb(k, w, o, dst, delay, chips, frac, n);
}
static void run_op(uint16_t const *power, wave_ref const &w, op_pass &o, int16_t const *mod, int16_t *dst, uint32_t rs, uint32_t n)
{
	if (whole_table(w))
		pass_op(power, wave_sine(w), o, mod, dst, rs, n);
	else
		pass_op(power, w, o, mod, dst, rs, n);
}

// one more operator on the output: each addition is clamped
BATCH_NOINLINE static void pass_mix(int16_t *__restrict out, int16_t const *row, uint32_t rs, int32_t clipmax, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++)
		out[i] = int16_t(clamp(out[i] + (row[i] >> rs), -clipmax - 1, clipmax));
}

void batch_run(batch_piece<2> &p, kernel_2op &k)
{
	uint16_t env[2][CHUNK];
	int16_t row[2][CHUNK];  // operator 1, zero
	op_pass op[2] = {{p.ph[0], p.feed_step, env[0]}, {p.ph[1], p.step[1], env[1]}};
	bool const mod = k.serial && k.pmod != 0;
	if (!mod)
		std::fill(row[1], row[1] + CHUNK, int16_t(0));
	piece_envelopes(p, env);
	for (uint32_t c = p.from; c < p.to; c += CHUNK)
	{
		uint32_t n = std::min(CHUNK, p.to - c);
		chunk_envelopes(p, env, c, n);
		run_fb(k, k.w[0], op[0], row[0], k.delay, p.chip ? p.frame->chips + c : nullptr, p.frame->frac + c, n);
		int16_t *out = k.out + c;
		if (!k.any)
		{
			std::fill(out, out + n, int16_t(0));
			op[1].ph += op[1].step * n;
			continue;
		}
		run_op(k.power, k.w[1], op[1], row[mod ? 0 : 1], out, k.rshift, n);
		if (!k.serial)
			pass_mix(out, row[0], k.rshift, k.clipmax, n);
	}
	p.ph[0] = op[0].ph;
	p.ph[1] = op[1].ph;
}

// An operator on a slow envelope at an attenuation that shifts every power[]
// value out (they are below 1 << 13) gives zero for the whole piece: it and
// the operators nobody hears are not rendered, only their phases move.
static constexpr uint32_t ENV_ZERO = 0xd00;

void batch_run(batch_piece<4> &p, kernel_4op &k)
{
	uint16_t env[4][CHUNK];
	int16_t row[5][CHUNK];  // the outputs of operators 1-3, a sum of two, zero
	op_pass op[4];
	uint32_t restarts[4] = {};
	bool quiet[4], heard[4];
	for (uint32_t i = 0; i < 4; i++)
	{
		op[i] = {p.ph[i], (i == 0) ? p.feed_step : p.step[i], env[i]};
		quiet[i] = !p.fast[i] && p.env[i] >= ENV_ZERO;
	}
	// heard: not quiet and on the output or modulating a heard operator
	heard[3] = !quiet[3];
	for (uint32_t i = 3; i-- != 0; )
	{
		bool used = bitfield(k.connections, 12 + i);
		for (uint32_t j = i + 1; j < 4; j++)
			used |= heard[j] && bitfield(k.connections, 4 * (j - 1) + i);
		heard[i] = !quiet[i] && used;
	}
	int16_t *const zero = row[4];
	std::fill(zero, zero + CHUNK, int16_t(0));
	piece_envelopes(p, env);
	for (uint32_t c = p.from; c < p.to; c += CHUNK)
	{
		uint32_t n = std::min(CHUNK, p.to - c);
		chunk_envelopes(p, env, c, n, restarts);
		uint16_t const *chips = p.chip ? p.frame->chips + c : nullptr;
		uint8_t const *frac = p.frame->frac + c;
		int16_t const *src[3] = {row[0], zero, zero};
		// operator 1 keeps the feedback history: skipped once that is all zero
		if (quiet[0] && (k.fb0 | k.fb1 | k.fbin) == 0)
		{
			op[0].ph += op[0].step * (chips != nullptr ? uint32_t(chips[n] - chips[0]) : n);
			src[0] = zero;
		}
		else
			in_runs(op[0], restarts[0], n, [&](uint32_t at, uint32_t m) { run_fb(k, k.w[0], op[0], row[0] + at, false, chips != nullptr ? chips + at : nullptr, frac + at, m); });
		int16_t *out = k.out + c;
		if (!k.any)
		{
			std::fill(out, out + n, int16_t(0));
			for (uint32_t i = 1; i < 4; i++)
				skip_runs(op[i], restarts[i], n);
			continue;
		}
		for (uint32_t i = 1; i < 4; i++)
		{
			if (!heard[i])
			{
				skip_runs(op[i], restarts[i], n);
				if (i == 3)
					std::fill(out, out + n, int16_t(0));
				continue;
			}
			// the modulation: nothing, one row or the sum of two
			uint32_t set = bitfield(k.connections, 4 * (i - 1), 4);
			int16_t const *mod = zero;
			for (uint32_t m = 0; m < 3; m++)
			{
				if (!bitfield(set, m) || src[m] == zero)
					continue;
				if (mod != zero)
				{
					for (uint32_t j = 0; j < n; j++)
						row[3][j] = int16_t(mod[j] + src[m][j]);
					mod = row[3];
				}
				else
					mod = src[m];
			}
			int16_t *const dst = (i < 3) ? row[i] : out;
			in_runs(op[i], restarts[i], n, [&](uint32_t at, uint32_t m) { run_op(k.power, k.w[i], op[i], mod + at, dst + at, (i < 3) ? 0 : k.rshift, m); });
			if (i < 3)
				src[i] = row[i];
		}
		for (uint32_t i = 0; i < 3; i++)
			if (bitfield(k.connections, 12 + i) && src[i] != zero)
				pass_mix(out, src[i], k.rshift, k.clipmax, n);
	}
	for (uint32_t i = 0; i < 4; i++)
		p.ph[i] = op[i].ph;
}

// the rhythm kernel sample by sample
void batch_run(batch_piece<4> &p, kernel_rhythm &kernel)
{
	constexpr uint32_t N = 4;
	uint16_t env[N][CHUNK];
	uint32_t ph[N], step[N];
	for (uint32_t k = 0; k < N; k++)
	{
		ph[k] = p.ph[k];
		step[k] = p.step[k];
	}
	piece_envelopes(p, env);
	for (uint32_t c = p.from; c < p.to; c += CHUNK)
	{
		uint32_t n = std::min(CHUNK, p.to - c);
		chunk_envelopes(p, env, c, n);
		for (uint32_t j = 0; j < n; j++)
		{
			uint32_t e[N];
			for (uint32_t k = 0; k < N; k++)
			{
				e[k] = env[k][j];
				ph[k] += step[k];
			}
			kernel(c + j, ph, e);
		}
	}
	for (uint32_t k = 0; k < N; k++)
		p.ph[k] = ph[k];
}

}
