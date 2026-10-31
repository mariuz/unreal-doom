#include "DoomInfo.h"
#include "DoomGameData.h"
#include "DoomSettings.h"
#include "DoomUE.h"

// the built-in tables (a named namespace: in a unity build an anonymous one would put Monster,
// Item, Fire, White and EThingFlag's names in every other file of the same group)
namespace DoomInfoBuiltIn
{
	using namespace EThingFlag;

	FDoomThingDef Monster(int32 Num, const TCHAR* Name, const TCHAR* Sprite,
		const TCHAR* Idle, const TCHAR* Run, const TCHAR* Melee, const TCHAR* Attack, const TCHAR* Pain, const TCHAR* Death, const TCHAR* XDeath,
		float Radius, float Height, int32 Health, float Speed, int32 RunTics, int32 PainChance, uint32 Flags,
		EDoomAttack AttackKind, const TCHAR* Projectile, int32 MeleeDamage,
		const TCHAR* See, const TCHAR* AttackSound, const TCHAR* PainSound, const TCHAR* DeathSound, const TCHAR* Active, int32 Drop = 0)
	{
		FDoomThingDef D;
		D.DoomEdNum = Num; D.Name = Name; D.Sprite = Sprite;
		D.Idle = Idle; D.Run = Run; D.Melee = Melee; D.Attack = Attack; D.Pain = Pain; D.Death = Death; D.XDeath = XDeath;
		D.Radius = Radius; D.Height = Height; D.Health = Health; D.Speed = Speed; D.RunTics = RunTics; D.PainChance = PainChance;
		D.Flags = Flags | EThingFlag::Solid | EThingFlag::Shootable | EThingFlag::Monster | EThingFlag::CountKill;
		D.Attack_ = AttackKind; D.Projectile = Projectile; D.MeleeDamage = MeleeDamage;
		D.SeeSound = See; D.AttackSound = AttackSound; D.PainSound = PainSound; D.DeathSound = DeathSound; D.ActiveSound = Active;
		D.DropType = Drop;
		D.IdleTics = 10;
		return D;
	}

	FDoomThingDef Item(int32 Num, const TCHAR* Name, const TCHAR* Sprite, const TCHAR* Frames, EPickup Pickup, uint32 Flags = CountItem, int32 Tics = 6)
	{
		FDoomThingDef D;
		D.DoomEdNum = Num; D.Name = Name; D.Sprite = Sprite; D.Idle = Frames;
		D.Radius = 20; D.Height = 16; D.Pickup = Pickup; D.Flags = Flags | EThingFlag::Pickup; D.IdleTics = Tics;
		return D;
	}

	FDoomThingDef Decor(int32 Num, const TCHAR* Name, const TCHAR* Sprite, const TCHAR* Frames, float Radius, float Height, uint32 Flags,
		FLinearColor Light = FLinearColor::Transparent, int32 Tics = 6)
	{
		FDoomThingDef D;
		D.DoomEdNum = Num; D.Name = Name; D.Sprite = Sprite; D.Idle = Frames;
		D.Radius = Radius; D.Height = Height; D.Flags = Flags; D.LightColor = Light; D.IdleTics = Tics;
		D.bFullBrightIdle = Light.A > 0;
		return D;
	}

	const FLinearColor Fire(1.0f, 0.55f, 0.2f, 1.f);
	const FLinearColor BlueFire(0.3f, 0.45f, 1.0f, 1.f);
	const FLinearColor GreenFire(0.3f, 1.0f, 0.35f, 1.f);
	const FLinearColor RedFire(1.0f, 0.2f, 0.15f, 1.f);
	const FLinearColor White(1.0f, 0.95f, 0.85f, 1.f);

	TArray<FDoomThingDef> BuildThings()
	{
		using EA = EDoomAttack;
		TArray<FDoomThingDef> T;
		// --- monsters ---------------------------------------------------------------------------------------------------
		T.Add(Monster(3004, TEXT("Zombieman"), TEXT("POSS"), TEXT("AB"), TEXT("ABCD"), TEXT(""), TEXT("EF"), TEXT("G"), TEXT("HIJKL"), TEXT("MNOPQRSTU"),
			20, 56, 20, 8, 4, 200, 0, EA::Hitscan, TEXT(""), 0, TEXT("POSIT1"), TEXT("PISTOL"), TEXT("POPAIN"), TEXT("PODTH1"), TEXT("POSACT"), 2007));
		T.Add(Monster(9, TEXT("ShotgunGuy"), TEXT("SPOS"), TEXT("AB"), TEXT("ABCD"), TEXT(""), TEXT("EF"), TEXT("G"), TEXT("HIJKL"), TEXT("MNOPQRSTU"),
			20, 56, 30, 8, 3, 170, 0, EA::Shotgun, TEXT(""), 0, TEXT("POSIT2"), TEXT("SHOTGN"), TEXT("POPAIN"), TEXT("PODTH2"), TEXT("POSACT"), 2001));
		T.Add(Monster(65, TEXT("Chaingunner"), TEXT("CPOS"), TEXT("AB"), TEXT("ABCD"), TEXT(""), TEXT("EFEF"), TEXT("G"), TEXT("HIJKLMN"), TEXT("OPQRST"),
			20, 56, 70, 8, 3, 170, 0, EA::Chaingun, TEXT(""), 0, TEXT("POSIT2"), TEXT("SHOTGN"), TEXT("POPAIN"), TEXT("PODTH2"), TEXT("POSACT"), 2002));
		T.Add(Monster(84, TEXT("WolfensteinSS"), TEXT("SSWV"), TEXT("AB"), TEXT("ABCD"), TEXT(""), TEXT("EFG"), TEXT("H"), TEXT("IJKLM"), TEXT("NOPQRSTUV"),
			20, 56, 50, 8, 3, 170, 0, EA::Chaingun, TEXT(""), 0, TEXT("SSSIT"), TEXT("SHOTGN"), TEXT("POPAIN"), TEXT("SSDTH"), TEXT("POSACT"), 2007));
		T.Add(Monster(3001, TEXT("Imp"), TEXT("TROO"), TEXT("AB"), TEXT("ABCD"), TEXT("EFG"), TEXT("EFG"), TEXT("H"), TEXT("IJKLM"), TEXT("NOPQRSTU"),
			20, 56, 60, 8, 3, 200, 0, EA::MeleeOrMissile, TEXT("ImpBall"), 3, TEXT("BGSIT1"), TEXT("CLAW"), TEXT("POPAIN"), TEXT("BGDTH1"), TEXT("BGACT")));
		T.Add(Monster(3002, TEXT("Demon"), TEXT("SARG"), TEXT("AB"), TEXT("ABCD"), TEXT("EFG"), TEXT(""), TEXT("H"), TEXT("IJKLMN"), TEXT(""),
			30, 56, 150, 10, 2, 180, 0, EA::Melee, TEXT(""), 4, TEXT("SGTSIT"), TEXT("SGTATK"), TEXT("DMPAIN"), TEXT("SGTDTH"), TEXT("DMACT")));
		T.Add(Monster(58, TEXT("Spectre"), TEXT("SARG"), TEXT("AB"), TEXT("ABCD"), TEXT("EFG"), TEXT(""), TEXT("H"), TEXT("IJKLMN"), TEXT(""),
			30, 56, 150, 10, 2, 180, Shadow, EA::Melee, TEXT(""), 4, TEXT("SGTSIT"), TEXT("SGTATK"), TEXT("DMPAIN"), TEXT("SGTDTH"), TEXT("DMACT")));
		{
			FDoomThingDef Skull = Monster(3006, TEXT("LostSoul"), TEXT("SKUL"), TEXT("AB"), TEXT("AB"), TEXT(""), TEXT("CD"), TEXT("E"), TEXT("FGHIJK"), TEXT(""),
				16, 56, 100, 8, 6, 256, Float | FullBright | NoBlood, EA::Skull, TEXT(""), 3, TEXT(""), TEXT("SKLATK"), TEXT("DMPAIN"), TEXT("FIRXPL"), TEXT("DMACT"));
			Skull.Flags &= ~CountKill;
			Skull.LightColor = Fire;
			T.Add(Skull);
		}
		T.Add(Monster(3005, TEXT("Cacodemon"), TEXT("HEAD"), TEXT("A"), TEXT("A"), TEXT(""), TEXT("BCD"), TEXT("EF"), TEXT("GHIJKL"), TEXT(""),
			31, 56, 400, 8, 3, 128, Float, EA::MeleeOrMissile, TEXT("CacoBall"), 10, TEXT("CACSIT"), TEXT(""), TEXT("DMPAIN"), TEXT("CACDTH"), TEXT("DMACT")));
		T.Add(Monster(3003, TEXT("BaronOfHell"), TEXT("BOSS"), TEXT("AB"), TEXT("ABCD"), TEXT("EFG"), TEXT("EFG"), TEXT("H"), TEXT("IJKLMNO"), TEXT(""),
			24, 64, 1000, 8, 3, 50, 0, EA::MeleeOrMissile, TEXT("BaronBall"), 10, TEXT("BRSSIT"), TEXT(""), TEXT("DMPAIN"), TEXT("BRSDTH"), TEXT("DMACT")));
		T.Add(Monster(69, TEXT("HellKnight"), TEXT("BOS2"), TEXT("AB"), TEXT("ABCD"), TEXT("EFG"), TEXT("EFG"), TEXT("H"), TEXT("IJKLMNO"), TEXT(""),
			24, 64, 500, 8, 3, 50, 0, EA::MeleeOrMissile, TEXT("BaronBall"), 10, TEXT("KNTSIT"), TEXT(""), TEXT("DMPAIN"), TEXT("KNTDTH"), TEXT("DMACT")));
		T.Add(Monster(16, TEXT("Cyberdemon"), TEXT("CYBR"), TEXT("AB"), TEXT("ABCD"), TEXT(""), TEXT("EFEFEF"), TEXT("G"), TEXT("HIJKLMNOP"), TEXT(""),
			40, 110, 4000, 16, 3, 20, Boss, EA::Cyber, TEXT("Rocket"), 0, TEXT("CYBSIT"), TEXT(""), TEXT("DMPAIN"), TEXT("CYBDTH"), TEXT("DMACT")));
		T.Add(Monster(7, TEXT("SpiderMastermind"), TEXT("SPID"), TEXT("AB"), TEXT("ABCDEF"), TEXT(""), TEXT("GHGH"), TEXT("I"), TEXT("JKLMNOPQRS"), TEXT(""),
			128, 100, 3000, 12, 3, 40, Boss, EA::Spider, TEXT(""), 0, TEXT("SPISIT"), TEXT("SHOTGN"), TEXT("DMPAIN"), TEXT("SPIDTH"), TEXT("DMACT")));
		T.Add(Monster(68, TEXT("Arachnotron"), TEXT("BSPI"), TEXT("AB"), TEXT("ABCDEF"), TEXT(""), TEXT("GHGH"), TEXT("I"), TEXT("JKLMNOP"), TEXT(""),
			64, 64, 500, 12, 3, 128, 0, EA::Missile, TEXT("ArachPlasma"), 0, TEXT("BSPSIT"), TEXT(""), TEXT("DMPAIN"), TEXT("BSPDTH"), TEXT("BSPACT")));
		T.Add(Monster(71, TEXT("PainElemental"), TEXT("PAIN"), TEXT("A"), TEXT("ABC"), TEXT(""), TEXT("BCD"), TEXT("EF"), TEXT("GHIJKLM"), TEXT(""),
			31, 56, 400, 8, 3, 128, Float, EA::PainSpawn, TEXT(""), 0, TEXT("PESIT"), TEXT(""), TEXT("PEPAIN"), TEXT("PEDTH"), TEXT("DMACT")));
		T.Add(Monster(66, TEXT("Revenant"), TEXT("SKEL"), TEXT("AB"), TEXT("ABCDEF"), TEXT("GHI"), TEXT("JK"), TEXT("L"), TEXT("LMNOPQ"), TEXT(""),
			20, 56, 300, 10, 2, 100, 0, EA::Revenant, TEXT("Tracer"), 6, TEXT("SKESIT"), TEXT("SKESWG"), TEXT("POPAIN"), TEXT("SKEDTH"), TEXT("SKEACT")));
		T.Add(Monster(67, TEXT("Mancubus"), TEXT("FATT"), TEXT("AB"), TEXT("ABCDEF"), TEXT(""), TEXT("GHIGHIGH"), TEXT("J"), TEXT("KLMNOPQRST"), TEXT(""),
			48, 64, 600, 8, 4, 80, 0, EA::Mancubus, TEXT("FatShot"), 0, TEXT("MANSIT"), TEXT("MANATK"), TEXT("MNPAIN"), TEXT("MANDTH"), TEXT("POSACT")));
		T.Add(Monster(64, TEXT("ArchVile"), TEXT("VILE"), TEXT("AB"), TEXT("ABCDEF"), TEXT(""), TEXT("GHIJKLMNOP"), TEXT("Q"), TEXT("QRSTUVWXYZ"), TEXT(""),
			20, 56, 700, 15, 2, 10, 0, EA::Vile, TEXT(""), 0, TEXT("VILSIT"), TEXT("VILATK"), TEXT("VIPAIN"), TEXT("VILDTH"), TEXT("VILACT")));
		T.Add(Monster(72, TEXT("CommanderKeen"), TEXT("KEEN"), TEXT("A"), TEXT(""), TEXT(""), TEXT(""), TEXT("M"), TEXT("ABCDEFGHIJKL"), TEXT(""),
			16, 72, 100, 0, 4, 256, Hanging, EA::None, TEXT(""), 0, TEXT(""), TEXT(""), TEXT("KEENPN"), TEXT("KEENDT"), TEXT("")));
		{
			FDoomThingDef Brain = Monster(88, TEXT("BossBrain"), TEXT("BBRN"), TEXT("A"), TEXT(""), TEXT(""), TEXT(""), TEXT("B"), TEXT("A"), TEXT(""),
				16, 16, 250, 0, 4, 255, Boss | NoBlood, EA::None, TEXT(""), 0, TEXT(""), TEXT(""), TEXT("BOSPN"), TEXT("BOSDTH"), TEXT(""));
			T.Add(Brain);
		}

		// --- barrel ----------------------------------------------------------------------------------------------------------
		{
			FDoomThingDef Barrel = Decor(2035, TEXT("Barrel"), TEXT("BAR1"), TEXT("AB"), 10, 42, Solid | Shootable | EThingFlag::Barrel | NoBlood);
			Barrel.Health = 20;
			Barrel.Death = TEXT("ABCDE");
			Barrel.DeathSound = TEXT("BAREXP");
			T.Add(Barrel);
		}

		// --- pickups ---------------------------------------------------------------------------------------------------------
		T.Add(Item(2011, TEXT("Stimpack"), TEXT("STIM"), TEXT("A"), EPickup::Health, 0));
		T.Add(Item(2012, TEXT("Medikit"), TEXT("MEDI"), TEXT("A"), EPickup::Health, 0));
		T.Add(Item(2014, TEXT("HealthBonus"), TEXT("BON1"), TEXT("ABCDCB"), EPickup::HealthBonus));
		T.Add(Item(2015, TEXT("ArmorBonus"), TEXT("BON2"), TEXT("ABCDCB"), EPickup::ArmorBonus));
		T.Add(Item(2018, TEXT("GreenArmor"), TEXT("ARM1"), TEXT("AB"), EPickup::GreenArmor, 0));
		T.Add(Item(2019, TEXT("BlueArmor"), TEXT("ARM2"), TEXT("AB"), EPickup::BlueArmor, 0));
		T.Add(Item(2013, TEXT("Soulsphere"), TEXT("SOUL"), TEXT("ABCDCB"), EPickup::Soulsphere, CountItem | FullBright));
		T.Add(Item(83, TEXT("Megasphere"), TEXT("MEGA"), TEXT("ABCD"), EPickup::Megasphere, CountItem | FullBright));
		T.Add(Item(2022, TEXT("Invulnerability"), TEXT("PINV"), TEXT("ABCD"), EPickup::Invulnerability, CountItem | FullBright));
		T.Add(Item(2023, TEXT("Berserk"), TEXT("PSTR"), TEXT("A"), EPickup::Berserk, CountItem | FullBright));
		T.Add(Item(2024, TEXT("Invisibility"), TEXT("PINS"), TEXT("ABCD"), EPickup::Invisibility, CountItem | FullBright));
		T.Add(Item(2025, TEXT("RadiationSuit"), TEXT("SUIT"), TEXT("A"), EPickup::RadSuit, FullBright));
		T.Add(Item(2026, TEXT("ComputerMap"), TEXT("PMAP"), TEXT("ABCDCB"), EPickup::Map, CountItem | FullBright));
		T.Add(Item(2045, TEXT("LightAmp"), TEXT("PVIS"), TEXT("AB"), EPickup::LightAmp, CountItem | FullBright));
		T.Add(Item(5, TEXT("BlueCard"), TEXT("BKEY"), TEXT("AB"), EPickup::BlueCard, 0, 10));
		T.Add(Item(6, TEXT("YellowCard"), TEXT("YKEY"), TEXT("AB"), EPickup::YellowCard, 0, 10));
		T.Add(Item(13, TEXT("RedCard"), TEXT("RKEY"), TEXT("AB"), EPickup::RedCard, 0, 10));
		T.Add(Item(40, TEXT("BlueSkull"), TEXT("BSKU"), TEXT("AB"), EPickup::BlueSkull, FullBright, 10));
		T.Add(Item(39, TEXT("YellowSkull"), TEXT("YSKU"), TEXT("AB"), EPickup::YellowSkull, FullBright, 10));
		T.Add(Item(38, TEXT("RedSkull"), TEXT("RSKU"), TEXT("AB"), EPickup::RedSkull, FullBright, 10));
		T.Add(Item(2007, TEXT("Clip"), TEXT("CLIP"), TEXT("A"), EPickup::Clip, 0));
		T.Add(Item(2048, TEXT("BoxOfBullets"), TEXT("AMMO"), TEXT("A"), EPickup::ClipBox, 0));
		T.Add(Item(2008, TEXT("Shells"), TEXT("SHEL"), TEXT("A"), EPickup::Shells, 0));
		T.Add(Item(2049, TEXT("BoxOfShells"), TEXT("SBOX"), TEXT("A"), EPickup::ShellBox, 0));
		T.Add(Item(2010, TEXT("Rocket"), TEXT("ROCK"), TEXT("A"), EPickup::Rocket, 0));
		T.Add(Item(2046, TEXT("BoxOfRockets"), TEXT("BROK"), TEXT("A"), EPickup::RocketBox, 0));
		T.Add(Item(2047, TEXT("Cell"), TEXT("CELL"), TEXT("A"), EPickup::Cell, 0));
		T.Add(Item(17, TEXT("CellPack"), TEXT("CELP"), TEXT("A"), EPickup::CellPack, 0));
		T.Add(Item(8, TEXT("Backpack"), TEXT("BPAK"), TEXT("A"), EPickup::Backpack, 0));
		T.Add(Item(2005, TEXT("Chainsaw"), TEXT("CSAW"), TEXT("A"), EPickup::Chainsaw, 0));
		T.Add(Item(2001, TEXT("Shotgun"), TEXT("SHOT"), TEXT("A"), EPickup::Shotgun, 0));
		T.Add(Item(82, TEXT("SuperShotgun"), TEXT("SGN2"), TEXT("A"), EPickup::SuperShotgun, 0));
		T.Add(Item(2002, TEXT("Chaingun"), TEXT("MGUN"), TEXT("A"), EPickup::Chaingun, 0));
		T.Add(Item(2003, TEXT("RocketLauncher"), TEXT("LAUN"), TEXT("A"), EPickup::RocketLauncher, 0));
		T.Add(Item(2004, TEXT("PlasmaGun"), TEXT("PLAS"), TEXT("A"), EPickup::Plasma, 0));
		T.Add(Item(2006, TEXT("BFG9000"), TEXT("BFUG"), TEXT("A"), EPickup::BFG, 0));

		// --- decorations -----------------------------------------------------------------------------------------------------
		T.Add(Decor(2028, TEXT("FloorLamp"), TEXT("COLU"), TEXT("A"), 16, 48, Solid, White));
		T.Add(Decor(85, TEXT("TallTechLamp"), TEXT("TLMP"), TEXT("ABCD"), 16, 80, Solid, White, 4));
		T.Add(Decor(86, TEXT("ShortTechLamp"), TEXT("TLP2"), TEXT("ABCD"), 16, 60, Solid, White, 4));
		T.Add(Decor(34, TEXT("Candle"), TEXT("CAND"), TEXT("A"), 20, 16, 0, Fire));
		T.Add(Decor(35, TEXT("Candelabra"), TEXT("CBRA"), TEXT("A"), 16, 60, Solid, Fire));
		T.Add(Decor(44, TEXT("TallBlueTorch"), TEXT("TBLU"), TEXT("ABCD"), 16, 64, Solid, BlueFire, 4));
		T.Add(Decor(45, TEXT("TallGreenTorch"), TEXT("TGRN"), TEXT("ABCD"), 16, 64, Solid, GreenFire, 4));
		T.Add(Decor(46, TEXT("TallRedTorch"), TEXT("TRED"), TEXT("ABCD"), 16, 64, Solid, RedFire, 4));
		T.Add(Decor(55, TEXT("ShortBlueTorch"), TEXT("SMBT"), TEXT("ABCD"), 16, 40, Solid, BlueFire, 4));
		T.Add(Decor(56, TEXT("ShortGreenTorch"), TEXT("SMGT"), TEXT("ABCD"), 16, 40, Solid, GreenFire, 4));
		T.Add(Decor(57, TEXT("ShortRedTorch"), TEXT("SMRT"), TEXT("ABCD"), 16, 40, Solid, RedFire, 4));
		T.Add(Decor(70, TEXT("BurningBarrel"), TEXT("FCAN"), TEXT("ABC"), 16, 32, Solid, Fire, 4));
		T.Add(Decor(41, TEXT("EvilEye"), TEXT("CEYE"), TEXT("ABCB"), 16, 54, Solid, GreenFire));
		T.Add(Decor(42, TEXT("FloatingSkull"), TEXT("FSKU"), TEXT("ABC"), 16, 26, Solid, Fire));
		T.Add(Decor(30, TEXT("TallGreenPillar"), TEXT("COL1"), TEXT("A"), 16, 52, Solid));
		T.Add(Decor(31, TEXT("ShortGreenPillar"), TEXT("COL2"), TEXT("A"), 16, 40, Solid));
		T.Add(Decor(32, TEXT("TallRedPillar"), TEXT("COL3"), TEXT("A"), 16, 52, Solid));
		T.Add(Decor(33, TEXT("ShortRedPillar"), TEXT("COL4"), TEXT("A"), 16, 40, Solid));
		T.Add(Decor(36, TEXT("PillarWithHeart"), TEXT("COL5"), TEXT("AB"), 16, 40, Solid, FLinearColor::Transparent, 14));
		T.Add(Decor(37, TEXT("PillarWithSkull"), TEXT("COL6"), TEXT("A"), 16, 40, Solid));
		T.Add(Decor(43, TEXT("BurntTree"), TEXT("TRE1"), TEXT("A"), 16, 40, Solid));
		T.Add(Decor(54, TEXT("BigTree"), TEXT("TRE2"), TEXT("A"), 32, 108, Solid));
		T.Add(Decor(47, TEXT("Stalagmite"), TEXT("SMIT"), TEXT("A"), 16, 40, Solid));
		T.Add(Decor(48, TEXT("TechColumn"), TEXT("ELEC"), TEXT("A"), 16, 128, Solid));
		T.Add(Decor(25, TEXT("ImpaledHuman"), TEXT("POL1"), TEXT("A"), 16, 64, Solid));
		T.Add(Decor(26, TEXT("TwitchingImpaled"), TEXT("POL6"), TEXT("AB"), 16, 64, Solid, FLinearColor::Transparent, 8));
		T.Add(Decor(27, TEXT("SkullOnPole"), TEXT("POL4"), TEXT("A"), 16, 64, Solid));
		T.Add(Decor(28, TEXT("SkullKebab"), TEXT("POL2"), TEXT("A"), 16, 64, Solid));
		T.Add(Decor(29, TEXT("PileOfSkulls"), TEXT("POL3"), TEXT("AB"), 16, 64, Solid, Fire));
		T.Add(Decor(49, TEXT("HangingVictim"), TEXT("GOR1"), TEXT("ABCB"), 16, 68, Solid | Hanging, FLinearColor::Transparent, 10));
		T.Add(Decor(50, TEXT("HangingArmsOut"), TEXT("GOR2"), TEXT("A"), 16, 84, Solid | Hanging));
		T.Add(Decor(51, TEXT("HangingOneLegged"), TEXT("GOR3"), TEXT("A"), 16, 84, Solid | Hanging));
		T.Add(Decor(52, TEXT("HangingPair"), TEXT("GOR4"), TEXT("A"), 16, 68, Solid | Hanging));
		T.Add(Decor(53, TEXT("HangingLeg"), TEXT("GOR5"), TEXT("A"), 16, 52, Solid | Hanging));
		T.Add(Decor(59, TEXT("HangingArmsOutNB"), TEXT("GOR2"), TEXT("A"), 20, 84, Hanging));
		T.Add(Decor(60, TEXT("HangingPairNB"), TEXT("GOR4"), TEXT("A"), 20, 68, Hanging));
		T.Add(Decor(61, TEXT("HangingOneLeggedNB"), TEXT("GOR3"), TEXT("A"), 20, 52, Hanging));
		T.Add(Decor(62, TEXT("HangingLegNB"), TEXT("GOR5"), TEXT("A"), 20, 52, Hanging));
		T.Add(Decor(63, TEXT("HangingVictimNB"), TEXT("GOR1"), TEXT("ABCB"), 20, 68, Hanging, FLinearColor::Transparent, 10));
		T.Add(Decor(73, TEXT("HangingGuts1"), TEXT("HDB1"), TEXT("A"), 16, 88, Solid | Hanging));
		T.Add(Decor(74, TEXT("HangingGuts2"), TEXT("HDB2"), TEXT("A"), 16, 88, Solid | Hanging));
		T.Add(Decor(75, TEXT("HangingTorso1"), TEXT("HDB3"), TEXT("A"), 16, 64, Solid | Hanging));
		T.Add(Decor(76, TEXT("HangingTorso2"), TEXT("HDB4"), TEXT("A"), 16, 64, Solid | Hanging));
		T.Add(Decor(77, TEXT("HangingTorso3"), TEXT("HDB5"), TEXT("A"), 16, 64, Solid | Hanging));
		T.Add(Decor(78, TEXT("HangingTorso4"), TEXT("HDB6"), TEXT("A"), 16, 64, Solid | Hanging));
		T.Add(Decor(10, TEXT("BloodyMess"), TEXT("PLAY"), TEXT("W"), 20, 16, 0));
		T.Add(Decor(12, TEXT("BloodyMess2"), TEXT("PLAY"), TEXT("W"), 20, 16, 0));
		T.Add(Decor(15, TEXT("DeadPlayer"), TEXT("PLAY"), TEXT("N"), 20, 16, 0));
		T.Add(Decor(18, TEXT("DeadZombieman"), TEXT("POSS"), TEXT("L"), 20, 16, 0));
		T.Add(Decor(19, TEXT("DeadShotgunGuy"), TEXT("SPOS"), TEXT("L"), 20, 16, 0));
		T.Add(Decor(20, TEXT("DeadImp"), TEXT("TROO"), TEXT("M"), 20, 16, 0));
		T.Add(Decor(21, TEXT("DeadDemon"), TEXT("SARG"), TEXT("N"), 20, 16, 0));
		T.Add(Decor(22, TEXT("DeadCacodemon"), TEXT("HEAD"), TEXT("L"), 20, 16, 0));
		T.Add(Decor(23, TEXT("DeadLostSoul"), TEXT("SKUL"), TEXT("K"), 20, 16, 0));
		T.Add(Decor(24, TEXT("PoolOfBlood"), TEXT("POL5"), TEXT("A"), 20, 16, 0));
		T.Add(Decor(79, TEXT("PoolOfBlood2"), TEXT("POB1"), TEXT("A"), 20, 16, 0));
		T.Add(Decor(80, TEXT("PoolOfBlood3"), TEXT("POB2"), TEXT("A"), 20, 16, 0));
		T.Add(Decor(81, TEXT("BrainStem"), TEXT("BRS1"), TEXT("A"), 20, 16, 0));
		return T;
	}

	TArray<FDoomProjectileDef> BuildProjectiles()
	{
		return {
			{ TEXT("ImpBall"),     TEXT("BAL1"), TEXT("AB"),  TEXT("CDE"),  10, 6,  3,   0, TEXT("FIRSHT"), TEXT("FIRXPL"), FLinearColor(1.f, 0.5f, 0.15f) },
			{ TEXT("CacoBall"),    TEXT("BAL2"), TEXT("AB"),  TEXT("CDE"),  10, 6,  5,   0, TEXT("FIRSHT"), TEXT("FIRXPL"), FLinearColor(0.8f, 0.2f, 1.f) },
			{ TEXT("BaronBall"),   TEXT("BAL7"), TEXT("AB"),  TEXT("CDE"),  15, 6,  8,   0, TEXT("FIRSHT"), TEXT("FIRXPL"), FLinearColor(0.3f, 1.f, 0.3f) },
			{ TEXT("Rocket"),      TEXT("MISL"), TEXT("A"),   TEXT("BCD"),  20, 11, 20, 128, TEXT("RLAUNC"), TEXT("BAREXP"), FLinearColor(1.f, 0.6f, 0.2f) },
			{ TEXT("Plasma"),      TEXT("PLSS"), TEXT("AB"),  TEXT("ABCDE"), 25, 13, 5,   0, TEXT("PLASMA"), TEXT("FIRXPL"), FLinearColor(0.3f, 0.5f, 1.f) },
			{ TEXT("BFGBall"),     TEXT("BFS1"), TEXT("AB"),  TEXT("ABCDEF"), 25, 13, 100, 0, TEXT("BFG"),    TEXT("RXPLOD"), FLinearColor(0.3f, 1.f, 0.3f) },
			{ TEXT("ArachPlasma"), TEXT("APLS"), TEXT("AB"),  TEXT("ABCDE"), 25, 13, 5,   0, TEXT("PLASMA"), TEXT("FIRXPL"), FLinearColor(0.4f, 1.f, 0.4f) },
			{ TEXT("FatShot"),     TEXT("MANF"), TEXT("AB"),  TEXT("CDE"),  20, 6,  8,   0, TEXT("FIRSHT"), TEXT("FIRXPL"), FLinearColor(1.f, 0.5f, 0.15f) },
			{ TEXT("Tracer"),      TEXT("FATB"), TEXT("AB"),  TEXT("CDE"),  10, 11, 10,  0, TEXT("SKEATK"), TEXT("BAREXP"), FLinearColor(1.f, 0.4f, 0.2f), true },
		};
	}

	TArray<FDoomWeaponDef> BuildWeapons()
	{
		TArray<FDoomWeaponDef> W;
		W.Add({ EDoomWeapon::Fist, TEXT("PUNG"), TEXT(""), 'A', EDoomAmmo::None, 0,
			{ {'B', 4, false, -1}, {'C', 4, true, -1}, {'D', 5, false, -1}, {'C', 4, false, -1}, {'B', 5, false, -1} }, TEXT("PUNCH"), 1 });
		W.Add({ EDoomWeapon::Pistol, TEXT("PISG"), TEXT("PISF"), 'A', EDoomAmmo::Bullets, 1,
			{ {'A', 4, false, -1}, {'B', 6, true, 0}, {'C', 4, false, -1}, {'B', 5, false, -1} }, TEXT("PISTOL"), 2 });
		W.Add({ EDoomWeapon::Shotgun, TEXT("SHTG"), TEXT("SHTF"), 'A', EDoomAmmo::Shells, 1,
			{ {'A', 3, false, -1}, {'A', 7, true, 0}, {'B', 5, false, -1}, {'C', 5, false, -1}, {'D', 4, false, -1}, {'C', 5, false, -1}, {'B', 5, false, -1}, {'A', 3, false, -1}, {'A', 7, false, -1} }, TEXT("SHOTGN"), 3 });
		W.Add({ EDoomWeapon::Chaingun, TEXT("CHGG"), TEXT("CHGF"), 'A', EDoomAmmo::Bullets, 1,
			{ {'A', 4, true, 0}, {'B', 4, true, 1} }, TEXT("PISTOL"), 4 });
		W.Add({ EDoomWeapon::Rocket, TEXT("MISG"), TEXT("MISF"), 'A', EDoomAmmo::Rockets, 1,
			{ {'B', 8, false, 0}, {'B', 12, true, 1}, {'B', 0, false, -1} }, TEXT(""), 5 });
		W.Add({ EDoomWeapon::Plasma, TEXT("PLSG"), TEXT("PLSF"), 'A', EDoomAmmo::Cells, 1,
			{ {'A', 3, true, 0}, {'B', 20, false, -1} }, TEXT(""), 6 });
		W.Add({ EDoomWeapon::BFG, TEXT("BFGG"), TEXT("BFGF"), 'A', EDoomAmmo::Cells, 40,
			{ {'A', 20, false, -1}, {'B', 10, false, 0}, {'B', 10, true, 1}, {'B', 20, false, -1} }, TEXT("BFG"), 7 });
		W.Add({ EDoomWeapon::Chainsaw, TEXT("SAWG"), TEXT(""), 'C', EDoomAmmo::None, 0,
			{ {'A', 4, true, -1}, {'B', 4, true, -1} }, TEXT("SAWFUL"), 1 });
		W.Add({ EDoomWeapon::SuperShotgun, TEXT("SHT2"), TEXT("SHT2"), 'A', EDoomAmmo::Shells, 2,
			{ {'A', 3, false, -1}, {'A', 7, true, 8}, {'B', 7, false, -1}, {'C', 7, false, -1}, {'D', 7, false, -1}, {'E', 7, false, -1}, {'F', 7, false, -1}, {'G', 6, false, -1}, {'H', 6, false, -1}, {'A', 5, false, -1} }, TEXT("DSHTGN"), 3 });
		return W;
	}

	FRandomStream GRandom(0x0D00);
}

// The tables are patchable (DeHackEd): ResetTables restores the base tables (the UDoomGameData
// asset, else the built-in ones) before patches apply.
namespace
{
	TArray<FDoomThingDef>& ThingTable() { static TArray<FDoomThingDef> T = DoomInfoBuiltIn::BuildThings(); return T; }
	TArray<FDoomProjectileDef>& ProjectileTable() { static TArray<FDoomProjectileDef> T = DoomInfoBuiltIn::BuildProjectiles(); return T; }
	TArray<FDoomWeaponDef>& WeaponTable() { static TArray<FDoomWeaponDef> T = DoomInfoBuiltIn::BuildWeapons(); return T; }
	TMap<int32, int32>& ThingIndex()
	{
		static TMap<int32, int32> M = []()
		{
			TMap<int32, int32> Index;
			for (int32 i = 0; i < ThingTable().Num(); ++i) Index.Add(ThingTable()[i].DoomEdNum, i);
			return Index;
		}();
		return M;
	}
	FString GTablesSource = TEXT("built-in");
	FDoomRules GRules;
	TMap<FString, FString> GStrings;
	TMap<FString, int32> GPars;
	TMap<FString, FString> GSpriteRenames, GSoundRenames;
}

FDoomThingDef* DoomInfo::MutableThing(int32 DoomEdNum)
{
	const int32* I = ThingIndex().Find(DoomEdNum);
	return I ? &ThingTable()[*I] : nullptr;
}

const FDoomThingDef* DoomInfo::FindThing(int32 DoomEdNum)
{
	return MutableThing(DoomEdNum);
}

const FDoomThingDef* DoomInfo::FindThingByName(const FString& Name)
{
	// ZDoom class names used by UMAPINFO, plus the port's own names
	static const TMap<FString, FString> Aliases = {
		{ TEXT("zombieman"), TEXT("Zombieman") }, { TEXT("chaingunguy"), TEXT("Chaingunner") }, { TEXT("doomimp"), TEXT("Imp") },
		{ TEXT("fatso"), TEXT("Mancubus") }, { TEXT("archvile"), TEXT("ArchVile") }, { TEXT("explosivebarrel"), TEXT("Barrel") } };
	const FString* Alias = Aliases.Find(Name.ToLower());
	const FString Wanted = Alias ? *Alias : Name;
	for (const FDoomThingDef& D : ThingTable())
	{
		if (Wanted.Equals(D.Name, ESearchCase::IgnoreCase)) return &D;
	}
	return nullptr;
}

TConstArrayView<FDoomThingDef> DoomInfo::AllThings()
{
	return ThingTable();
}

FDoomProjectileDef* DoomInfo::MutableProjectile(const FString& Name)
{
	for (FDoomProjectileDef& D : ProjectileTable())
	{
		if (Name == D.Name) return &D;
	}
	return nullptr;
}

const FDoomProjectileDef* DoomInfo::FindProjectile(const FString& Name)
{
	return MutableProjectile(Name);
}

FDoomWeaponDef& DoomInfo::MutableWeapon(EDoomWeapon W)
{
	TArray<FDoomWeaponDef>& Defs = WeaponTable();
	return Defs[FMath::Clamp((int32)W, 0, Defs.Num() - 1)];
}

const FDoomWeaponDef& DoomInfo::Weapon(EDoomWeapon W)
{
	return MutableWeapon(W);
}

FDoomRules& DoomInfo::Rules()
{
	return GRules;
}

int32 DoomInfo::MaxAmmo(EDoomAmmo A, bool bBackpack)
{
	return A == EDoomAmmo::None ? 0 : GRules.MaxAmmo[(int32)A] * (bBackpack ? 2 : 1);
}

void DoomInfo::BuiltInTables(TArray<FDoomThingDef>& Things, TArray<FDoomProjectileDef>& Projectiles, TArray<FDoomWeaponDef>& Weapons)
{
	Things = DoomInfoBuiltIn::BuildThings();
	Projectiles = DoomInfoBuiltIn::BuildProjectiles();
	Weapons = DoomInfoBuiltIn::BuildWeapons();
}

FString DoomInfo::TablesSource()
{
	return GTablesSource;
}

void DoomInfo::ResetTables()
{
	ThingTable() = DoomInfoBuiltIn::BuildThings();
	ProjectileTable() = DoomInfoBuiltIn::BuildProjectiles();
	WeaponTable() = DoomInfoBuiltIn::BuildWeapons();
	GRules = FDoomRules();
	GTablesSource = TEXT("built-in");
	// the editable copy, when the project has one
	const TSoftObjectPtr<UDoomGameData>& Path = GetDefault<UDoomSettings>()->GameData;
	if (const UDoomGameData* Data = Path.IsNull() ? nullptr : Path.LoadSynchronous())
	{
		if (Data->Things.Num())
		{
			ThingTable() = Data->Things;
		}
		if (Data->Projectiles.Num())
		{
			ProjectileTable() = Data->Projectiles;
		}
		// one entry per weapon, in EDoomWeapon order (Weapon() indexes by it); missing ones stay built in
		for (const FDoomWeaponDef& W : Data->Weapons)
		{
			if ((int32)W.Weapon < WeaponTable().Num())
			{
				WeaponTable()[(int32)W.Weapon] = W;
			}
		}
		GRules = Data->Rules;
		GTablesSource = Data->GetPathName();
		// a broken table shows up here in packaged games too (the editor's Validate Data says the same)
		FDoomDataProblems Problems;
		Data->Check(Problems);
		for (const FText& E : Problems.Errors)
		{
			UE_LOG(LogDoomGame, Error, TEXT("Game data %s: %s"), *GTablesSource, *E.ToString());
		}
		for (const FText& W : Problems.Warnings)
		{
			UE_LOG(LogDoomGame, Warning, TEXT("Game data %s: %s"), *GTablesSource, *W.ToString());
		}
	}
	ThingIndex().Reset();
	for (int32 i = 0; i < ThingTable().Num(); ++i)
	{
		ThingIndex().Add(ThingTable()[i].DoomEdNum, i);
	}
	UE_LOG(LogDoom, Log, TEXT("Game tables: %d things, %d projectiles, %d weapons (%s)"),
		ThingTable().Num(), ProjectileTable().Num(), WeaponTable().Num(), *GTablesSource);
	GStrings.Reset();
	GPars.Reset();
	// vanilla par times (g_game.c pars / cpars; episode 4 has none); DeHackEd and UMAPINFO override them
	static const int32 Pars[3][9] = {
		{ 30, 75, 120, 90, 165, 180, 180, 30, 165 },
		{ 90, 90, 90, 120, 90, 360, 240, 30, 170 },
		{ 90, 45, 90, 150, 90, 90, 165, 30, 135 } };
	static const int32 CPars[32] = {
		30, 90, 120, 120, 90, 150, 120, 120, 270, 90, 210, 150, 150, 150, 210, 150,
		420, 150, 210, 150, 240, 150, 180, 150, 150, 300, 330, 420, 300, 180, 120, 30 };
	for (int32 e = 0; e < 3; ++e)
	{
		for (int32 m = 0; m < 9; ++m) GPars.Add(FString::Printf(TEXT("E%dM%d"), e + 1, m + 1), Pars[e][m]);
	}
	for (int32 m = 0; m < 32; ++m) GPars.Add(FString::Printf(TEXT("MAP%02d"), m + 1), CPars[m]);
	GSpriteRenames.Reset();
	GSoundRenames.Reset();
}

void DoomInfo::SetString(const FString& Mnemonic, const FString& Text)
{
	GStrings.Add(Mnemonic.ToUpper(), Text);
}

FString DoomInfo::FindStringKey(const FString& Text)
{
	const FString Wanted = Text.TrimStartAndEnd();
	for (const TPair<FString, FString>& Pair : GStrings)
	{
		if (Pair.Value.TrimStartAndEnd().Equals(Wanted, ESearchCase::IgnoreCase))
		{
			return Pair.Key;
		}
	}
	return FString();
}

void DoomInfo::SetSpriteRename(const FString& From, const FString& To)
{
	GSpriteRenames.Add(From.ToUpper(), To.ToUpper());
}

FString DoomInfo::SpriteName(const FString& Prefix)
{
	const FString* R = GSpriteRenames.Find(Prefix);
	return R ? *R : Prefix;
}

void DoomInfo::SetSoundRename(const FString& From, const FString& To)
{
	GSoundRenames.Add(From.ToUpper(), To.ToUpper());
}

FString DoomInfo::SoundName(const FString& Name)
{
	const FString* R = GSoundRenames.Find(Name.ToUpper());
	return R ? *R : Name;
}

FString DoomInfo::Text(const TCHAR* Mnemonic, const FString& Default)
{
	const FString* Found = GStrings.Find(FString(Mnemonic).ToUpper());
	return Found ? *Found : Default;
}

bool DoomInfo::HasText(const TCHAR* Mnemonic)
{
	return GStrings.Contains(FString(Mnemonic).ToUpper());
}

void DoomInfo::SetParTime(const FString& Map, int32 Seconds)
{
	GPars.Add(Map.ToUpper(), Seconds);
}

int32 DoomInfo::ParTime(const FString& Map)
{
	const int32* P = GPars.Find(Map.ToUpper());
	return P ? *P : -1;
}

int32 DoomInfo::Random()
{
	return DoomInfoBuiltIn::GRandom.RandRange(0, 255);
}

static FString DefaultPickupMessage(EPickup P);

FString DoomInfo::PickupMessage(EPickup P)
{
	// mnemonics of the BEX [STRINGS] / DEHACKED string table (text comes from the WAD)
	static const TMap<EPickup, const TCHAR*> Mnemonics = {
		{ EPickup::HealthBonus, TEXT("GOTHTHBONUS") }, { EPickup::ArmorBonus, TEXT("GOTARMBONUS") },
		{ EPickup::Soulsphere, TEXT("GOTSUPER") }, { EPickup::Megasphere, TEXT("GOTMSPHERE") },
		{ EPickup::GreenArmor, TEXT("GOTARMOR") }, { EPickup::BlueArmor, TEXT("GOTMEGA") },
		{ EPickup::Invulnerability, TEXT("GOTINVUL") }, { EPickup::Berserk, TEXT("GOTBERSERK") },
		{ EPickup::Invisibility, TEXT("GOTINVIS") }, { EPickup::RadSuit, TEXT("GOTSUIT") },
		{ EPickup::Map, TEXT("GOTMAP") }, { EPickup::LightAmp, TEXT("GOTVISOR") },
		{ EPickup::BlueCard, TEXT("GOTBLUECARD") }, { EPickup::YellowCard, TEXT("GOTYELWCARD") }, { EPickup::RedCard, TEXT("GOTREDCARD") },
		{ EPickup::BlueSkull, TEXT("GOTBLUESKUL") }, { EPickup::YellowSkull, TEXT("GOTYELWSKUL") }, { EPickup::RedSkull, TEXT("GOTREDSKULL") },
		{ EPickup::Clip, TEXT("GOTCLIP") }, { EPickup::ClipBox, TEXT("GOTCLIPBOX") }, { EPickup::Shells, TEXT("GOTSHELLS") },
		{ EPickup::ShellBox, TEXT("GOTSHELLBOX") }, { EPickup::Rocket, TEXT("GOTROCKET") }, { EPickup::RocketBox, TEXT("GOTROCKBOX") },
		{ EPickup::Cell, TEXT("GOTCELL") }, { EPickup::CellPack, TEXT("GOTCELLBOX") }, { EPickup::Backpack, TEXT("GOTBACKPACK") },
		{ EPickup::Chainsaw, TEXT("GOTCHAINSAW") }, { EPickup::Shotgun, TEXT("GOTSHOTGUN") }, { EPickup::SuperShotgun, TEXT("GOTSHOTGUN2") },
		{ EPickup::Chaingun, TEXT("GOTCHAINGUN") }, { EPickup::RocketLauncher, TEXT("GOTLAUNCHER") }, { EPickup::Plasma, TEXT("GOTPLASMA") },
		{ EPickup::BFG, TEXT("GOTBFG9000") } };
	const TCHAR* const* M = Mnemonics.Find(P);
	return M ? Text(*M, DefaultPickupMessage(P)) : DefaultPickupMessage(P);
}

static FString DefaultPickupMessage(EPickup P)
{
	switch (P)
	{
	case EPickup::HealthBonus: return TEXT("Health bonus.");
	case EPickup::ArmorBonus: return TEXT("Armor bonus.");
	case EPickup::Soulsphere: return TEXT("Soul sphere!");
	case EPickup::Megasphere: return TEXT("Megasphere!");
	case EPickup::GreenArmor: return TEXT("Picked up armor.");
	case EPickup::BlueArmor: return TEXT("Picked up heavy armor.");
	case EPickup::Invulnerability: return TEXT("Invulnerability!");
	case EPickup::Berserk: return TEXT("Berserk!");
	case EPickup::Invisibility: return TEXT("Partial invisibility.");
	case EPickup::RadSuit: return TEXT("Radiation suit.");
	case EPickup::Map: return TEXT("Computer area map.");
	case EPickup::LightAmp: return TEXT("Light amplification visor.");
	case EPickup::BlueCard: return TEXT("Picked up a blue keycard.");
	case EPickup::YellowCard: return TEXT("Picked up a yellow keycard.");
	case EPickup::RedCard: return TEXT("Picked up a red keycard.");
	case EPickup::BlueSkull: return TEXT("Picked up a blue skull key.");
	case EPickup::YellowSkull: return TEXT("Picked up a yellow skull key.");
	case EPickup::RedSkull: return TEXT("Picked up a red skull key.");
	case EPickup::Clip: return TEXT("Picked up a clip.");
	case EPickup::ClipBox: return TEXT("Picked up a box of bullets.");
	case EPickup::Shells: return TEXT("Picked up some shells.");
	case EPickup::ShellBox: return TEXT("Picked up a box of shells.");
	case EPickup::Rocket: return TEXT("Picked up a rocket.");
	case EPickup::RocketBox: return TEXT("Picked up a box of rockets.");
	case EPickup::Cell: return TEXT("Picked up an energy cell.");
	case EPickup::CellPack: return TEXT("Picked up an energy cell pack.");
	case EPickup::Backpack: return TEXT("Picked up a backpack.");
	case EPickup::Chainsaw: return TEXT("A chainsaw!");
	case EPickup::Shotgun: return TEXT("You got the shotgun!");
	case EPickup::SuperShotgun: return TEXT("You got the super shotgun!");
	case EPickup::Chaingun: return TEXT("You got the chaingun!");
	case EPickup::RocketLauncher: return TEXT("You got the rocket launcher!");
	case EPickup::Plasma: return TEXT("You got the plasma gun!");
	case EPickup::BFG: return TEXT("You got the BFG!");
	default: return TEXT("Picked up a health item.");
	}
}
