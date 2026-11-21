// SoundSinth: the family-independent batch renderer, per piece: the shared
// tables and the piece cutting of batch_render (the per-sample loops are in
// ymfm_batch_hot.cpp). BSD-3-Clause, as the rest of ymfm.

#include "ymfm_fm.h"
#include "ymfm_fm.ipp"

namespace ymfm
{

void build_fm_tables(fm_tables &t)
{
	// waveforms 0 and 7 of opl_registers_base<3>
	for (uint32_t index = 0; index < 0x400; index++)
	{
		t.waveform[0][index] = abs_sin_attenuation(index) | (bitfield(index, 9) << 15);
		t.waveform[1][index] = (bitfield(index, 9) ? (index ^ 0x13ff) : index) << 3;
	}
	for (uint32_t index = 0; index < 256; index++)
		t.power[index] = uint16_t(attenuation_to_volume(index));
	for (uint32_t rate = 0; rate < 64; rate++)
	{
		t.increment[rate] = 0;
		for (uint32_t index = 0; index < 8; index++)
			t.increment[rate] |= attenuation_increment(rate, index) << (4 * index);
	}
	// the LFO waveforms of opm_registers, the noise one stays per chip
	for (uint32_t index = 0; index < 256; index++)
	{
		uint8_t am = index ^ 0xff;
		uint8_t pm = index;
		t.opm_lfo[0][index] = int16_t(am | (pm << 8));
		am = bitfield(index, 7) ? 0 : 0xff;
		pm = am ^ 0x80;
		t.opm_lfo[1][index] = int16_t(am | (pm << 8));
		am = bitfield(index, 7) ? (index << 1) : ((index ^ 0xff) << 1);
		pm = bitfield(index, 6) ? am : ~am;
		t.opm_lfo[2][index] = int16_t(am | (pm << 8));
	}
}

wave_ref batch_wave(uint16_t const (*tables)[0x400], uint32_t waveform)
{
	// waveforms 1-5 of opl_registers_base<3> from the sine wf0: 1 - the
	// second half zero, 2 - wf0 without the sign, 3 - 2 with the second and
	// fourth quarters zero, 4 - wf0[index * 2] and 5 - wf0[(index * 2) &
	// 0x1ff] with the second half zero; 6, the square, is wf0 at its
	// peaks: wf0[0x100] is 0, wf0[0x300] is 0 with the sign
	static uint16_t const s_lookup[8][4] =
	{   // table offset, imask, mute, shift
		{ 0x000, 0x3ff, 0x000, 0 },
		{ 0x000, 0x3ff, 0x200, 0 },
		{ 0x000, 0x1ff, 0x000, 0 },
		{ 0x000, 0x1ff, 0x100, 0 },
		{ 0x000, 0x3ff, 0x200, 1 },
		{ 0x000, 0x1ff, 0x200, 1 },
		{ 0x100, 0x200, 0x000, 0 },
		{ 0x400, 0x3ff, 0x000, 0 },
	};
	uint16_t const *l = s_lookup[waveform & 7];
	return wave_ref{tables[0] + l[0], l[1], l[2], l[3], tables[0][0]};
}

uint32_t batch_algorithm(uint32_t algorithm)
{
	// the connections of output_4op(): operator 2, 3, 4 inputs as opout[]
	// indexes (1 - O1, 2 - O2, 3 - O3, 5 - O1+O2, 6 - O1+O3, 7 - O2+O3), the
	// outputs of operators 1, 2, 3 added
#define ALGORITHM(op2in, op3in, op4in, op1out, op2out, op3out) \
	((op2in) | ((op3in) << 1) | ((op4in) << 4) | ((op1out) << 7) | ((op2out) << 8) | ((op3out) << 9))
	static uint16_t const s_algorithm_ops[8+4] =
	{
		ALGORITHM(1,2,3, 0,0,0), ALGORITHM(0,5,3, 0,0,0), ALGORITHM(0,2,6, 0,0,0), ALGORITHM(1,0,7, 0,0,0),
		ALGORITHM(1,0,3, 0,1,0), ALGORITHM(1,1,1, 0,1,1), ALGORITHM(1,0,0, 0,1,1), ALGORITHM(0,0,0, 1,1,1),
		ALGORITHM(1,2,3, 0,0,0), ALGORITHM(0,2,3, 1,0,0), ALGORITHM(1,0,3, 0,1,0), ALGORITHM(0,2,0, 1,0,1)
	};
#undef ALGORITHM
	static uint8_t const s_terms[8] = { 0, 1, 2, 4, 0, 3, 5, 6 };
	uint32_t ops = s_algorithm_ops[algorithm];
	return s_terms[bitfield(ops, 0, 1)] | (s_terms[bitfield(ops, 1, 3)] << 4) | (s_terms[bitfield(ops, 4, 3)] << 8) | (bitfield(ops, 7, 3) << 12);
}

// chip sample >= from, below limit, where clock_envelope() changes the
// attenuation; limit if none
static uint32_t next_envelope_step(batch_op const &op, batch_clock const &clock, uint32_t from, uint32_t limit)
{
	// SSG-EG clocks on every chip sample
	if (op.ssg != 0)
		return (from < limit) ? from : limit;
	// rates 0 and 1 never change the attenuation
	uint32_t rate = op.rate[*op.state];
	if (rate < 2 || from >= limit || batch_envelope_ended(*op.att, *op.state))
		return limit;
	uint32_t rate_shift = rate >> 2;
	uint32_t at = clock.next(from, (rate_shift >= 11) ? 1 : 1u << (11 - rate_shift));
	return (at < limit) ? at : limit;
}

bool envelope_stepper::ssg_clock(uint32_t chip)
{
	// clock_ssg_eg_state(): the work starts past the middle
	bool restart = false;
	if (bitfield(att, 9))
	{
		if (bitfield(ssg, 0))
		{
			// the hold modes: the end state
			inverted = bitfield(ssg, 2) ^ bitfield(ssg, 1);
			if (state != EG_ATTACK)
				att = inverted ? 0x200 : 0x3ff;
		}
		else
		{
			// the repeating modes: the attack again, the phase from zero or
			// the inversion toggled
			inverted ^= bitfield(ssg, 1);
			if (state == EG_DECAY || state == EG_SUSTAIN)
			{
				state = EG_ATTACK;
				if (rates[EG_ATTACK] >= 62)
					att = 0;
			}
			restart = !bitfield(ssg, 1);
		}
		if (state == EG_RELEASE)
			att = 0x3ff;
	}
	// clock_envelope(): the transitions on every envelope clock, the step
	// at 4x below the middle
	uint32_t at;
	if (!clock.at(chip, at))
		return restart;
	if (state == EG_ATTACK && att == 0)
		state = EG_DECAY;
	if (state == EG_DECAY && att >= sustain)
		state = EG_SUSTAIN;
	prepare();
	if (rate < 2 || (at & period_mask) != 0)
		return restart;
	uint32_t inc = (increment[rate] >> (4 * ((at >> index_shift) & 7))) & 15;
	if (state == EG_ATTACK)
	{
		if (rate < 62)
			att += (~att * inc) >> 4;
	}
	else if (att < 0x200)
		att += 4 * inc;
	return restart;
}

bool batch_step_ssg(batch_op &op, batch_frame const &f, uint32_t limit, uint32_t chip, uint32_t chip_end)
{
	envelope_stepper st(op, f, limit);
	bool restart = false;
	for (; chip < chip_end; chip++)
		restart |= st.ssg_clock(chip);
	st.store(op);
	return restart;
}

uint32_t batch_fill_ssg(batch_op &op, batch_frame const &f, uint32_t limit, uint32_t from, uint32_t c, uint32_t c_end, uint32_t add, uint16_t *buf)
{
	envelope_stepper st(op, f, limit);
	uint32_t restarts = 0;
	for (uint32_t j = c; j < c_end; j++)
	{
		if (j != from)
			for (uint32_t chip = f.chips[j]; chip < f.chips[j + 1]; chip++)
				if (st.ssg_clock(chip))
					restarts |= 1u << (j - c);
		buf[j - c] = uint16_t(st.value(add));
	}
	st.store(op);
	return restarts;
}

static bool fast_envelope(batch_op const &op)
{
	if (op.ssg != 0)
		return true;
	uint32_t rate = op.rate[*op.state];
	return rate >= 2 && (rate >> 2) >= 5 && !batch_envelope_ended(*op.att, *op.state);
}

// A channel goes in pieces: a piece ends at a slow envelope step or an LFO
// change the channel hears, inside it the phase steps and attenuations are
// constant. An envelope stepping every 64 chip samples or more often does
// not cut pieces: batch_run steps it per sample.
template<uint32_t N, class Kernel>
void batch_render(batch_op *ops, batch_source const &src, batch_frame const &f, uint32_t from, uint32_t to, bool active, Kernel &kernel)
{
	uint16_t const *chips = f.chips;
	uint32_t limit = chips[to];
	auto output_of = [&](uint32_t chip) { return (chip >= limit) ? to : uint32_t(f.owner[chip]); };

	batch_piece<N> p;
	p.ops = ops;
	p.frame = &f;
	p.limit = limit;
	p.chip = f.ratio != (1u << 24) && kernel.recursive();
	uint32_t next[N];
	// the answers of src change with the LFO only: asked again when it moves
	int32_t last_pm = 0x7fffffff;
	uint32_t last_am = 0xffffffff;
	uint32_t am_off[N];
	bool ticks = f.clock.tick(chips[from]) < limit;
	// the LFO moves this channel: a phase step asked per piece, an AM offset
	// not zero at 0x3f - the top of the OPN range, inside the other ranges
	bool pm_cut = false, am_cut = false;
	for (uint32_t k = 0; k < N; k++)
	{
		if (ticks)
			batch_settle(ops[k]);
		next[k] = next_envelope_step(ops[k], f.clock, chips[from], limit);
		p.ph[k] = ops[k].phase;
		pm_cut |= ops[k].phase_step == BATCH_PHASE_DYNAMIC;
		am_cut |= src.am_offset(k, 0x3f) != 0;
	}

	for (uint32_t i = from; i < to; )
	{
		// envelope steps of the chip samples of this output sample; the piece
		// runs until the next slow envelope step or LFO change
		uint32_t end = to;
		p.any_fast = false;
		for (uint32_t k = 0; k < N; k++)
		{
			if (next[k] < chips[i + 1])
			{
				if (ops[k].ssg == 0)
					batch_step_envelope(ops[k], f, limit, next[k], chips[i + 1]);
				else if (batch_step_ssg(ops[k], f, limit, next[k], chips[i + 1]))
					p.ph[k] = 0;
				next[k] = next_envelope_step(ops[k], f.clock, chips[i + 1], limit);
			}
			p.fast[k] = fast_envelope(ops[k]);
			p.any_fast |= p.fast[k];
			if (!p.fast[k])
				end = std::min(end, output_of(next[k]));
		}
		uint32_t lfo_end = (am_cut || pm_cut) ? i + 1 : to;
		while (lfo_end < to && (!am_cut || f.am[lfo_end] == f.am[i]) && (!pm_cut || f.pm[lfo_end] == f.pm[i]))
			lfo_end++;
		end = std::min(end, lfo_end);

		if (f.pm[i] != last_pm)
		{
			last_pm = f.pm[i];
			for (uint32_t k = 0; k < N; k++)
			{
				uint32_t s = ops[k].phase_step;
				if (s == BATCH_PHASE_DYNAMIC)
					s = src.phase_step(k, f.pm[i]);
				p.step[k] = uint32_t(((uint64_t(s) << 12) * f.ratio) >> 24);
				if (k == 0)
					p.feed_step = p.chip ? s << 12 : p.step[0];
			}
		}

		if (!active)
		{
			// output() skips the channel: phases, feedback history and the
			// envelopes move on
			for (uint32_t k = 0; k < N; k++)
			{
				if (p.fast[k])
					batch_step_envelope(ops[k], f, limit, chips[i + 1], chips[end]);
				p.ph[k] += (k == 0) ? p.feed_step * (p.chip ? uint32_t(chips[end] - chips[i]) : end - i) : p.step[k] * (end - i);
			}
			kernel.idle(i, end);
		}
		else
		{
			if (f.am[i] != last_am)
			{
				last_am = f.am[i];
				for (uint32_t k = 0; k < N; k++)
					am_off[k] = src.am_offset(k, f.am[i]);
			}
			for (uint32_t k = 0; k < N; k++)
			{
				p.add[k] = am_off[k] + ops[k].total_level;
				p.env[k] = batch_envelope(ops[k], p.add[k]);
			}
			p.from = i;
			p.to = end;
			batch_run(p, kernel);
		}

		// the fast envelopes went past the slow cut: resume their schedule
		for (uint32_t k = 0; k < N; k++)
			if (p.fast[k])
				next[k] = next_envelope_step(ops[k], f.clock, chips[end], limit);
		i = end;
	}

	for (uint32_t k = 0; k < N; k++)
		ops[k].phase = p.ph[k];
}

template void batch_render<2, kernel_2op>(batch_op *, batch_source const &, batch_frame const &, uint32_t, uint32_t, bool, kernel_2op &);
template void batch_render<4, kernel_rhythm>(batch_op *, batch_source const &, batch_frame const &, uint32_t, uint32_t, bool, kernel_rhythm &);
template void batch_render<4, kernel_4op>(batch_op *, batch_source const &, batch_frame const &, uint32_t, uint32_t, bool, kernel_4op &);

}
