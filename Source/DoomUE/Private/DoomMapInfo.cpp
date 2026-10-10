#include "DoomMapInfo.h"
#include "DoomInfo.h"
#include "DoomUE.h"

namespace
{
	struct FToken
	{
		enum EKind { End, Ident, String, Number, Symbol } Kind = End;
		FString Text;
	};

	/** UMAPINFO tokens: identifiers, "strings", numbers, { } = , with // and block comments. */
	class FLexer
	{
	public:
		explicit FLexer(const FString& InText) : S(InText) {}

		FToken Next()
		{
			SkipSpace();
			FToken T;
			if (P >= S.Len())
			{
				return T;
			}
			const TCHAR C = S[P];
			if (C == '"')
			{
				T.Kind = FToken::String;
				++P;
				while (P < S.Len() && S[P] != '"')
				{
					if (S[P] == '\\' && P + 1 < S.Len()) ++P;
					T.Text.AppendChar(S[P++]);
				}
				++P;
				return T;
			}
			if (C == '{' || C == '}' || C == '=' || C == ',')
			{
				T.Kind = FToken::Symbol;
				T.Text.AppendChar(C);
				++P;
				return T;
			}
			T.Kind = (FChar::IsDigit(C) || C == '-') ? FToken::Number : FToken::Ident;
			while (P < S.Len() && !FChar::IsWhitespace(S[P]) && S[P] != '{' && S[P] != '}' && S[P] != '=' && S[P] != ',' && S[P] != '"')
			{
				T.Text.AppendChar(S[P++]);
			}
			return T;
		}

	private:
		void SkipSpace()
		{
			while (P < S.Len())
			{
				if (FChar::IsWhitespace(S[P])) { ++P; continue; }
				if (S[P] == '/' && P + 1 < S.Len() && S[P + 1] == '/')
				{
					while (P < S.Len() && S[P] != '\n') ++P;
					continue;
				}
				if (S[P] == '/' && P + 1 < S.Len() && S[P + 1] == '*')
				{
					const int32 End = S.Find(TEXT("*/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, P + 2);
					P = End == INDEX_NONE ? S.Len() : End + 2;
					continue;
				}
				break;
			}
		}

		const FString& S;
		int32 P = 0;
	};
}

int32 FDoomMapInfoSet::Parse(const FString& Text)
{
	FLexer Lex(Text);
	int32 Count = 0;
	for (FToken T = Lex.Next(); T.Kind != FToken::End; T = Lex.Next())
	{
		if (T.Kind != FToken::Ident || !T.Text.Equals(TEXT("map"), ESearchCase::IgnoreCase))
		{
			continue;
		}
		FDoomMapInfo Info;
		Info.Map = Lex.Next().Text.ToUpper();
		if (Lex.Next().Text != TEXT("{"))
		{
			break;
		}
		// key = value[, value...] until "}"
		FToken K = Lex.Next();
		while (K.Kind != FToken::End && K.Text != TEXT("}"))
		{
			const FString Key = K.Text.ToLower();
			TArray<FToken> Values;
			FToken Eq = Lex.Next();
			FToken V = Lex.Next();
			if (Eq.Text == TEXT("="))
			{
				Values.Add(V);
				FToken Sep = Lex.Next();
				while (Sep.Text == TEXT(","))
				{
					Values.Add(Lex.Next());
					Sep = Lex.Next();
				}
				K = Sep; // the token after the values starts the next key (or is "}")
			}
			else
			{
				K = V;
			}
			if (Values.Num() == 0)
			{
				continue;
			}
			const FString& First = Values[0].Text;
			const bool bClear = Values[0].Kind == FToken::Ident && First.Equals(TEXT("clear"), ESearchCase::IgnoreCase);
			const bool bTrue = First.Equals(TEXT("true"), ESearchCase::IgnoreCase);
			if (Key == TEXT("levelname")) Info.LevelName = First;
			else if (Key == TEXT("label")) { if (bClear) Info.bClearLabel = true; else Info.Label = First; }
			else if (Key == TEXT("author")) Info.Author = First;
			else if (Key == TEXT("music")) Info.Music = First.ToUpper();
			else if (Key == TEXT("skytexture")) Info.Sky = First.ToUpper();
			else if (Key == TEXT("next")) Info.Next = First.ToUpper();
			else if (Key == TEXT("nextsecret")) Info.NextSecret = First.ToUpper();
			else if (Key == TEXT("partime")) Info.ParTime = FCString::Atoi(*First);
			else if (Key == TEXT("endgame")) Info.bEndGame |= bTrue;
			else if (Key == TEXT("endbunny")) { Info.bEndBunny = bTrue; Info.bEndGame |= bTrue; }
			else if (Key == TEXT("endcast")) { Info.bEndCast = bTrue; Info.bEndGame |= bTrue; }
			else if (Key == TEXT("endpic")) { Info.EndPic = First.ToUpper(); Info.bEndGame = true; }
			else if (Key == TEXT("nointermission")) Info.bNoIntermission = bTrue;
			else if (Key == TEXT("interbackdrop")) Info.InterBackdrop = First.ToUpper();
			else if (Key == TEXT("intermusic")) Info.InterMusic = First.ToUpper();
			else if (Key == TEXT("intertext") || Key == TEXT("intertextsecret"))
			{
				TArray<FString>& Lines = Key == TEXT("intertext") ? Info.InterText : Info.InterTextSecret;
				if (bClear) Info.bClearInterText = true;
				else for (const FToken& L : Values) Lines.Add(L.Text);
			}
			else if (Key == TEXT("bossaction") || Key == TEXT("bossactionednum"))
			{
				if (bClear)
				{
					Info.bClearBossActions = true;
					Info.BossActions.Reset();
				}
				else if (Values.Num() >= 3)
				{
					FDoomBossAction A;
					if (Key == TEXT("bossactionednum"))
					{
						A.DoomEdNum = FCString::Atoi(*First);
					}
					else if (const FDoomThingDef* Def = DoomInfo::FindThingByName(First))
					{
						A.DoomEdNum = Def->DoomEdNum;
					}
					A.Special = FCString::Atoi(*Values[1].Text);
					A.Tag = FCString::Atoi(*Values[2].Text);
					if (A.DoomEdNum)
					{
						Info.BossActions.Add(A);
						Info.bClearBossActions = true; // a map's own boss actions replace the vanilla ones
					}
					else
					{
						UE_LOG(LogDoom, Warning, TEXT("UMAPINFO %s: unknown bossaction thing %s"), *Info.Map, *First);
					}
				}
			}
		}
		if (Info.ParTime >= 0)
		{
			DoomInfo::SetParTime(Info.Map, Info.ParTime);
		}
		Maps.Add(Info.Map, MoveTemp(Info));
		++Count;
	}
	return Count;
}
