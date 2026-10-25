#include "DoomMusic.h"
#include "DoomWad.h"
#include "DoomUE.h"
#include "Misc/FileHelper.h"

namespace
{
	using namespace WadRead;

	// ---- tables ------------------------------------------------------------------------------------
	// OPL frequency multipliers (register 0x20 bits 0-3)
	const double Multipliers[16] = { 0.5, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 10, 12, 12, 15, 15 };
	// key scale level: dB per octave for KSL bits 00, 01, 10, 11
	const double KslPerOctave[4] = { 0.0, 3.0, 1.5, 6.0 };

	// Envelope timing modelled on the OPL's rates: each rate step halves the time.
	double AttackSeconds(int32 Rate) { return Rate == 0 ? 1e9 : Rate >= 15 ? 0.0 : 2.826 / double(int64(1) << (Rate - 1)); }
	double DecaySeconds(int32 Rate) { return Rate == 0 ? 1e9 : 39.28 / double(int64(1) << (Rate - 1)); } // time for 96 dB

	constexpr int32 SineSize = 4096;
	struct FSine
	{
		float T[SineSize];
		FSine() { for (int32 i = 0; i < SineSize; ++i) T[i] = (float)FMath::Sin(2.0 * PI * i / SineSize); }
	};
	const FSine GSine;

	inline double Sine01(double Phase) // phase in cycles
	{
		const double F = Phase - FMath::FloorToDouble(Phase);
		return GSine.T[(int32)(F * SineSize) & (SineSize - 1)];
	}

	// ---- variable length helpers --------------------------------------------------------------------
	struct FReader
	{
		TConstArrayView<uint8> D;
		int32 Pos = 0;
		bool Ok() const { return Pos < D.Num(); }
		uint8 U8() { return Pos < D.Num() ? D[Pos++] : 0; }
		uint32 VarLen()
		{
			uint32 V = 0;
			for (int32 i = 0; i < 4 && Ok(); ++i)
			{
				const uint8 B = U8();
				V = (V << 7) | (B & 0x7F);
				if (!(B & 0x80)) break;
			}
			return V;
		}
		uint32 BE32() { uint32 V = 0; for (int32 i = 0; i < 4; ++i) V = (V << 8) | U8(); return V; }
		uint16 BE16() { uint16 V = U8(); return (uint16)((V << 8) | U8()); }
	};
}

// ================================================================================================
// MUS / MIDI parsing
// ================================================================================================

bool FDoomSong::LoadMus(TConstArrayView<uint8> Data)
{
	if (Data.Num() < 16 || FMemory::Memcmp(Data.GetData(), "MUS\x1A", 4) != 0)
	{
		return false;
	}
	const int32 ScoreLen = U16(&Data[4]);
	const int32 ScoreStart = U16(&Data[6]);
	FReader R{ Data.Slice(0, FMath::Min(Data.Num(), ScoreStart + ScoreLen)), ScoreStart };
	// MUS channel 15 is percussion; map it to MIDI channel 9 (and 9 to 15)
	auto MapChannel = [](int32 C) { return C == 15 ? 9 : C == 9 ? 15 : C; };
	int32 LastVelocity[16];
	for (int32& V : LastVelocity) V = 127;
	double Tick = 0;
	constexpr double TickSeconds = 1.0 / 140.0;
	// MUS controller numbers -> MIDI controllers (0 = program change, handled separately)
	const uint8 CtrlMap[10] = { 0, 0, 1, 7, 10, 11, 91, 93, 64, 67 };

	while (R.Ok())
	{
		const uint8 Desc = R.U8();
		const int32 Type = (Desc >> 4) & 7;
		const int32 Ch = MapChannel(Desc & 15);
		const double T = Tick * TickSeconds;
		bool bEnd = false;
		switch (Type)
		{
		case 0: // release
			Events.Add({ T, EType::NoteOff, (uint8)Ch, (uint8)(R.U8() & 127), 0 });
			break;
		case 1: // play
		{
			const uint8 N = R.U8();
			if (N & 0x80) LastVelocity[Ch] = R.U8() & 127;
			Events.Add({ T, EType::NoteOn, (uint8)Ch, (uint8)(N & 127), (int16)LastVelocity[Ch] });
			break;
		}
		case 2: // pitch bend: 0..255, 128 = centre, full range = +-2 semitones
			Events.Add({ T, EType::PitchBend, (uint8)Ch, 0, (int16)((int32(R.U8()) - 128) * 64) });
			break;
		case 3: // system event
		{
			const uint8 S = R.U8();
			if (S == 10 || S == 11) Events.Add({ T, EType::AllNotesOff, (uint8)Ch, 0, 0 });
			break;
		}
		case 4: // controller
		{
			const uint8 C = R.U8();
			const uint8 V = R.U8();
			if (C == 0) Events.Add({ T, EType::Program, (uint8)Ch, (uint8)(V & 127), 0 });
			else if (C < 10) Events.Add({ T, EType::Controller, (uint8)Ch, CtrlMap[C], (int16)FMath::Min<int32>(V, 127) });
			break;
		}
		case 5: break; // end of measure
		case 6: bEnd = true; break;
		default: break;
		}
		if (bEnd)
		{
			break;
		}
		if (Desc & 0x80)
		{
			Tick += R.VarLen();
		}
	}
	Length = Tick * TickSeconds;
	return Events.Num() > 0;
}

bool FDoomSong::LoadMidi(TConstArrayView<uint8> Data)
{
	if (Data.Num() < 14 || FMemory::Memcmp(Data.GetData(), "MThd", 4) != 0)
	{
		return false;
	}
	FReader H{ Data, 8 };
	H.BE16(); // format
	const int32 NumTracks = H.BE16();
	const int32 Division = H.BE16();
	if (Division & 0x8000)
	{
		return false; // SMPTE timing: not used by Doom WADs
	}
	struct FRaw { uint64 Tick; int32 Order; FEvent E; uint32 Tempo; bool bTempo; };
	TArray<FRaw> Raw;
	int32 Pos = 14;
	int32 Order = 0;
	for (int32 t = 0; t < NumTracks && Pos + 8 <= Data.Num(); ++t)
	{
		FReader TR{ Data, Pos };
		if (FMemory::Memcmp(&Data[Pos], "MTrk", 4) != 0) break;
		TR.Pos += 4;
		const int32 Len = TR.BE32();
		const int32 End = FMath::Min(Data.Num(), TR.Pos + Len);
		FReader R{ Data.Slice(0, End), TR.Pos };
		uint64 Tick = 0;
		uint8 Status = 0;
		while (R.Ok())
		{
			Tick += R.VarLen();
			uint8 B = R.U8();
			if (B & 0x80) Status = B; else --R.Pos;
			const uint8 Ch = Status & 15;
			FRaw Ev{ Tick, Order++, { 0, EType::NoteOn, Ch, 0, 0 }, 0, false };
			switch (Status & 0xF0)
			{
			case 0x80: Ev.E.Type = EType::NoteOff; Ev.E.A = R.U8() & 127; R.U8(); Raw.Add(Ev); break;
			case 0x90:
				Ev.E.A = R.U8() & 127; Ev.E.B = R.U8() & 127;
				if (Ev.E.B == 0) Ev.E.Type = EType::NoteOff;
				Raw.Add(Ev);
				break;
			case 0xA0: R.U8(); R.U8(); break;
			case 0xB0:
				Ev.E.Type = EType::Controller; Ev.E.A = R.U8() & 127; Ev.E.B = R.U8() & 127;
				if (Ev.E.A == 123 || Ev.E.A == 120) Ev.E.Type = EType::AllNotesOff;
				Raw.Add(Ev);
				break;
			case 0xC0: Ev.E.Type = EType::Program; Ev.E.A = R.U8() & 127; Raw.Add(Ev); break;
			case 0xD0: R.U8(); break;
			case 0xE0:
			{
				const int32 Lo = R.U8() & 127, Hi = R.U8() & 127;
				Ev.E.Type = EType::PitchBend; Ev.E.B = (int16)(((Hi << 7) | Lo) - 8192);
				Raw.Add(Ev);
				break;
			}
			case 0xF0:
				if (Status == 0xFF)
				{
					const uint8 Meta = R.U8();
					const uint32 L = R.VarLen();
					if (Meta == 0x51 && L == 3)
					{
						FRaw T{ Tick, Order++, {}, 0, true };
						T.Tempo = (R.U8() << 16) | (R.U8() << 8) | R.U8();
						Raw.Add(T);
					}
					else
					{
						R.Pos += L;
					}
					if (Meta == 0x2F) R.Pos = End;
				}
				else
				{
					R.Pos += R.VarLen(); // sysex
				}
				break;
			}
		}
		Pos = End;
	}
	Raw.StableSort([](const FRaw& X, const FRaw& Y) { return X.Tick != Y.Tick ? X.Tick < Y.Tick : X.Order < Y.Order; });
	double Seconds = 0;
	uint64 LastTick = 0;
	double SecondsPerTick = 0.5 / Division; // 120 bpm until told otherwise
	for (const FRaw& Ev : Raw)
	{
		Seconds += (Ev.Tick - LastTick) * SecondsPerTick;
		LastTick = Ev.Tick;
		if (Ev.bTempo)
		{
			SecondsPerTick = Ev.Tempo / 1e6 / Division;
			continue;
		}
		FEvent E = Ev.E;
		E.Time = Seconds;
		Events.Add(E);
	}
	Length = Seconds;
	return Events.Num() > 0;
}

// ================================================================================================
// Synthesizer
// ================================================================================================

FDoomMusicPlayer::FDoomMusicPlayer(const TArray<FOplInstrument>& InBank, FDoomSong&& InSong, bool bInLoop)
	: Bank(InBank), Song(MoveTemp(InSong)), bLoop(bInLoop)
{
	for (int32 c = 0; c < 16; ++c) Channels[c] = FChannel();
}

double FDoomMusicPlayer::Wave(int32 Form, double Phase)
{
	const double F = Phase - FMath::FloorToDouble(Phase);
	const double S = Sine01(F);
	switch (Form & 7)
	{
	case 0: return S;                                         // sine
	case 1: return F < 0.5 ? S : 0.0;                          // half sine
	case 2: return FMath::Abs(S);                              // absolute sine
	case 3: return (F < 0.25 || (F >= 0.5 && F < 0.75)) ? FMath::Abs(S) : 0.0; // quarter (pulse) sine
	case 4: return F < 0.5 ? Sine01(F * 2.0) : 0.0;            // alternating sine (OPL3)
	case 5: return F < 0.5 ? FMath::Abs(Sine01(F * 2.0)) : 0.0; // camel sine (OPL3)
	case 6: return F < 0.5 ? 1.0 : -1.0;                       // square (OPL3)
	default: return F < 0.5 ? 1.0 - F * 4.0 : -1.0 + (F - 0.5) * 4.0; // saw-like (OPL3 log-saw)
	}
}

double FDoomMusicPlayer::StepEnvelope(FOperator& Op, uint8 Attack, uint8 Sustain, uint8 Char, double Dt) const
{
	const int32 AR = Attack >> 4, DR = Attack & 15;
	const int32 SL = Sustain >> 4, RR = Sustain & 15;
	const double SustainDb = SL == 15 ? 93.0 : SL * 3.0;
	const bool bHold = (Char & 0x20) != 0; // EG type: sustain until key off
	switch (Op.Stage)
	{
	case 1: // attack: exponential approach towards 0 dB
	{
		const double T = AttackSeconds(AR);
		if (T <= 0.0)
		{
			Op.Env = 0;
		}
		else
		{
			Op.Env -= (Op.Env + 6.0) * Dt / (T * 0.35);
		}
		if (Op.Env <= 0.05) { Op.Env = 0; Op.Stage = 2; }
		break;
	}
	case 2: // decay to the sustain level
		Op.Env += 96.0 * Dt / DecaySeconds(DR);
		if (Op.Env >= SustainDb) { Op.Env = SustainDb; Op.Stage = 3; }
		break;
	case 3: // sustain (or keep decaying at the release rate for percussive sounds)
		if (!bHold) Op.Env += 96.0 * Dt / DecaySeconds(RR);
		break;
	case 4: // release
		Op.Env += 96.0 * Dt / DecaySeconds(RR);
		break;
	default:
		Op.Env = 96;
		break;
	}
	if (Op.Env >= 96.0)
	{
		Op.Env = 96.0;
		if (Op.Stage >= 3) Op.Stage = 0;
	}
	return Op.Env;
}

FDoomMusicPlayer::FVoice& FDoomMusicPlayer::AllocVoice()
{
	// free voice, else the oldest released one, else the oldest of all
	FVoice* Best = nullptr;
	for (FVoice& V : Voices) if (!V.bActive) return V;
	for (FVoice& V : Voices) if (V.bReleased && (!Best || V.Age < Best->Age)) Best = &V;
	if (Best) return *Best;
	for (FVoice& V : Voices) if (!Best || V.Age < Best->Age) Best = &V;
	return *Best;
}

void FDoomMusicPlayer::StartVoice(FVoice& V, int32 Channel, int32 Note, int32 Velocity, const FOplVoicePatch& P, double Pitch)
{
	V = FVoice();
	V.bActive = true;
	V.Channel = Channel;
	V.Note = Note;
	V.BaseNote = Pitch + P.NoteOffset;
	V.Velocity = Velocity;
	V.Patch = &P;
	V.Mod.Stage = V.Car.Stage = 1;
	V.Mod.Env = V.Car.Env = 96;
	V.Age = ++AgeCounter;
}

void FDoomMusicPlayer::NoteOn(int32 Channel, int32 Note, int32 Velocity)
{
	if (Velocity <= 0)
	{
		NoteOff(Channel, Note);
		return;
	}
	const FOplInstrument* Ins = nullptr;
	if (Channel == 9)
	{
		if (Note < 35 || Note > 81) return;
		const int32 Index = 128 + Note - 35;
		if (!Bank.IsValidIndex(Index)) return;
		Ins = &Bank[Index];
	}
	else
	{
		if (!Bank.IsValidIndex(Channels[Channel].Program)) return;
		Ins = &Bank[Channels[Channel].Program];
	}
	const double Pitch = (Ins->Flags & 1) ? Ins->FixedNote : Note;
	++NotesPlayed;
	StartVoice(AllocVoice(), Channel, Note, Velocity, Ins->Voice[0], Pitch);
	if (Ins->Flags & 4)
	{
		// double-voice instrument: second voice slightly detuned
		const double Detune = (int32(Ins->FineTune) - 128) / 64.0;
		StartVoice(AllocVoice(), Channel, Note, Velocity, Ins->Voice[1], Pitch + Detune);
	}
}

void FDoomMusicPlayer::NoteOff(int32 Channel, int32 Note)
{
	for (FVoice& V : Voices)
	{
		if (V.bActive && !V.bReleased && V.Channel == Channel && V.Note == Note)
		{
			V.bReleased = true;
			V.Mod.Stage = V.Car.Stage = 4;
		}
	}
}

void FDoomMusicPlayer::AllNotesOff(int32 Channel)
{
	for (FVoice& V : Voices)
	{
		if (V.bActive && (Channel < 0 || V.Channel == Channel))
		{
			V.bReleased = true;
			V.Mod.Stage = V.Car.Stage = 4;
		}
	}
}

void FDoomMusicPlayer::Dispatch(const FDoomSong::FEvent& E)
{
	FChannel& C = Channels[E.Channel & 15];
	switch (E.Type)
	{
	case FDoomSong::EType::NoteOn: NoteOn(E.Channel, E.A, E.B); break;
	case FDoomSong::EType::NoteOff: NoteOff(E.Channel, E.A); break;
	case FDoomSong::EType::PitchBend: C.Bend = E.B / 8192.0 * 2.0; break;
	case FDoomSong::EType::Program: C.Program = E.A; break;
	case FDoomSong::EType::AllNotesOff: AllNotesOff(E.Channel); break;
	case FDoomSong::EType::Controller:
		switch (E.A)
		{
		case 7: C.Volume = E.B; break;
		case 10: C.Pan = E.B; break;
		case 11: C.Expression = E.B; break;
		case 121: C.Volume = 100; C.Expression = 127; C.Pan = 64; C.Bend = 0; break;
		default: break;
		}
		break;
	}
}

bool FDoomMusicPlayer::Render(int16* Out, int32 Frames)
{
	const double Dt = 1.0 / SampleRate;
	const float Gain = Volume.Load();
	for (int32 f = 0; f < Frames; ++f)
	{
		// sequencer
		while (NextEvent < Song.Events.Num() && Song.Events[NextEvent].Time <= Time)
		{
			Dispatch(Song.Events[NextEvent++]);
		}
		if (NextEvent >= Song.Events.Num() && Time >= Song.Length)
		{
			if (!bLoop)
			{
				FMemory::Memzero(Out + f * 2, (Frames - f) * 2 * sizeof(int16));
				return false;
			}
			AllNotesOff(-1);
			NextEvent = 0;
			Time = 0;
			for (FChannel& C : Channels) C.Bend = 0;
		}
		Time += Dt;
		LfoTime += Dt;

		// LFOs: OPL tremolo ~3.7 Hz (1 dB), vibrato ~6.1 Hz (7 cents)
		const double TremoloDb = (1.0 + Sine01(LfoTime * 3.7)) * 0.5;
		const double VibratoMul = 1.0 + Sine01(LfoTime * 6.1) * 0.07 * 0.0577623; // 7 cents, linearized

		double L = 0, R = 0;
		for (FVoice& V : Voices)
		{
			if (!V.bActive)
			{
				continue;
			}
			const FOplVoicePatch& P = *V.Patch;
			const FChannel& C = Channels[V.Channel];
			const double Note = V.BaseNote + C.Bend;
			if (Note != V.CachedNote)
			{
				V.CachedNote = Note;
				V.CachedFreq = 440.0 * FMath::Pow(2.0, (Note - 69.0) / 12.0);
			}
			const double Freq = V.CachedFreq;
			const double Octave = FMath::Max(0.0, (Note - 48.0) / 12.0);

			auto Operator = [&](FOperator& Op, uint8 Char, uint8 Attack, uint8 Sustain, uint8 WaveForm, uint8 Ksl, uint8 Level, double PhaseMod) -> double
			{
				const double OpFreq = Freq * Multipliers[Char & 15] * ((Char & 0x40) ? VibratoMul : 1.0);
				Op.Phase += OpFreq * Dt;
				if (Op.Phase > 1e6) Op.Phase -= FMath::FloorToDouble(Op.Phase);
				const double Env = StepEnvelope(Op, Attack, Sustain, Char, Dt);
				double Db = Env + (Level & 63) * 0.75 + KslPerOctave[(Ksl >> 6) & 3] * Octave;
				if (Char & 0x80) Db += TremoloDb;
				if (Db >= 96.0) return 0.0;
				return Wave(WaveForm, Op.Phase + PhaseMod) * FMath::Exp(-Db * 0.11512925); // 10^(-dB/20)
			};

			const int32 Fb = (P.Feedback >> 1) & 7;
			const double FbMod = Fb ? (V.Mod.Out + V.Mod.PrevOut) * double(int64(1) << Fb) / 128.0 : 0.0;
			const double ModOut = Operator(V.Mod, P.ModChar, P.ModAttack, P.ModSustain, P.ModWave, P.ModKsl, P.ModLevel, FbMod);
			V.Mod.PrevOut = V.Mod.Out;
			V.Mod.Out = ModOut;
			const bool bAdditive = (P.Feedback & 1) != 0;
			const double CarOut = Operator(V.Car, P.CarChar, P.CarAttack, P.CarSustain, P.CarWave, P.CarKsl, P.CarLevel, bAdditive ? 0.0 : ModOut * 4.0);
			double S = bAdditive ? (ModOut + CarOut) * 0.5 : CarOut;

			if (V.Car.Stage == 0 && (bAdditive ? V.Mod.Stage == 0 : true))
			{
				V.bActive = false;
				continue;
			}
			// MIDI velocity, channel volume and expression (squared: perceived loudness)
			const double Amp = (V.Velocity / 127.0) * (C.Volume / 127.0) * (C.Expression / 127.0);
			S *= Amp * Amp * 0.9 + Amp * 0.1;
			const double Pan = FMath::Clamp((C.Pan - 64) / 63.0, -1.0, 1.0);
			L += S * FMath::Cos((Pan + 1.0) * PI * 0.25);
			R += S * FMath::Sin((Pan + 1.0) * PI * 0.25);
		}
		// soft clip the mix
		const double OL = FMath::Tanh(L * 0.35 * Gain);
		const double OR = FMath::Tanh(R * 0.35 * Gain);
		Out[f * 2] = (int16)FMath::Clamp(OL * 32000.0, -32767.0, 32767.0);
		Out[f * 2 + 1] = (int16)FMath::Clamp(OR * 32000.0, -32767.0, 32767.0);
	}
	return true;
}

// ================================================================================================
// Unreal glue
// ================================================================================================

UDoomMusicWave::UDoomMusicWave(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SetSampleRate(FDoomMusicPlayer::SampleRate);
	NumChannels = 2;
	Duration = INDEFINITELY_LOOPING_DURATION;
	bLooping = false;
	SoundGroup = SOUNDGROUP_Music;
	bCanProcessAsync = true;
}

void UDoomMusicWave::Start(TSharedPtr<FDoomMusicPlayer> InPlayer)
{
	Player = InPlayer;
}

int32 UDoomMusicWave::OnGeneratePCMAudio(TArray<uint8>& OutAudio, int32 NumSamples)
{
	const int32 Frames = NumSamples / 2;
	OutAudio.SetNumZeroed(Frames * 2 * sizeof(int16));
	if (Player.IsValid())
	{
		Player->Render(reinterpret_cast<int16*>(OutAudio.GetData()), Frames);
	}
	return Frames * 2;
}

TArray<FOplInstrument> DoomMusic::LoadBank(const FDoomWad& Wad)
{
	TArray<FOplInstrument> Bank;
	TConstArrayView<uint8> D = Wad.LumpData(TEXT("GENMIDI"));
	if (D.Num() < 8 + 175 * 36 || FMemory::Memcmp(D.GetData(), "#OPL_II#", 8) != 0)
	{
		UE_LOG(LogDoomAudio, Warning, TEXT("No GENMIDI bank: music disabled"));
		return Bank;
	}
	Bank.SetNum(175);
	for (int32 i = 0; i < 175; ++i)
	{
		const uint8* P = &D[8 + i * 36];
		FOplInstrument& I = Bank[i];
		I.Flags = U16(P);
		I.FineTune = P[2];
		I.FixedNote = P[3];
		for (int32 v = 0; v < 2; ++v)
		{
			const uint8* Q = P + 4 + v * 16;
			FOplVoicePatch& Vp = I.Voice[v];
			Vp.ModChar = Q[0]; Vp.ModAttack = Q[1]; Vp.ModSustain = Q[2]; Vp.ModWave = Q[3]; Vp.ModKsl = Q[4]; Vp.ModLevel = Q[5];
			Vp.Feedback = Q[6];
			Vp.CarChar = Q[7]; Vp.CarAttack = Q[8]; Vp.CarSustain = Q[9]; Vp.CarWave = Q[10]; Vp.CarKsl = Q[11]; Vp.CarLevel = Q[12];
			Vp.NoteOffset = S16(Q + 14);
		}
	}
	return Bank;
}

TSharedPtr<FDoomMusicPlayer> DoomMusic::CreatePlayer(const FDoomWad& Wad, const FString& Lump, bool bLoop)
{
	static TMap<const FDoomWad*, TArray<FOplInstrument>> Banks;
	TArray<FOplInstrument>* Bank = Banks.Find(&Wad);
	if (!Bank)
	{
		Bank = &Banks.Add(&Wad, LoadBank(Wad));
	}
	if (Bank->Num() == 0)
	{
		return nullptr;
	}
	TConstArrayView<uint8> D = Wad.LumpData(Lump);
	FDoomSong Song;
	if (!Song.LoadMus(D) && !Song.LoadMidi(D))
	{
		UE_LOG(LogDoomAudio, Warning, TEXT("Music %s: missing or unreadable"), *Lump);
		return nullptr;
	}
	UE_LOG(LogDoomAudio, Log, TEXT("Music %s: %d events, %.1f s"), *Lump, Song.Events.Num(), Song.Length);
	return MakeShared<FDoomMusicPlayer>(*Bank, MoveTemp(Song), bLoop);
}

FString DoomMusic::LumpForMap(const FString& MapName)
{
	static const TCHAR* Doom2[] = {
		TEXT("RUNNIN"), TEXT("STALKS"), TEXT("COUNTD"), TEXT("BETWEE"), TEXT("DOOM"), TEXT("THE_DA"), TEXT("SHAWN"), TEXT("DDTBLU"),
		TEXT("IN_CIT"), TEXT("DEAD"), TEXT("STLKS2"), TEXT("THEDA2"), TEXT("DOOM2"), TEXT("DDTBL2"), TEXT("RUNNI2"), TEXT("DEAD2"),
		TEXT("STLKS3"), TEXT("ROMERO"), TEXT("SHAWN2"), TEXT("MESSAG"), TEXT("COUNT2"), TEXT("DDTBL3"), TEXT("AMPIE"), TEXT("THEDA3"),
		TEXT("ADRIAN"), TEXT("MESSG2"), TEXT("ROMER2"), TEXT("TENSE"), TEXT("SHAWN3"), TEXT("OPENIN"), TEXT("EVIL"), TEXT("ULTIMA") };
	if (MapName.StartsWith(TEXT("MAP")))
	{
		const int32 N = FCString::Atoi(*MapName.Mid(3));
		return (N >= 1 && N <= 32) ? FString(TEXT("D_")) + Doom2[N - 1] : FString();
	}
	return TEXT("D_") + MapName;
}

bool DoomMusic::RenderToWav(const FDoomWad& Wad, const FString& Lump, float Seconds, const FString& Path, FString& OutStats)
{
	TSharedPtr<FDoomMusicPlayer> Player = CreatePlayer(Wad, Lump, false);
	if (!Player)
	{
		return false;
	}
	const int32 Frames = (int32)(Seconds * FDoomMusicPlayer::SampleRate);
	TArray<int16> Pcm;
	Pcm.SetNumZeroed(Frames * 2);
	for (int32 f = 0; f < Frames; f += 1024)
	{
		Player->Render(Pcm.GetData() + f * 2, FMath::Min(1024, Frames - f));
	}
	double Sum = 0;
	int32 Peak = 0;
	for (int16 S : Pcm)
	{
		Sum += double(S) * S;
		Peak = FMath::Max(Peak, FMath::Abs<int32>(S));
	}
	const double Rms = FMath::Sqrt(Sum / FMath::Max(1, Pcm.Num()));
	OutStats = FString::Printf(TEXT("%s: %.1f s, %d notes, RMS %.0f, peak %d"), *Lump, Seconds, Player->NotesPlayed, Rms, Peak);

	TArray<uint8> Wav;
	auto Put32 = [&](uint32 V) { Wav.Append(reinterpret_cast<uint8*>(&V), 4); };
	auto Put16 = [&](uint16 V) { Wav.Append(reinterpret_cast<uint8*>(&V), 2); };
	const uint32 DataBytes = Pcm.Num() * 2;
	Wav.Append((const uint8*)"RIFF", 4); Put32(36 + DataBytes); Wav.Append((const uint8*)"WAVEfmt ", 8);
	Put32(16); Put16(1); Put16(2); Put32(FDoomMusicPlayer::SampleRate); Put32(FDoomMusicPlayer::SampleRate * 4); Put16(4); Put16(16);
	Wav.Append((const uint8*)"data", 4); Put32(DataBytes);
	Wav.Append(reinterpret_cast<const uint8*>(Pcm.GetData()), DataBytes);
	return FFileHelper::SaveArrayToFile(Wav, *Path);
}
