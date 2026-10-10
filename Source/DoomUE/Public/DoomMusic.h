// Doom music: MUS (and MIDI) lumps sequenced in real time and rendered by an
// OPL-style 2-operator FM synthesizer programmed with the WAD's GENMIDI bank
// (the AdLib / Sound Blaster sound of the original), streamed to Unreal's audio
// mixer by a procedural sound wave generating on the audio render thread.
#pragma once

#include "CoreMinimal.h"
#include "Sound/SoundWaveProcedural.h"
#include "DoomMusic.generated.h"

class FDoomWad;

/** One GENMIDI instrument: up to two 2-operator voices. */
struct FOplVoicePatch
{
	uint8 ModChar = 0, ModAttack = 0, ModSustain = 0, ModWave = 0, ModKsl = 0, ModLevel = 0;
	uint8 Feedback = 0;
	uint8 CarChar = 0, CarAttack = 0, CarSustain = 0, CarWave = 0, CarKsl = 0, CarLevel = 0;
	int16 NoteOffset = 0;
};

struct FOplInstrument
{
	uint16 Flags = 0;       // 1 fixed pitch, 4 double voice
	uint8 FineTune = 128;
	uint8 FixedNote = 0;
	FOplVoicePatch Voice[2];
};

/** A song as time-stamped MIDI-like events (seconds), from MUS or standard MIDI. */
struct FDoomSong
{
	enum class EType : uint8 { NoteOn, NoteOff, PitchBend, Controller, Program, AllNotesOff };
	struct FEvent
	{
		double Time;
		EType Type;
		uint8 Channel;   // 0..15, 9 = percussion
		uint8 A;         // note / controller / program
		int16 B;         // velocity / value / bend (-8192..8191)
	};
	TArray<FEvent> Events;
	double Length = 0;

	bool LoadMus(TConstArrayView<uint8> Data);
	bool LoadMidi(TConstArrayView<uint8> Data);
};

/** The synthesizer and sequencer; runs on the audio render thread. */
class DOOMUE_API FDoomMusicPlayer
{
public:
	static constexpr int32 SampleRate = 44100;
	static constexpr int32 NumVoices = 18; // an OPL3's 18 two-operator channels

	FDoomMusicPlayer(const TArray<FOplInstrument>& InBank, FDoomSong&& InSong, bool bInLoop);

	/** Renders interleaved stereo int16 frames. Returns false when a non-looping song ended. */
	bool Render(int16* Out, int32 Frames);

	TAtomic<float> Volume { 0.7f };
	int32 NotesPlayed = 0;

private:
	struct FOperator
	{
		double Phase = 0;
		double Env = 96;     // attenuation in dB (96 = silent)
		int32 Stage = 0;     // 0 off, 1 attack, 2 decay, 3 sustain, 4 release
		double Out = 0, PrevOut = 0;
	};
	struct FVoice
	{
		bool bActive = false;
		bool bReleased = false;
		int32 Channel = 0;
		int32 Note = 0;          // MIDI note as played on the channel (for note off)
		double BaseNote = 0;     // pitch before bend
		int32 Velocity = 0;
		const FOplVoicePatch* Patch = nullptr;
		FOperator Mod, Car;
		uint64 Age = 0;
		double CachedNote = -1000, CachedFreq = 0;
	};
	struct FChannel
	{
		int32 Program = 0;
		int32 Volume = 100;
		int32 Expression = 127;
		int32 Pan = 64;
		double Bend = 0;      // semitones
	};

	void Dispatch(const FDoomSong::FEvent& E);
	void NoteOn(int32 Channel, int32 Note, int32 Velocity);
	void NoteOff(int32 Channel, int32 Note);
	void StartVoice(FVoice& V, int32 Channel, int32 Note, int32 Velocity, const FOplVoicePatch& P, double Pitch);
	FVoice& AllocVoice();
	void AllNotesOff(int32 Channel);
	double StepEnvelope(FOperator& Op, uint8 Attack, uint8 Sustain, uint8 Char, double Dt) const;
	static double Wave(int32 Form, double Phase);

	TArray<FOplInstrument> Bank;
	FDoomSong Song;
	bool bLoop = true;
	int32 NextEvent = 0;
	double Time = 0;
	double LfoTime = 0;
	FVoice Voices[NumVoices];
	FChannel Channels[16];
	uint64 AgeCounter = 0;
};

/** Streams FDoomMusicPlayer output into the audio mixer. */
UCLASS()
class DOOMUE_API UDoomMusicWave : public USoundWaveProcedural
{
	GENERATED_BODY()

public:
	UDoomMusicWave(const FObjectInitializer& ObjectInitializer);

	void Start(TSharedPtr<FDoomMusicPlayer> InPlayer);
	TSharedPtr<FDoomMusicPlayer> GetPlayer() const { return Player; }

	virtual int32 OnGeneratePCMAudio(TArray<uint8>& OutAudio, int32 NumSamples) override;
	virtual Audio::EAudioMixerStreamDataFormat::Type GetGeneratedPCMDataFormat() const override { return Audio::EAudioMixerStreamDataFormat::Int16; }

private:
	TSharedPtr<FDoomMusicPlayer> Player;
};

namespace DoomMusic
{
	/** GENMIDI lump -> 175 instruments (128 melodic + 47 percussion). Empty if missing. */
	DOOMUE_API TArray<FOplInstrument> LoadBank(const FDoomWad& Wad);
	/** Builds a player for a music lump (D_E1M1, D_RUNNIN...). Null if missing or unreadable. */
	DOOMUE_API TSharedPtr<FDoomMusicPlayer> CreatePlayer(const FDoomWad& Wad, const FString& Lump, bool bLoop);
	/** Music lump for a map: D_ExMy for Doom 1, the MAPxx table for Doom 2. */
	DOOMUE_API FString LumpForMap(const FString& MapName);
	/** Renders Seconds of a song to a 16-bit stereo WAV file (testing / -dumpmusic=). */
	DOOMUE_API bool RenderToWav(const FDoomWad& Wad, const FString& Lump, float Seconds, const FString& Path, FString& OutStats);
}
